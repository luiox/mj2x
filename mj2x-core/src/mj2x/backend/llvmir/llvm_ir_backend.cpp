/// @file llvm_ir_backend.cpp
/// @brief Restricted textual LLVM IR backend：编译管线（验证/发射拆分到
///        llvm_ir_validation.cpp / llvm_ir_emitter.cpp / llvm_ir_helpers.cpp）。
#include "llvm_ir_backend.hpp"
#include "llvm_ir_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "libca/log/log_macros.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace llvmir;   // 类型工具 / 验证 / 发射器 / 共享 helper

using codegen::mangleMethod;
using RegisterTypes = llvmir::RegisterTypes;

bool supportsMethodDescriptor(const MethodNode& method, SSAType& resultType, std::string& reason)
{
    // 实例方法自 Phase 4 起支持：this 为 SSA 参数 0（REF），wrapper 第二参为
    // jobject this；实例内调用经 JNI 分派（CallXxxMethodA）。
    const std::string descriptor = method.desc.to_std_string();
    resultType = primitiveToSsaType(returnKind(descriptor));
    if (resultType == SSAType::UNKNOWN) {
        reason = "LLVM IR first slice only supports int/long/float/double/void returns";
        return false;
    }
    for (const PrimitiveKind kind : argumentKinds(descriptor)) {
        if (primitiveToSsaType(kind) == SSAType::UNKNOWN) {
            reason = "LLVM IR first slice only supports int/long/float/double parameters";
            return false;
        }
    }
    return true;
}

std::string helperName(const std::string& className, const MethodNode& method)
{
    return "mj2x_llvm_" + mangleMethod(className,
                                        method.name.to_std_string(),
                                        method.desc.to_std_string());
}

/// @brief 整方法 fail-closed 降级诊断（#1121）：降级决策保持不变，仅在命中时
/// 打警告让 native 保护面的消失可观测。口径对齐 RegC 的 "Skipping ..." 通道
/// （regc_backend 同款 target/格式），结果固定带 method falls back to bytecode。
void logMethodFallback(const std::string& className, const MethodNode& method,
                       const std::string& reason)
{
    CA_LOGT_WARN("llvm",
                 "Skipping {}.{}{}: {}: method falls back to bytecode\n",
                 className.c_str(),
                 method.name.to_std_string().c_str(),
                 method.desc.to_std_string().c_str(),
                 reason.c_str());
}

/// @brief 解析并预检 LLVM IR 切片内的静态互调目标（类声明见 internal.hpp）。
///
/// @details 目标须 INVOKESTATIC + 目标类在编译索引内（非 library）+ markedForCompile
/// + 纯标量签名 + 方法体可转换。跨类与递归（互递归）均允许：符号由 owner/name/desc
/// 确定性生成，环上各方法的非递归部分仍各自验证。
bool InternalCallResolver::resolve(const SSAMethodRef& method, CallTarget& target,
                                   std::string& reason)
{
    if (method.invokeOpcode != Opcodes::INVOKESTATIC) {
        reason = "LLVM IR first slice only supports static method calls";
        return false;
    }
    // 直调档位门控（与 RegC 的 allowsDirectCall 同一判定）：Off 档不直调，
    // IntraClass 档只放行同类目标。LLVM 侧只会走到 INVOKESTATIC，确定性虚调用
    // 的免门控分支不适用，故第三参恒 false。
    if (!codegen::allowsDirectCall(directCallTier_, method.owner == className_, false)) {
        reason = "direct call disabled by --regc-direct-calls tier";
        return false;
    }
    const std::string owner = method.owner.to_std_string();
    const MethodID id{owner, method.name.to_std_string(), method.desc.to_std_string()};
    const auto     cached = cache_.find(id);
    if (cached != cache_.end()) {
        target = cached->second;
        return true;
    }
    CompilerMethod* targetMethod = findTarget(id, reason);
    if (targetMethod == nullptr || targetMethod->methodNode == nullptr) return false;

    if (targetMethod->backendResult.kind == BackendResult::Kind::C_SOURCE) {
        // 已编译成功：方法体已被 injectIntoClass 清空，不能再预检；成功即已通过
        // 完整验证，签名直接从描述解析。
        CallTarget parsed;
        if (!fillTargetFromDescriptor(owner, *targetMethod->methodNode, parsed, reason))
            return false;
        cache_.emplace(id, parsed);
        target = parsed;
        return true;
    }
    if (!targetMethod->backendResult.text.empty()) {
        // 已尝试编译且 fail-closed：调用方走 JNI fallback。
        reason = "LLVM IR call target failed to compile";
        return false;
    }

    // 递归/互递归：目标仍在预检中（visiting）。符号确定性生成不会悬空，按描述
    // 接受即可；环上各方法的非递归部分由各自的 analyze 负责验证。
    if (!visiting_.insert(id).second) {
        CallTarget parsed;
        if (!fillTargetFromDescriptor(owner, *targetMethod->methodNode, parsed, reason))
            return false;
        cache_.emplace(id, parsed);
        target = parsed;
        return true;
    }

    // 未编译且非递归：字节码完好，完整预检（含嵌套调用）。
    if (!analyze(owner, *targetMethod->methodNode, target, reason)) {
        visiting_.erase(id);
        return false;
    }
    visiting_.erase(id);
    cache_.emplace(id, target);
    return true;
}

CompilerMethod* InternalCallResolver::findTarget(const MethodID& id, std::string& reason)
{
    CompilerClass* clazz = index_.getClass(id.owner);
    if (clazz == nullptr) {
        reason = "LLVM IR call target class is not in the compilation index";
        return nullptr;
    }
    if (clazz->isLibrary) {
        reason = "LLVM IR call target class is a library class and is not compiled";
        return nullptr;
    }
    for (auto& candidate : clazz->methods) {
        if (!candidate.markedForCompile || candidate.methodNode == nullptr) continue;
        if (candidate.methodNode->name.to_std_string() == id.name &&
            candidate.methodNode->desc.to_std_string() == id.desc) {
            return &candidate;
        }
    }
    reason = "LLVM IR call target is not a marked-for-compile method in this class";
    return nullptr;
}

bool InternalCallResolver::fillTargetFromDescriptor(const std::string& owner, MethodNode& node,
                                                    CallTarget& target, std::string& reason)
{
    SSAType resultType = SSAType::UNKNOWN;
    if (!supportsMethodDescriptor(node, resultType, reason)) return false;
    target.symbol     = helperName(owner, node);
    target.returnType = resultType;
    for (const PrimitiveKind kind : argumentKinds(node.desc.to_std_string())) {
        target.argTypes.push_back(primitiveToSsaType(kind));
    }
    return true;
}

/// @brief 目标方法体级预检（不发射）：复用主编译链的 SSA/RegIR 验证。
/// @param owner 目标方法所属类（跨类调用时符号与 SSA 上下文使用目标类名）。
bool InternalCallResolver::analyze(const std::string& owner, MethodNode& node, CallTarget& target,
                                   std::string& reason)
{
    SSAType resultType = SSAType::UNKNOWN;
    if (!supportsMethodDescriptor(node, resultType, reason)) return false;
    CodeConverter converter(index_.arena());
    auto          blocks = converter.convert(node);
    SSABuilder    builder;
    ca::str::Utf8StringArena ssaArena;   // SSA 派生串入池，活到 ssa 消费完（本函数内同步）
    auto ssa = builder.build(blocks,
                             owner,
                             node.name.to_std_string(),
                             node.desc.to_std_string(),
                             true,
                             node.maxLocals,
                             ssaArena,
                             reason);
    if (!ssa) return false;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return false;
    RegisterTypes types;
    if (!collectRegisterTypes(*reg, types, this, reason)) return false;
    target.symbol     = helperName(owner, node);
    target.returnType = resultType;
    for (const PrimitiveKind kind : argumentKinds(node.desc.to_std_string())) {
        target.argTypes.push_back(primitiveToSsaType(kind));
    }
    return true;
}

/// @brief helper 参数类型：JVM 栈位宽（窄类型提升为 i32）。
const char* calcArgType(PrimitiveKind kind)
{
    return llvmType(primitiveToSsaType(kind));
}

std::string emitJniWrapper(const std::string& className, const MethodNode& method,
                           const std::string& helper, SSAType /*returnType*/, bool isStatic)
{
    const std::string             descriptor = method.desc.to_std_string();
    const std::vector<PrimitiveKind> arguments = argumentKinds(descriptor);
    const PrimitiveKind          retKind   = returnKind(descriptor);
    const char*                  boundaryRet = jniBoundaryType(retKind);
    const char*                  calcRet    = llvmType(primitiveToSsaType(retKind));
    std::ostringstream           out;
    // JNI native 签名：静态方法第二参为 jclass；实例方法为 jobject this。
    out << "define dllexport " << boundaryRet << " @"
        << mangleMethod(className, method.name.to_std_string(), descriptor) << "(ptr %env, ptr "
        << (isStatic ? "%clazz" : "%this");
    for (size_t index = 0; index < arguments.size(); ++index) {
        out << ", " << jniBoundaryType(arguments[index]) << " %p" << index;
    }
    out << ") {\nentry:\n";
    // 窄参数提升到 i32 计算位宽（byte/short 符号扩展，boolean/char 零扩展）。
    for (size_t index = 0; index < arguments.size(); ++index) {
        const PrimitiveKind kind  = arguments[index];
        const std::string   btype = jniBoundaryType(kind);
        if (btype == "i8" || btype == "i16") {
            const std::string ext = (kind == PrimitiveKind::BYTE || kind == PrimitiveKind::SHORT)
                                        ? "sext"
                                        : "zext";
            out << "  %w" << index << " = " << ext << " " << btype << " %p" << index
                << " to i32\n";
        }
    }
    if (retKind == PrimitiveKind::VOID) {
        out << "  call void @" << helper << "(ptr %env";
    }
    else {
        out << "  %result = call " << calcRet << " @" << helper << "(ptr %env";
    }
    if (!isStatic) {
        // 实例方法：this 是 SSA 参数 0（REF），直接透传。
        out << ", ptr %this";
    }
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::string btype = jniBoundaryType(arguments[index]);
        const bool        narrow = btype == "i8" || btype == "i16";
        out << ", " << calcArgType(arguments[index]) << " "
            << (narrow ? ("%w" + std::to_string(index)) : ("%p" + std::to_string(index)));
    }
    out << ")\n";
    if (retKind == PrimitiveKind::VOID) {
        out << "  ret void\n";
    }
    else if (std::string(boundaryRet) == "i8" || std::string(boundaryRet) == "i16") {
        out << "  %t = trunc i32 %result to " << boundaryRet << "\n";
        out << "  ret " << boundaryRet << " %t\n";
    }
    else {
        out << "  ret " << boundaryRet << " %result\n";
    }
    out << "}\n\n";
    return out.str();
}

struct LlvmIrBackend::Impl
{
    std::ostringstream bodies;
    GlobalStringTable  strings;
    // 文件级 invokedynamic 站点表（跨方法累积；发射端按 base+local 索引）。
    std::vector<RegFunction::InvokeDynamicSite> indySites;
    // 模块级检查点状态全局（--runtime-protection；函数级累积，finalize 统一输出）。
    std::string checkpointGlobals;
    // 检查点全局命名计数器（跨函数唯一）。
    uint32_t checkpointCounter = 0;
    // JDK intrinsic 直调 declare（模块级输出，跨函数去重累积）。
    std::string runtimeDeclares;
    // 调用目标间接化站点表（模块级累积，finalize 统一输出）。
    LlvmIndirectSiteRegistry indirectSites;
};

LlvmIrBackend::LlvmIrBackend()
    : impl_(std::make_unique<Impl>())
{}

LlvmIrBackend::~LlvmIrBackend() = default;

/// @brief 当前类是否在混淆作用域内（issue #8，与 RegC 同语义）。
bool LlvmIrBackend::currentClassInObfuscationScope(const std::string& className,
                                                  CompilerIndex* index)
{
    if (index == nullptr) return true;
    CompilerClass* clazz = index->getClass(className);
    return clazz == nullptr ? true : clazz->inObfuscationScope;
}

BackendResult LlvmIrBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                           const std::vector<Block*>& blocks,
                                           CompilerIndex* index)
{
    BackendResult result;
    if (method.methodNode == nullptr) {
        result.text = "LLVM IR lowering failed: missing method node";
        return result;
    }

    SSAType     resultType = SSAType::UNKNOWN;
    std::string reason;
    if (!supportsMethodDescriptor(*method.methodNode, resultType, reason)) {
        logMethodFallback(className, *method.methodNode, reason);
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    const bool isStatic = (method.methodNode->access & Opcodes::ACC_STATIC) != 0;
    // 互调需要编译器索引（单元测试不传 index 时行为与以前一致：静态调用走 JNI）。
    std::unique_ptr<InternalCallResolver> callResolver;
    if (index != nullptr) {
        callResolver = std::make_unique<InternalCallResolver>(*index, className, directCallTier_);
    }

    // SSA → RegIR → 验证（每轮 fresh builder/lowering：变换失败需回退未变换形态，
    // RegIR 变换是就地修改，不能复用同一份）。
    // SSA 派生串（如 ANEWARRAY 的 "[L...;" 数组描述符）入池后由 RegFunction 的
    // Utf8StringRef 视图引用，arena 必须活到本方法发射完成（曾因 arena 是 lambda
    // 局部、返回即析构，导致发射期读到被复用内存的垃圾描述符）。
    ca::str::Utf8StringArena ssaArena;
    auto lowerAndValidate = [&](std::unique_ptr<RegFunction>& out, RegisterTypes& typesOut,
                                std::string& r) -> bool {
        SSABuilder     freshBuilder;
        auto           freshSsa = freshBuilder.build(blocks,
                                                     className,
                                                     method.methodNode->name.to_std_string(),
                                                     method.methodNode->desc.to_std_string(),
                                                     isStatic,
                                                     method.methodNode->maxLocals,
                                                     ssaArena,
                                                     r);
        if (!freshSsa) return false;
        RegLowering freshLowering;
        auto        freshReg = freshLowering.lower(*freshSsa, r);
        if (!freshReg) return false;
        RegisterTypes types;
        if (!collectRegisterTypes(*freshReg, types, callResolver.get(), r)) return false;
        out      = std::move(freshReg);
        typesOut = std::move(types);
        return true;
    };

    std::unique_ptr<RegFunction> reg;
    RegisterTypes                registerTypes;
    if (!lowerAndValidate(reg, registerTypes, reason)) {
        // #1121：SSA/RegIR/验证层 fail-closed 在此整方法降级——此前静默（undef
        // 来路边拷贝自方向 1 起放行，不再触发；诊断通道由真 fail-closed 形态
        // 覆盖，见 unittest RetainsFallbackDiagnosticForUndefinedLocalRead）。
        logMethodFallback(className, *method.methodNode, reason);
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    // R4 保护变换（--regc-obfuscate 等；RegIR 级，与 RegC 同源）：变换后重新
    // 验证；被拒则回退未变换形态（LLVM 子集窄于 RegC，变换不应丢方法）。
    if (transformOptions_.enableMba || transformOptions_.enableOpaquePredicates ||
        transformOptions_.enableControlFlowFlattening ||
        transformOptions_.hideImmediateConstants || transformOptions_.indirectCallTargets) {
        std::unique_ptr<RegFunction> transformedReg;
        RegisterTypes                transformedTypes;
        if (!lowerAndValidate(transformedReg, transformedTypes, reason)) {
            logMethodFallback(className, *method.methodNode, reason);
            result.text = "LLVM IR lowering failed: " + reason;
            return result;
        }
        // 混淆作用域（issue #8）：与 RegC 同语义。
        if (currentClassInObfuscationScope(className, index)) {
            applyRegTransformPasses(*transformedReg, transformOptions_);
        }
        RegisterTypes validatedTypes;
        if (collectRegisterTypes(*transformedReg, validatedTypes, callResolver.get(), reason)) {
            reg            = std::move(transformedReg);
            registerTypes  = std::move(validatedTypes);
        }
        else {
            CA_LOGT_WARN("llvm",
                         "{}.{}{}: transform output rejected ({}); using untransformed "
                         "IR\n",
                         className.c_str(),
                         method.methodNode->name.to_std_string().c_str(),
                         method.methodNode->desc.to_std_string().c_str(),
                         reason.c_str());
        }
    }

    // invokedynamic 站点：方法局部索引 → 文件级全局索引（bsm helper 名）。
    const size_t indySiteBase = impl_->indySites.size();
    impl_->indySites.insert(impl_->indySites.end(), reg->indySites.begin(), reg->indySites.end());

    const std::string helper = helperName(className, *method.methodNode);
    std::string       helperSource;
    std::vector<std::string> checkpointGlobals;
    std::vector<std::string> runtimeDeclares;
    if (!emitLlvmFunction(*reg, registerTypes, helper, resultType, callResolver.get(),
                          impl_->strings, indySiteBase,
                          runtimeProtection_.enabled() ? &runtimeProtection_ : nullptr,
                          &impl_->checkpointCounter, &checkpointGlobals, &runtimeDeclares,
                          helperSource, reason, &impl_->indirectSites)) {
        logMethodFallback(className, *method.methodNode, reason);
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = helperSource +
                  emitJniWrapper(className, *method.methodNode, helper, resultType,
                                 isStatic);
    impl_->bodies << result.text;
    if (!impl_->checkpointGlobals.empty()) impl_->checkpointGlobals += "\n";
    for (const auto& global : checkpointGlobals) {
        impl_->checkpointGlobals += global + "\n";
    }
    // intrinsic 直调声明：模块级去重累积（finalize 统一输出）。
    for (const auto& declare : runtimeDeclares) {
        if (impl_->runtimeDeclares.find(declare) == std::string::npos) {
            impl_->runtimeDeclares += declare + "\n";
        }
    }
    return result;
}

void LlvmIrBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                    const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::C_SOURCE || method.methodNode == nullptr) return;
    method.methodNode->access |= Opcodes::ACC_NATIVE;
    method.methodNode->instructions.clear();
    // native 方法无字节码：maxStack/maxLocals 清零，对齐 StackC/VMC4。
    method.methodNode->maxStack  = 0;
    method.methodNode->maxLocals = 0;
    clazz.modified = true;
}

void LlvmIrBackend::finalize(const CompilerIndex&)
{
    std::ostringstream full;
    full << "; ModuleID = 'mj2x_llvm'\n";
    full << "source_filename = \"mj2x_llvm\"\n\n";
    if (runtimeProtection_.enabled()) {
        full << emitRuntimeProtectionDeclarations() << "\n";
    }
    const std::string strings = impl_->strings.emitDefinitions();
    full << strings;
    if (!strings.empty()) full << "\n";
    full << impl_->runtimeDeclares;
    if (!impl_->runtimeDeclares.empty()) full << "\n";
    // 调用目标间接化站点表：结构类型须早于任何表定义与 dispatcher 调用。
    if (!impl_->indirectSites.empty()) {
        full << LlvmIndirectSiteRegistry::typeDeclaration() << "\n";
        full << impl_->indirectSites.emitDefinitions() << "\n";
    }
    full << impl_->checkpointGlobals;
    full << impl_->bodies.str();
    const std::string bodies = impl_->bodies.str();
    if (bodies.find("mj2x_llvm_throw_arith") != std::string::npos) {
        full << emitThrowArithmeticHelper();
    }
    if (bodies.find("mj2x_llvm_field_load_") != std::string::npos ||
        bodies.find("mj2x_llvm_field_store_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_load_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_store_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_length") != std::string::npos ||
        bodies.find("mj2x_llvm_call_") != std::string::npos ||
        bodies.find("mj2x_llvm_new_object") != std::string::npos ||
        bodies.find("mj2x_llvm_new_array") != std::string::npos ||
        bodies.find("mj2x_llvm_new_multi_array") != std::string::npos ||
        bodies.find("mj2x_llvm_check_cast") != std::string::npos ||
        bodies.find("mj2x_llvm_instance_of") != std::string::npos ||
        bodies.find("mj2x_llvm_exception_") != std::string::npos ||
        bodies.find("mj2x_llvm_throw(") != std::string::npos ||
        bodies.find("mj2x_llvm_string_const") != std::string::npos ||
        bodies.find("mj2x_llvm_class_const") != std::string::npos ||
        bodies.find("mj2x_llvm_ref_eq") != std::string::npos ||
        bodies.find("mj2x_llvm_monitor_") != std::string::npos ||
        bodies.find("mj2x_llvm_indy_bsm_") != std::string::npos) {
        full << emitObjectHelpers();
    }
    if (!impl_->indySites.empty()) {
        full << emitIndyHelpers();
        for (size_t index = 0; index < impl_->indySites.size(); ++index) {
            full << emitIndyBootstrapFunction(index, impl_->indySites[index]);
        }
    }
    sourceCode_ = full.str();
    if (llvmOutputPath_.empty()) return;

    std::ofstream output(llvmOutputPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        CA_LOGT_ERROR("llvm", "cannot open output: {}\n", llvmOutputPath_.c_str());
        return;
    }
    output.write(sourceCode_.data(), static_cast<std::streamsize>(sourceCode_.size()));
    if (!output) {
        CA_LOGT_ERROR("llvm", "cannot write output: {}\n", llvmOutputPath_.c_str());
    }
}

}   // namespace mj2x

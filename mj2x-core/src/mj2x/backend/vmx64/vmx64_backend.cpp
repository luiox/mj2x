#include "mj2x/backend/vmx64/vmx64_backend.hpp"

#include "mj2x/core/jvm_opcodes.hpp"

#include "mj2x/backend/dom_inject.hpp"
#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/backend/vmx64/vmx64_encoder.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/postprocessor/cloader_call.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/log/log_macros.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {

namespace {

using codegen::CSourceStream;
using codegen::cStringLiteral;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::registerFunctionName;

}   // namespace

// ===========================================================================
// Vmx64Backend::Impl：方法段累积 + RegisterNatives 簿记（镜像
// Vmc4Backend::Impl 的同名结构；差异仅在「fn 指针是 .asm 入口符号的 extern
// 声明」而非同 TU 的 static C 函数）。
// ===========================================================================
struct Vmx64Backend::Impl
{
    ca::str::Utf8StringBuilder source;

    /// RegisterNatives 表条目：Java name/desc + .asm 入口符号 + extern 原型
    /// 所需的参数/返回 JNI 类型（jint/jlong/void——Phase 2 最小 opcode 面）。
    struct NativeMethodReg
    {
        std::string javaName;
        std::string javaDesc;
        std::string asmSymbol;   ///< mj2x_vmx64_exec_<tag>（.asm 内 PUBLIC）
        std::vector<bool> argIs64;
        bool retIs64      = false;
        bool returnsValue = false;
    };
    std::unordered_map<std::string, std::vector<NativeMethodReg>> regsByClass;
    std::vector<std::string>                                      classOrder;
    std::unordered_map<std::string, int>                         classIds;
    std::unordered_set<std::string>                              clinitInjected;

    /// 已虚拟化方法计数（日志/diagnostics 用）。
    size_t virtualizedMethods = 0;

    int registerNativeMethod(const std::string& className, const std::string& javaName,
                             const std::string& javaDesc, const std::string& asmSymbol,
                             const Vmx64Program& program)
    {
        auto [regsIt, insertedClass] = regsByClass.emplace(className, std::vector<NativeMethodReg>{});
        if (insertedClass) {
            classIds.emplace(className, static_cast<int>(classOrder.size()));
            classOrder.push_back(className);
        }
        NativeMethodReg reg;
        reg.javaName     = javaName;
        reg.javaDesc     = javaDesc;
        reg.asmSymbol    = asmSymbol;
        for (const auto& arg : program.args) reg.argIs64.push_back(arg.is64);
        reg.retIs64      = program.retIs64;
        reg.returnsValue = program.returnsValue;
        regsIt->second.push_back(std::move(reg));
        return classIds[className];
    }

    /// 组装注册 C TU（extern 声明 + per-class register 函数 + 桥接 + JNI_OnLoad）。
    void emitRegistrationCode(ca::str::Utf8StringBuilder& out) const;
};

Vmx64Backend::Vmx64Backend()
    : impl_(std::make_unique<Impl>())
{}

Vmx64Backend::~Vmx64Backend() = default;

// ===========================================================================
// compileMethod：与 VMC4 同前端（L1 → SSA → RegIR → DCE），分叉点换
// Vmx64Encoder + Vmx64AsmEmitter。fail-closed：任一阶段失败保留字节码。
// ===========================================================================
BackendResult Vmx64Backend::compileMethod(const std::string& className, CompilerMethod& method,
                                          const std::vector<Block*>& blocks, CompilerIndex* index)
{
    (void)index;
    BackendResult     backendResult;
    const std::string methodName = method.name;
    const std::string methodDesc = method.desc;

    // 构造器/类初始化器不直接标 ACC_NATIVE（JVM 部分实现拒收），不转
    //（<clinit> 只承接 RegisterNatives 注入，见 injectIntoClass）。
    if (methodName == "<init>" || methodName == "<clinit>") {
        CA_LOGT_WARN("vmx64", "Skipping {}.{}{}: constructor/<clinit> not virtualized\n",
                     className.c_str(), methodName.c_str(), methodDesc.c_str());
        return backendResult;
    }

    // --- SSA 构建（与 RegC/VMC4 共享前端）---
    SSABuilder              ssaBuilder;
    ca::str::Utf8StringArena ssaArena;
    std::string             reason;
    const bool              isStatic = (method.access & Opcodes::ACC_STATIC) != 0;
    auto ssa = ssaBuilder.build(blocks, className, methodName, methodDesc, isStatic, method.maxLocals,
                                ssaArena, reason);
    if (!ssa) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: SSA build failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // --- RegIR lowering + DCE ---
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: lowering failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    // 不调用 applyRegTransformPasses：与 VMC4 同口径（MBA/CFF 是 RegC 的发射期
    // 混淆；vmx64 的混淆本体是 opcode 编码 + 加密跳转链，不需要在 IR 内再套）。

    // --- vmx64 分叉点：RegIR → 最小 opcode 流 ---
    auto program = Vmx64Encoder::encode(*reg, reason);
    if (!program) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: vmx64 encode failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // 方法级 tag（fnv1a64 hex，保证多方法符号命名唯一）与实例 seed 派生。
    const std::string methodId     = className + "." + methodName + methodDesc;
    const std::string methodTag    = hex64(fnv1a64(methodId));
    const uint64_t    instanceSeed = vmx64DeriveInstanceSeed(options_.seed, fnv1a64(methodId));

    // --- 发射 .asm 方法段（加密流 + 加密站点表 + JNI 入口 PROC）---
    backendResult.kind = BackendResult::Kind::ASM_SOURCE;
    backendResult.text = Vmx64AsmEmitter::emitMethod(*program, instanceSeed, methodTag);

    // --- 登记 RegisterNatives（Phase 2）：fn 指针是 .asm 入口符号本身 ---
    impl_->registerNativeMethod(className, methodName, methodDesc,
                                "mj2x_vmx64_exec_" + methodTag, *program);

    impl_->virtualizedMethods++;
    impl_->source.append(backendResult.text.data(), backendResult.text.size());
    return backendResult;
}

// ===========================================================================
// injectIntoClass：成功产物形态 = ASM_SOURCE 时标 ACC_NATIVE + clinit 注入
// （镜像 vmc4_backend.cpp 的 dom_inject 路径；注册面在注册 C TU）。
// ===========================================================================
void Vmx64Backend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                   const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::ASM_SOURCE) return;

    if (clazz.dom == nullptr) {
        throw std::runtime_error("vmx64_backend: DOM not synced for native injection");
    }
    const std::string className(clazz.dom->thisName());
    dom_inject::markMethodNative(clazz, className, method.name, method.desc);

    if (className.empty()) return;
    if (impl_->classIds.find(className) != impl_->classIds.end() &&
        impl_->clinitInjected.find(className) == impl_->clinitInjected.end()) {
        dom_inject::prependClinitRegisterCall(clazz, className, impl_->classIds[className],
                                              cloader::kClassName, cloader::kRegisterName,
                                              cloader::kRegisterDesc);
        impl_->clinitInjected.insert(className);
    }
}

// ===========================================================================
// finalize：组装 .asm TU（头注释 + 共享 handler 集 + 全部方法段 + end）与
// RegisterNatives 绑定 C TU（第二产物，CLI 写进生成工程与 .asm 一并编译）。
// ===========================================================================
void Vmx64Backend::finalize(const CompilerIndex& index)
{
    (void)index;
    resetDiagnostics();

    std::string full;
    full += "; ----------------------------------------------------------------------------\n";
    full += "; Generated by mj2x Vmx64Backend (MASM x64, ml64)\n";
    full += ";\n";
    full += "; Deterministic build: same seed + same input -> byte-identical .asm.\n";
    full += "; Windows x64 only. Assemble with: ml64 /c vmx64.asm\n";
    full += ";\n";
    full += "; Structure: vmx64_NEXT dispatch macro + shared handlers (jmp rdx chain),\n";
    full += "; then per method: encrypted opcode stream + encrypted handler site table\n";
    full += "; (.data) and a JNI native entry PROC (vreg slots on the stack frame).\n";
    full += ";\n";
    full += "; JNI mixed-linking boundary: entries receive (env, thisOrClass, args) per\n";
    full += "; JNI ABI and home env/this into frame slots; the minimal opcode subset\n";
    full += "; never calls out to C. RegisterNatives binding lives in the companion C\n";
    full += "; TU (vmx64_reg.c) emitted by the backend as a second artifact.\n";
    full += "; ----------------------------------------------------------------------------\n";
    full += "\n";
    full += Vmx64AsmEmitter::emitSharedHandlers();

    const std::string methods = impl_->source.build().to_std_string();
    full += methods;
    // MASM 源文件终止指令：ml64 参考用法均以 end 收尾（其后文本忽略），
    // 缺失在部分 MASM 前端是硬错误——发射器确定性补齐。
    full += "end\n";

    sourceCode_ = std::move(full);
    if (!asmOutputPath_.empty()) {
        std::ofstream ofs(asmOutputPath_, std::ios::binary);
        if (ofs.is_open()) {
            ofs << sourceCode_;
            ofs.flush();
            if (!ofs.good())
                CA_LOGT_ERROR("vmx64", "finalize: short write to {}\n", asmOutputPath_);
        }
    }

    // 第二产物：RegisterNatives 绑定 C TU。无虚拟化方法时保持空串
    //（fail-closed：不产空注册面，也不产只有 .asm 的可链接半成品工程）。
    registrationCode_.clear();
    if (impl_->virtualizedMethods > 0u) {
        codegen::IncludeSet includes;
        includes.addSystem("jni.h");
        includes.addSystem("stdint.h");

        ca::str::Utf8StringBuilder reg;
        reg.append("/* Generated by mj2x Vmx64Backend (RegisterNatives binding TU) */\n");
        const std::string renderedIncludes = includes.render();
        reg.append(renderedIncludes.data(), renderedIncludes.size());
        reg.append("\n", 1);
        impl_->emitRegistrationCode(reg);
        registrationCode_ = reg.build().to_std_string();
    } else {
        CA_LOGT_WARN("vmx64", "finalize: no method virtualized (all inputs fail-closed or empty)\n");
    }
}

// ===========================================================================
// emitRegistrationCode：结构镜像 Vmc4Backend::Impl::emitRegistrationCode；
// 差异：native 函数本体在 .asm TU（PUBLIC PROC，MS x64 ABI 与 JNI 签名一致），
// 此处仅 extern 声明符号并取址入 RegisterNatives 表。
// ===========================================================================
void Vmx64Backend::Impl::emitRegistrationCode(ca::str::Utf8StringBuilder& out) const
{
    CSourceStream src;
    src.emitLine("/* Native registration (RegisterNatives, lazy per-class).");
    src.emitLine(" * Native bodies are MASM x64 PROCs in the companion .asm TU");
    src.emitLine(" * (public mj2x_vmx64_exec_<tag>); this TU only binds them. */");

    // extern 声明：jni.h 类型拼原型（jint/jlong/void，Phase 2 最小 opcode 面）。
    // 第二参恒写 jclass：当前实例方法因 this(REF) 在编码器 fail-closed 不可达
    //（静态方法收 jclass），到场的全是静态方法；将来放开实例方法时此参应为
    // jobject（JNI ABI 的 thisOrClass 双形态，.asm 入口按 rdx 原样接收不受
    // 影响），需回改此声明。
    for (const auto& className : classOrder) {
        auto found = regsByClass.find(className);
        if (found == regsByClass.end()) continue;
        for (const auto& reg : found->second) {
            std::string decl = std::string("extern ") +
                               (reg.returnsValue ? (reg.retIs64 ? "jlong" : "jint") : "void");
            decl += " " + reg.asmSymbol + "(JNIEnv*, jclass";
            for (bool is64 : reg.argIs64) decl += std::string(", ") + (is64 ? "jlong" : "jint");
            decl += ");";
            src.emitLine(decl);
        }
    }
    src.emitLine("");

    src.emitLine("typedef jint (*_mj2x_reg_method)(JNIEnv* env, jclass clazz);");
    src.emitLine(
        "static _mj2x_reg_method _mj2x_reg_methods[" +
        std::to_string(classOrder.empty() ? 1 : classOrder.size()) + "] = {0};");
    src.emitLine("");

    for (const auto& className : classOrder) {
        auto found = regsByClass.find(className);
        if (found == regsByClass.end() || found->second.empty()) continue;

        const auto registerName = registerFunctionName(className);
        src.emitLine("static jint " + registerName + "(JNIEnv* env, jclass clazz) {");
        src.indent();
        src.emitLine("if (clazz == NULL) return JNI_ERR;");
        src.emitLine("if ((*env)->MonitorEnter(env, clazz) != 0) return JNI_ERR;");
        src.emitLine("jint result = JNI_OK;");
        src.emitLine("static JNINativeMethod methods[] = {");
        src.indent();
        for (const auto& reg : found->second) {
            src.emitLine("{(char*)" + cStringLiteral(reg.javaName) + ", (char*)" +
                         cStringLiteral(reg.javaDesc) + ", (void*)" + reg.asmSymbol + "},");
        }
        src.dedent();
        src.emitLine("};");
        src.emitLine("if ((*env)->RegisterNatives(env, clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0) {");
        src.indent();
        src.emitLine("result = JNI_ERR;");
        src.emitLine("goto " + registerName + "_exit;");
        src.dedent();
        src.emitLine("}");
        src.emitLine(registerName + "_exit:");
        src.emitLine(
            "if ((*env)->MonitorExit(env, clazz) != 0 && result == JNI_OK) result = JNI_ERR;");
        src.emitLine("return result;");
        src.dedent();
        src.emitLine("}");
        src.emitLine("");
    }

    // 桥接（与 regc/vmc4 同一契约：Loader.registerNativesForClass → 本函数）。
    src.emitLine(std::string("static void JNICALL ") + cloader::kBridgeCFunc +
                 "(JNIEnv* env, jclass loader_class, jint class_id, jclass clazz) {");
    src.indent();
    src.emitLine("(void)loader_class;");
    src.emitLine("if (class_id < 0 || class_id >= " + std::to_string(classOrder.size()) +
                 " || _mj2x_reg_methods[class_id] == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class id mismatch\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (clazz == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class is null\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (_mj2x_reg_methods[class_id](env, clazz) != JNI_OK) {");
    src.indent();
    src.emitLine("if (!(*env)->ExceptionCheck(env)) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native registration failed\");");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    // JNI_OnLoad：登记逐类 register 函数并绑定 Loader 桥（vmx64 无 at-rest
    // 解密面——流/表加密是链接期可表达的重定位加法，无需加载期原语）。
    src.emitLine("JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {");
    src.indent();
    src.emitLine("JNIEnv* env = NULL;");
    src.emitLine("(void)reserved;");
    src.emitLine("if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;");
    for (const auto& className : classOrder) {
        auto found = regsByClass.find(className);
        if (found == regsByClass.end() || found->second.empty()) continue;
        auto idIt = classIds.find(className);
        if (idIt == classIds.end()) continue;
        src.emitLine("_mj2x_reg_methods[" + std::to_string(idIt->second) + "] = " +
                     registerFunctionName(className) + ";");
    }
    src.emitLine("jclass loader_class = (*env)->FindClass(env, " +
                 cStringLiteral(cloader::kClassName) + ");");
    src.emitLine("if ((*env)->ExceptionCheck(env) || loader_class == NULL) return JNI_ERR;");
    src.emitLine("static JNINativeMethod loader_methods[] = {");
    src.indent();
    src.emitLine("{(char*)" + cStringLiteral(cloader::kRegisterName) + ", (char*)" +
                 cStringLiteral(cloader::kRegisterDesc) + ", (void*)" + cloader::kBridgeCFunc +
                 "},");
    src.dedent();
    src.emitLine("};");
    src.emitLine("if ((*env)->RegisterNatives(env, loader_class, loader_methods, sizeof(loader_methods) / sizeof(loader_methods[0])) != 0) return JNI_ERR;");
    src.emitLine("(*env)->DeleteLocalRef(env, loader_class);");
    src.emitLine("return JNI_VERSION_1_6;");
    src.dedent();
    src.emitLine("}");

    const std::string rendered = src.str();
    out.append(rendered.data(), rendered.size());
}

}   // namespace mj2x

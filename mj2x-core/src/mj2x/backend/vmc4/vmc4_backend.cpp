#include "mj2x/backend/vmc4/vmc4_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_encoder.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/postprocessor/cloader_call.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <algorithm>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::nativeFunctionName;
using codegen::registerFunctionName;
using codegen::cStringLiteral;

// ---------------------------------------------------------------------------
// 类型映射 helper（镜像 regc_backend.cpp 的同名函数，VMC4 不依赖 RegC CRT）。
// ---------------------------------------------------------------------------

const char* jniType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "jboolean";
    case PrimitiveKind::BYTE: return "jbyte";
    case PrimitiveKind::CHAR: return "jchar";
    case PrimitiveKind::SHORT: return "jshort";
    case PrimitiveKind::INT: return "jint";
    case PrimitiveKind::LONG: return "jlong";
    case PrimitiveKind::FLOAT: return "jfloat";
    case PrimitiveKind::DOUBLE: return "jdouble";
    case PrimitiveKind::VOID: return "void";
    default: return "jobject";
    }
}

const char* defaultReturnValue(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::VOID: return "";
    case PrimitiveKind::FLOAT: return "0.0f";
    case PrimitiveKind::DOUBLE: return "0.0";
    case PrimitiveKind::OBJECT: return "NULL";
    default: return "0";
    }
}

PrimitiveKind returnKind(const std::string& desc)
{
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str())).getDescriptor());
}

std::vector<PrimitiveKind> argumentKinds(const std::string& desc)
{
    std::vector<PrimitiveKind> kinds;
    auto methodType = Type::getMethodType(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    for (const auto& argType : methodType.getArgumentTypes()) {
        kinds.push_back(jvmSortToPrimitive(argType.getSort()));
    }
    return kinds;
}

/// @brief 把参数/返回 PrimitiveKind 映射到 VM 寄存器槽的 C 表达式。
///
/// @details
/// JNI wrapper 把 jxxx 参数写进 mj2x_vm_slot（联合体），解释器消费 regs[]。
/// I32/I64 写 .i，F32/F64 写 .f，OBJECT 写 .ref（承载 jobject，Phase 2.0 起）。
const char* slotField(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::FLOAT:
    case PrimitiveKind::DOUBLE: return ".f";
    case PrimitiveKind::OBJECT: return ".ref";
    default: return ".i";
    }
}

/// 返回/参数是 VM 支持类型（标量或对象引用）时，VMC4 JNI wrapper 能正确转接。
/// OBJECT 自 Phase 2.0 起支持（slot.ref 承载 jobject）；具体对象操作（field/call）
/// 由 encoder 决定能否编码，不能则 fail-closed。
bool isVmCompatibleKind(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN:
    case PrimitiveKind::BYTE:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::SHORT:
    case PrimitiveKind::INT:
    case PrimitiveKind::LONG:
    case PrimitiveKind::FLOAT:
    case PrimitiveKind::DOUBLE:
    case PrimitiveKind::VOID:
    case PrimitiveKind::OBJECT: return true;
    default: return false;
    }
}

MethodNode* findMethod(CompilerClass& clazz, const char* name, const char* desc)
{
    for (auto* method : clazz.classNode->methods) {
        if (method->name.to_std_string() == name && method->desc.to_std_string() == desc) {
            return method;
        }
    }
    return nullptr;
}

/// 往 clinit 头部前插 registerNativesForClass(classId, thisClass) 调用（镜像 regc）。
void prependRegisterNativesCall(CompilerClass& clazz, int classId)
{
    constexpr int kClinitMaxStack = 2;
    MethodNode*    clinit         = findMethod(clazz, "<clinit>", "()V");
    if (clinit == nullptr) {
        std::vector<ca::str::Utf8StringRef> noExceptions;
        clinit = new MethodNode(Opcodes::ACC_STATIC,
                                ca::str::ZUtf8StringRef::from_static("<clinit>"),
                                ca::str::ZUtf8StringRef::from_static("()V"),
                                ca::str::ZUtf8StringRef::from_static(""),
                                noExceptions);
        clinit->instructions.add(new InsnNode(Opcodes::RETURN));
        clinit->maxStack  = 0;
        clinit->maxLocals = 0;
        clazz.classNode->methods.push_back(clinit);
    }
    InsnList prefix;
    prefix.add(new LdcInsnNode(classId));
    prefix.add(new LdcInsnNode(Type::getObjectType(clazz.classNode->name)));
    prefix.add(new MethodInsnNode(Opcodes::INVOKESTATIC,
                                  ca::str::ZUtf8StringRef::from_static(cloader::kClassName),
                                  ca::str::ZUtf8StringRef::from_static(cloader::kRegisterName),
                                  ca::str::ZUtf8StringRef::from_static(cloader::kRegisterDesc),
                                  false));
    clinit->instructions.insert(clinit->instructions.getFirst(), prefix);
    if (clinit->maxStack < kClinitMaxStack) clinit->maxStack = kClinitMaxStack;
}

}   // namespace

// ===========================================================================
// Vmc4Backend::Impl：RegisterNatives 簿记（镜像 RegCBackend::Impl 的同名结构）。
// ===========================================================================
struct Vmc4Backend::Impl
{
    ca::str::Utf8StringBuilder source;

    struct NativeMethodReg
    {
        std::string javaName;
        std::string javaDesc;
        std::string cFunctionName;
    };
    std::unordered_map<std::string, std::vector<NativeMethodReg>> regsByClass;
    std::vector<std::string>                                      classOrder;
    std::unordered_map<std::string, int>                         classIds;
    std::unordered_set<std::string>                              clinitInjected;

    /// VmInstance 缓存：per-class 粒度按 className 键（同类方法共享实例），
    /// per-method 粒度按 className.method+desc 键。构造确定性（同 seed 同产物）。
    std::unordered_map<std::string, std::unique_ptr<VmInstance>> instances;

    int registerNativeMethod(const std::string& className, const std::string& javaName,
                             const std::string& javaDesc, const std::string& cFunctionName)
    {
        auto [regsIt, insertedClass] = regsByClass.emplace(className, std::vector<NativeMethodReg>{});
        if (insertedClass) {
            classIds.emplace(className, static_cast<int>(classOrder.size()));
            classOrder.push_back(className);
        }
        regsIt->second.push_back({javaName, javaDesc, cFunctionName});
        return classIds[className];
    }

    void emitRegistrationCode(ca::str::Utf8StringBuilder& out) const;
};

Vmc4Backend::Vmc4Backend()
    : impl_(std::make_unique<Impl>())
{}

Vmc4Backend::~Vmc4Backend() = default;

std::string Vmc4Backend::traceSurface() const
{
    std::string out;
    out += "/* VMC4 expected trace surface (mjdbg red-team contract) */\n";
    // 按实例键排序保证确定性输出（per-class 时键即 className）。
    std::vector<std::string> keys;
    for (const auto& kv : impl_->instances) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        out += "\n===== instance \"" + key + "\" =====\n";
        out += impl_->instances.at(key)->describe();
    }
    return out;
}

// ===========================================================================
// compileMethod：镜像 RegCBackend 的阶段 0-8，阶段 5 换成 VmEncoder + 解释器发射。
// ===========================================================================
BackendResult Vmc4Backend::compileMethod(const std::string& className, CompilerMethod& method,
                                         const std::vector<Block*>& blocks,
                                         CompilerIndex* index)
{
    (void)index;
    BackendResult     backendResult;
    auto*             mn         = method.methodNode;
    const std::string methodName = mn->name.to_std_string();
    const std::string methodDesc = mn->desc.to_std_string();
    const bool        isStatic   = (mn->access & Opcodes::ACC_STATIC) != 0;

    // 构造器/类初始化器不直接标 ACC_NATIVE（JVM 部分实现拒收），MVP 不转。
    if (methodName == "<init>" || methodName == "<clinit>") {
        printf("[Vmc4Backend] Skipping %s.%s%s: constructor/<clinit> not virtualized in MVP\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str());
        return backendResult;
    }

    // 实例方法自 Phase 2.B 起支持：SSA 把 this 作为 id0 参数进 RegIR（REF），
    // JNI wrapper 把 thisObject 填进 regs[0]，显式参数从 regs[1] 开始，两者对齐。
    // <init> 仍跳过（JVM 拒收 native 构造器）。

    // 参数/返回类型二次防御：含对象引用的方法在编码阶段已 fail-closed，这里提前拒绝
    // 避免无谓的 SSA 构建。
    const auto retKind = returnKind(methodDesc);
    const auto args    = argumentKinds(methodDesc);
    if (!isVmCompatibleKind(retKind)) {
        printf("[Vmc4Backend] Skipping %s.%s%s: object return type not supported in MVP\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str());
        return backendResult;
    }
    for (auto ak : args) {
        if (!isVmCompatibleKind(ak)) {
            printf("[Vmc4Backend] Skipping %s.%s%s: object parameter not supported in MVP\n",
                   className.c_str(), methodName.c_str(), methodDesc.c_str());
            return backendResult;
        }
    }

    // --- 阶段 2：SSA 构建（与 RegC 共享前端）---
    SSABuilder             ssaBuilder;
    ca::str::Utf8StringArena ssaArena;
    std::string            reason;
    auto ssa = ssaBuilder.build(blocks, className, methodName, methodDesc, isStatic, mn->maxLocals,
                                ssaArena, reason);
    if (!ssa) {
        printf("[Vmc4Backend] Skipping %s.%s%s: SSA build failed: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // --- 阶段 3：RegIR lowering ---
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        printf("[Vmc4Backend] Skipping %s.%s%s: lowering failed: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    // 注意：不调用 applyRegTransformPasses（MBA/CFF/opaque-predicates 是 RegC 的
    // 发射期混淆）。CFF 会注入 switch dispatcher + state copy，VMC4 的 opcode 编码器
    // 不支持 switch terminator；且 VMC4 的混淆本体就是它自己的 opcode 编码与解释器
    // 形态（vmc4_multi_vm_design.md §4），不需要在 VM 内部再套 RegC 变换。

    // --- 阶段 5：VMC4 分叉点 —— RegIR → opcode 流 ---
    // 实例按粒度派生：per-class 同类方法共享同一 VmInstance（缓存），
    // per-method 每方法独立实例。instanceSeed = mix(主 seed, 身份哈希)，
    // 保证可复现（同 seed+同输入 → 同产物）且跨类/方法实例互不相同。
    const bool      perMethod   = options_.granularity == Vmc4Options::Granularity::PerMethod;
    const std::string instanceKey =
        perMethod ? (className + "." + methodName + methodDesc) : className;
    auto it = impl_->instances.find(instanceKey);
    if (it == impl_->instances.end()) {
        const uint64_t identityHash = fnv1a64(instanceKey);
        const uint64_t instanceSeed = vmc4DeriveInstanceSeed(options_.seed, identityHash);
        auto inserted = impl_->instances.emplace(
            instanceKey, std::make_unique<VmInstance>(options_, instanceSeed));
        it = inserted.first;
    }
    VmInstance& instance = *it->second;
    auto        program  = VmEncoder::encode(*reg, instance, reason);
    if (!program) {
        printf("[Vmc4Backend] Skipping %s.%s%s: VM encode failed: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // 方法级 tag（fnv1a64 hex，保证多方法解释器/数组命名唯一）。
    const std::string methodId = className + "." + methodName + methodDesc;
    const std::string methodTag = hex64(fnv1a64(methodId));

    // --- 阶段 5b：发射解释器源码（含数组）---
    const std::string interpreterSrc = VmInterpreterEmitter::emit(*program, instance, methodTag);

    // --- 阶段 6：JNI wrapper（标 ACC_NATIVE + RegisterNatives 绑定）---
    const std::string cFuncName = nativeFunctionName(className, methodName, methodDesc);
    const std::string wrapper   = emitNativeWrapper(className, methodName, methodDesc, isStatic,
                                                    retKind, args, *program, cFuncName, methodTag);

    // --- 阶段 7：登记 RegisterNatives ---
    impl_->registerNativeMethod(className, methodName, methodDesc, cFuncName);

    // --- 阶段 8：组装 BackendResult ---
    backendResult.kind = BackendResult::Kind::C_SOURCE;
    backendResult.text = interpreterSrc + wrapper;
    impl_->source.append(backendResult.text.data(), backendResult.text.size());
    return backendResult;
}

/// 发射 JNI wrapper：把 jxxx 参数填进 mj2x_vm_slot[]，调解释器，把 retval 转回 jxxx。
///
/// @details
/// 签名：static <jniRet> JNICALL <cFuncName>(JNIEnv* env, <jclass|jobject> thisOrClass, jxxx p0, ...)
/// 体：分配 regs[numRegs]；参数按序填 regs[0..n)；调 mj2x_vm_exec_<tag>(regs, &ret)；
/// 除零（status==1）时返回默认值（MVP 不抛 Java 异常，后续 Phase 补 ThrowNew）。
std::string Vmc4Backend::emitNativeWrapper(const std::string& className,
                                           const std::string& methodName,
                                           const std::string& methodDesc, bool isStatic,
                                           PrimitiveKind retKind,
                                           const std::vector<PrimitiveKind>& args,
                                           const VmProgram& program,
                                           const std::string& cFuncName,
                                           const std::string& methodTag)
{
    (void)className;
    (void)methodName;
    (void)methodDesc;
    CSourceStream out;
    {
        CSourceStream note;
        note.append("// VMC4 JNI wrapper (RegisterNatives-bound): ");
        note.append(cFuncName);
        out.emitLine(note.build());
    }
    // 签名行。
    {
        CSourceStream sig;
        sig.append("static ");
        sig.append(jniType(retKind));
        sig.append(" JNICALL ");
        sig.append(cFuncName);
        sig.append("(JNIEnv* env, ");
        sig.append(isStatic ? "jclass clazz" : "jobject thisObject");
        for (size_t i = 0; i < args.size(); ++i) {
            sig.append(", ");
            sig.append(jniType(args[i]));
            sig.append(" p");
            sig.appendInt(static_cast<ca::i64>(i));
        }
        sig.append(") {");
        out.emitLine(sig.build());
    }
    {
        CSourceStream::IndentGuard g(out);
        if (isStatic) {
            out.emitLine("(void)clazz;");
        }
        // 分配寄存器文件 + 返回值槽。
        out.emitLine("mj2x_vm_slot regs[" + std::to_string(program.numRegs ? program.numRegs : 1) +
                     "];");
        out.emitLine("mj2x_vm_slot ret;");
        out.emitLine("ret.i = 0;");
        if (!isStatic) {
            // 实例方法：this 进 regs[0]（SSA 约定 this=参数 id0，SSABuilder 把 this
            // 作为 parameters[0]），显式参数从 regs[1] 开始——与 encoder 的
            // numInputs（f.parameters.size()，含 this）对齐。
            out.emitLine("regs[0].ref = (void*)thisObject;");
        }
        // 填参数：静态方法从 regs[0] 开始对应 p0..p(n-1)；实例方法 this 已占 regs[0]，
        // 显式参数从 regs[1] 开始（desc 参数与 JNI 参数一一对应）。
        // 对象参数（OBJECT）经 (void*) cast 存入 slot.ref。
        for (size_t i = 0; i < args.size(); ++i) {
            CSourceStream line;
            line.append("regs[");
            line.appendInt(static_cast<ca::i64>(i + (isStatic ? 0 : 1)));
            line.append("]");
            line.append(slotField(args[i]));
            line.append(" = ");
            if (args[i] == PrimitiveKind::OBJECT) line.append("(void*)");
            line.append("p");
            line.appendInt(static_cast<ca::i64>(i));
            line.append(";");
            out.emitLine(line.build());
        }
        // 预加载常量槽。
        {
            CSourceStream line;
            line.append("/* load ");
            line.appendInt(static_cast<ca::i64>(program.constantPool.size()));
            line.append(" constants */");
            out.emitLine(line.build());
        }
        // 预加载常量槽：整型直接写，浮点型经 memcpy 从位模式还原（C89 合法的类型双关）。
        for (size_t i = 0; i < program.constantPool.size(); ++i) {
            const auto& c = program.constantPool[i];
            if (c.type == VmType::F32 || c.type == VmType::F64) {
                // 用临时变量 + memcpy 把位模式转成 float/double，再赋给 regs 槽。
                CSourceStream line;
                line.append("{ ");
                if (c.type == VmType::F32) {
                    line.append("uint32_t _b = (uint32_t)mj2x_vm_consts_");
                } else {
                    line.append("uint64_t _b = (uint64_t)mj2x_vm_consts_");
                }
                line.append(methodTag);
                line.append("[");
                line.appendInt(static_cast<ca::i64>(i));
                line.append("].bits; ");
                if (c.type == VmType::F32) {
                    line.append("float _v; memcpy(&_v, &_b, sizeof(_v)); ");
                } else {
                    line.append("double _v; memcpy(&_v, &_b, sizeof(_v)); ");
                }
                line.append("regs[");
                line.appendInt(static_cast<ca::i64>(c.destSlot));
                line.append("].f = _v; }");
                out.emitLine(line.build());
            } else {
                CSourceStream line;
                line.append("regs[");
                line.appendInt(static_cast<ca::i64>(c.destSlot));
                line.append("].i = ");
                if (c.type == VmType::I32) line.append("(int32_t)");
                line.appendInt(c.intBits);
                line.append(";");
                out.emitLine(line.build());
            }
        }
        // 预加载 String LDC 常量槽（Phase 2.E）：NewStringUTF 把 UTF-8 内容物化为
        // jstring，写入编码期分配的专用槽（按内容去重）。调用帧内局部引用，
        // 方法返回后由 JVM 释放——寄存器仅在本帧解释期间使用，安全。
        for (size_t i = 0; i < program.stringLiterals.size(); ++i) {
            const auto& lit = program.stringLiterals[i];
            CSourceStream line;
            line.append("regs[");
            line.appendInt(static_cast<ca::i64>(lit.slot));
            line.append("].ref = (void*)(*env)->NewStringUTF(env, ");
            line.append(codegen::cStringLiteral(lit.value));
            line.append(");");
            out.emitLine(line.build());
        }
        // 预加载 `.class` 字面量常量槽：FindClass 物化 jclass（按内部名去重）。
        // JVM ldc 类常量语义：触发类加载不触发初始化；FindClass 失败置 pending
        // exception，解释器入口检查点传播（NoClassDefFoundError）。局部引用，
        // 本帧解释期间使用安全。
        for (size_t i = 0; i < program.classLiterals.size(); ++i) {
            const auto& lit = program.classLiterals[i];
            CSourceStream line;
            line.append("regs[");
            line.appendInt(static_cast<ca::i64>(lit.slot));
            line.append("].ref = (void*)(*env)->FindClass(env, ");
            line.append(codegen::cStringLiteral(lit.typeDesc));
            line.append(");");
            out.emitLine(line.build());
        }
        // 调解释器。各 object-op 分派表：含该 family 的方法传对应表（backend finalize
        // 发射），不含的传 0（handler 不触发，解释器不读）。空表传 0 而非表名，避免
        // 引用未发射的符号。
        {
            CSourceStream line;
            line.append("unsigned int status = mj2x_vm_exec_");
            line.append(methodTag);
            line.append("(env, regs, &ret, ");
            line.append(program.methodRefs.empty() ? std::string("0")
                                                    : ("mj2x_vm_call_table_" + methodTag));
            line.append(", ");
            line.append(program.loadFieldRefs.empty()
                            ? std::string("0")
                            : ("mj2x_vm_load_field_table_" + methodTag));
            line.append(", ");
            line.append(program.storeFieldRefs.empty()
                            ? std::string("0")
                            : ("mj2x_vm_store_field_table_" + methodTag));
            line.append(", ");
            line.append(program.arrayLoadRefs.empty()
                            ? std::string("0")
                            : ("mj2x_vm_array_load_table_" + methodTag));
            line.append(", ");
            line.append(program.arrayStoreRefs.empty()
                            ? std::string("0")
                            : ("mj2x_vm_array_store_table_" + methodTag));
            line.append(", ");
            line.append(program.allocRefs.empty()
                            ? std::string("0")
                            : ("mj2x_vm_alloc_table_" + methodTag));
            line.append(", ");
            // Intrinsic 表（Phase 2.D 泛化：array.length/cmp.nan_*/instanceof/monitor）。
            line.append(program.intrinsics.empty()
                            ? std::string("0")
                            : ("mj2x_vm_intrinsic_table_" + methodTag));
            line.append(");");
            out.emitLine(line.build());
        }
        // 异常传播（Phase 2.C）：status==1（PendingException）时 pending exception
        // 已在 env 上，直接返回默认值让 JVM 抛（void 方法直接 return）。
        out.emitLine("if (status == 1) {");
        {
            CSourceStream::IndentGuard g2(out);
            if (retKind != PrimitiveKind::VOID) {
                CSourceStream line;
                line.append("return (");
                line.append(jniType(retKind));
                line.append(")");
                line.append(defaultReturnValue(retKind));
                line.append(";");
                out.emitLine(line.build());
            } else {
                out.emitLine("return;");
            }
        }
        out.emitLine("}");
        // 返回。
        if (retKind != PrimitiveKind::VOID) {
            CSourceStream line;
            line.append("return (");
            line.append(jniType(retKind));
            line.append(")ret");
            line.append(slotField(retKind));
            line.append(";");
            out.emitLine(line.build());
        } else {
            out.emitLine("return;");
        }
    }
    out.emitLine("}");
    out.emitBlankLine();
    return out.str();
}

// ===========================================================================
// injectIntoClass：标 ACC_NATIVE + clinit 注入（镜像 regc_backend.cpp:508）。
// ===========================================================================
void Vmc4Backend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                  const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    auto* mn = method.methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack  = 0;
    mn->maxLocals = 0;
    clazz.modified = true;

    if (clazz.classNode == nullptr) return;
    const std::string className = clazz.classNode->name.to_std_string();
    if (className.empty()) return;
    if (impl_->classIds.find(className) != impl_->classIds.end() &&
        impl_->clinitInjected.find(className) == impl_->clinitInjected.end()) {
        prependRegisterNativesCall(clazz, impl_->classIds[className]);
        impl_->clinitInjected.insert(className);
    }
}

// ===========================================================================
// finalize：组装 runtime prelude + 所有方法解释器 + RegisterNatives 表。
// ===========================================================================
void Vmc4Backend::finalize(const CompilerIndex& index)
{
    (void)index;

    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdint.h");
    includes.addSystem("stddef.h");
    includes.addSystem("stdbool.h");
    includes.addSystem("limits.h");
    includes.addSystem("math.h");
    includes.addSystem("stdio.h");
    includes.addSystem("stdlib.h");
    includes.addSystem("string.h");

    ca::str::Utf8StringBuilder full;
    full.append("/* Generated by mj2x Vmc4Backend */\n");
    {
        const std::string renderedIncludes = includes.render();
        full.append(renderedIncludes.data(), renderedIncludes.size());
    }
    full.append("\n");

    // VMC4 runtime prelude（mj2x_vm_slot 联合体 + div/rem helper）。
    const std::string prelude = VmInterpreterEmitter::emitRuntimePrelude();
    full.append(prelude.data(), prelude.size());

    // 所有方法的解释器源码（compileMethod 已累积进 impl_->source）。
    const std::string emittedSource = impl_->source.build().to_std_string();
    full.append(emittedSource.data(), emittedSource.size());

    // RegisterNatives 基础设施。
    ca::str::Utf8StringBuilder entryBuilder;
    impl_->emitRegistrationCode(entryBuilder);
    const std::string entryText = entryBuilder.build().to_std_string();
    full.append(entryText.data(), entryText.size());

    sourceCode_ = full.build().to_std_string();
    if (!cOutputPath_.empty()) {
        std::ofstream ofs(cOutputPath_, std::ios::binary);
        if (ofs.is_open()) ofs << sourceCode_;
    }
}

// ===========================================================================
// emitRegistrationCode：镜像 RegCBackend::Impl::emitRegistrationCode（结构相同）。
// ===========================================================================
void Vmc4Backend::Impl::emitRegistrationCode(ca::str::Utf8StringBuilder& out) const
{
    CSourceStream src;
    src.emitLine("/* Native registration (RegisterNatives, lazy per-class) */");
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
                         cStringLiteral(reg.javaDesc) + ", (void*)" + reg.cFunctionName + "},");
        }
        src.dedent();
        src.emitLine("};");
        src.emitLine("if ((*env)->RegisterNatives(env, clazz, methods, "
                     "sizeof(methods) / sizeof(methods[0])) != 0) {");
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

    // 桥接。
    src.emitLine(std::string("static void JNICALL ") + cloader::kBridgeCFunc +
                 "(JNIEnv* env, jclass loader_class, jint class_id, jclass clazz) {");
    src.indent();
    src.emitLine("(void)loader_class;");
    src.emitLine("if (class_id < 0 || class_id >= " + std::to_string(classOrder.size()) +
                 " || _mj2x_reg_methods[class_id] == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class id "
                 "mismatch\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (clazz == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class is "
                 "null\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (_mj2x_reg_methods[class_id](env, clazz) != JNI_OK) {");
    src.indent();
    src.emitLine("if (!(*env)->ExceptionCheck(env)) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native "
                 "registration failed\");");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    // JNI_OnLoad。
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
    src.emitLine("if ((*env)->RegisterNatives(env, loader_class, loader_methods, "
                 "sizeof(loader_methods) / sizeof(loader_methods[0])) != 0) return JNI_ERR;");
    src.emitLine("(*env)->DeleteLocalRef(env, loader_class);");
    src.emitLine("return JNI_VERSION_1_6;");
    src.dedent();
    src.emitLine("}");

    const std::string rendered = src.str();
    out.append(rendered.data(), rendered.size());
}

}   // namespace mj2x

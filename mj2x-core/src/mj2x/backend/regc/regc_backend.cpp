#include "regc_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/postprocessor/cloader_call.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

using codegen::CSourceStream;

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

const char* regcType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "bool";
    case PrimitiveKind::BYTE: return "int8_t";
    case PrimitiveKind::CHAR: return "uint16_t";
    case PrimitiveKind::SHORT: return "int16_t";
    case PrimitiveKind::INT: return "int32_t";
    case PrimitiveKind::LONG: return "int64_t";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    default: return "mj2x_ref";
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

// ---------------------------------------------------------------------------
// RegisterNatives naming helpers.
//
// RegC 不再使用 Java_<mangled> 符号绑定（符号导出/JVM 符号解析链路脆弱，
// 尤其是内部类名里的 '$' 即便按规范转义也难可靠工作）。改为照搬 StackC 的
// 不透明函数名 + RegisterNatives 动态绑定：每个 native 方法编译成一个
// TU 内部的 `static` C 函数，名字是 `__mj2x_c_<fnv1a64>`，通过每类的
// RegisterNatives 表显式绑定。函数名/可见性完全与 JNI 符号查找脱钩。
// ---------------------------------------------------------------------------

std::uint64_t fnv1a64(std::string_view value, std::uint64_t seed = 1469598103934665603ULL)
{
    std::uint64_t hash = seed;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex64(std::uint64_t value)
{
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llx",
             static_cast<unsigned long long>(value));
    return std::string(buffer);
}

/// 每个 native 方法的 C 函数名：不透明 fnv1a64 哈希，与 JNI 符号查找无关。
std::string nativeFunctionName(const std::string& className, const std::string& methodName,
                               const std::string& methodDesc)
{
    auto hash = fnv1a64(className);
    hash      = fnv1a64(methodName, hash);
    hash      = fnv1a64(methodDesc, hash);
    return "__mj2x_c_" + hex64(hash);
}

/// 每类的 register 函数名：把类名里的非字母数字转义为 _XX 十六进制，保证合法 C 标识符。
std::string registerFunctionName(const std::string& className)
{
    std::string ident = "_mj2x_register_";
    for (unsigned char c : className) {
        if (std::isalnum(c)) {
            ident += static_cast<char>(c);
        }
        else {
            ident += "_";
            ident += hex64(c).substr(14);   // 末 2 位十六进制
        }
    }
    return ident;
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

// high-risk 运行时调用黑名单：命中则该方法保留字节码路径、不转 native（fail-closed）。
// 分组便于逐项放开——每组放开的前置条件不同：
//
//   [反射组] 已放开（R6 引用管理前置完成后）。根因是 call_ref 返回的 Class/Method/Field
//            local ref 生命周期不受管理，现由 emitFunction 的 PushLocalFrame/PopLocalFrame
//            统一批量释放，反射临时 ref 不再撑爆 JNI 16 槽 local ref 表。
//   [类加载]  已放开（R6-B）：ClassLoader 的 loadClass(String)/getResourceAsStream(String)
//            经 ref 参数与 Class/InputStream ref 返回值，引用管理已吸收。defineClass(byte[])
//            的 byte[] 参数经 collect_args array 修正（R6-A）正确传递。
//   [并发]    Thread / java.util.concurrent：Thread.sleep 等在 native 帧调用会破坏线程池
//            monitor（ReentrantLock）状态抛 IllegalMonitorStateException，是独立根因，不属
//            引用管理范畴，暂保留。
//   [IO]      已放开（R6-A）：InputStream/ByteArrayInputStream 的 read/available 等返回 int，
//            byte[] 读写经 ref-array helper，引用管理已吸收 native 帧内产生的 ref。
//   [System]  exit / SecurityManager：语义敏感，暂不转 native。
bool isCompatibilityRiskCall(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name)
{
    // [并发] 保留：Thread.sleep 破坏线程池 monitor
    const bool concurrency =
        owner == "java/lang/Thread" ||
        owner.starts_with(ca::str::Utf8StringRef::from_cstr("java/util/concurrent/"));
    // [System] 保留：语义敏感
    const bool systemSensitive =
        owner == "java/lang/System" &&
        (name == "setSecurityManager" || name == "getSecurityManager" || name == "exit");

    // [反射组] 已放开：java/lang/Class、java/lang/reflect/*、Object.getClass。
    // [IO 组]   已放开（R6-A）：InputStream / ByteArrayInputStream。
    // [类加载]  已放开（R6-B）：ClassLoader.loadClass/getResourceAsStream/defineClass。
    // 引用管理（PushLocalFrame/PopLocalFrame）已吸收反射/IO/类加载临时 ref 泄漏，故不再 skip。

    return concurrency || systemSensitive;
}

bool shouldKeepBytecodeForCompatibility(const std::vector<Block*>& blocks, std::string& reason)
{
    // 注意：原 lambda$ 整体 skip 已撤销。javac 合成的 lambda$ 方法现在由
    // InvokeDynamicPreprocessor（regc_invokedynamic_strategy.md 阶段2-2f）把每个
    // LambdaMetafactory 站点重写为合成实现类，invokedynamic 标记与 lambda$ 体在进入
    // RegC 前已被消除，故无需再按方法名前缀 skip。覆盖见 e2e InvokeDynamic*LambdaRoundTrip。
    // 异常表阈值放宽：lowering 三层对 catch 数量无上限（实测 pack.Main 14 个 catch 正常）。
    // 原 >4 阈值过保守，会把 main 等含多 try-catch 的方法误伤。此处不再按异常表数量 skip。
    for (auto* block : blocks) {
        for (const auto& node : block->nodes) {
            if (node->kind == MIRKind::INVOKE_DYNAMIC) {
                reason = "invokedynamic remains on bytecode path for compatibility";
                return true;
            }
            if (node->kind == MIRKind::INVOKE) {
                const auto* invoke = static_cast<const MIRInvoke*>(node.get());
                if (isCompatibilityRiskCall(invoke->owner, invoke->name)) {
                    reason = "high-risk runtime call remains on bytecode path: " +
                             invoke->owner.to_std_string() + "." + invoke->name.to_std_string();
                    return true;
                }
            }
        }
    }
    return false;
}

std::string emitFailureBody(const std::string& reason, PrimitiveKind retKind)
{
    CSourceStream out;
    out.indent();
    {
        CSourceStream line;
        line.append("/* RegC lowering failed: ");
        line.append(reason);
        line.append(" */");
        out.emitLine(line.build());
    }
    if (retKind == PrimitiveKind::VOID) {
        out.emitLine("return;");
    }
    else {
        CSourceStream line;
        line.append("return ");
        line.append(defaultReturnValue(retKind));
        line.append(";");
        out.emitLine(line.build());
    }
    return out.str();
}

/// 发射单个 native 方法的 C 函数（不透明 `static ... JNICALL __mj2x_c_<hash>`）。
///
/// 这是受 RegisterNatives 绑定的函数：签名必须匹配 Java 描述符的 JNI 调用约定
/// （JNIEnv* + jclass/jobject + 按 jvmSort 映射的 jxxx 参数），返回值也按 jniType
/// 映射。但函数本身不导出（无 JNIEXPORT），名字是不透明哈希，JVM 不会按符号查找它——
/// 绑定完全靠 finalize 阶段发射的 RegisterNatives 表。
///
/// body 与旧 emitJniWrapper 完全一致：设置全局 _mj2x_regc_env、init context、
/// 转调 RegCEmitter 生成的 regc_* helper。
///
/// @param[out] cFuncName 生成的 C 函数名（供 compileMethod 登记到 RegisterNatives 表）。
std::string emitNativeMethod(const std::string& className, const MethodNode& method,
                             const RegCEmitResult& emitted, std::string& cFuncName)
{
    const std::string methodName = method.name.to_std_string();
    const std::string methodDesc = method.desc.to_std_string();
    const bool        isStatic   = (method.access & Opcodes::ACC_STATIC) != 0;
    const auto        retKind    = returnKind(methodDesc);
    const auto        args       = argumentKinds(methodDesc);

    cFuncName = nativeFunctionName(className, methodName, methodDesc);

    CSourceStream out;
    // 函数签名行（顶层，无缩进）：多片段拼一条再 emitLine。
    {
        CSourceStream sig;
        sig.append("static ");
        sig.append(jniType(retKind));
        sig.append(" JNICALL");
        out.emitLine(sig.build());
    }
    {
        CSourceStream sig;
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
        {
            CSourceStream line;
            line.append("(void)");
            line.append(isStatic ? "clazz" : "thisObject");
            line.append(";");
            out.emitLine(line.build());
        }
        out.emitLine("_mj2x_regc_env = env;");
        out.emitLine("mj2x_regc_init_context(&_mj2x_regc_ctx, env, _mj2x_regc_throw_runtime);");

        if (!emitted.ok) {
            const std::string failure = emitFailureBody(emitted.reason, retKind);
            out.emitRaw(failure);
            out.emitLine("}");
            out.emitBlankLine();
            return out.str();
        }

        {
            CSourceStream line;
            if (retKind != PrimitiveKind::VOID) {
                line.append("return (");
                line.append(jniType(retKind));
                line.append(")");
            }
            line.append(emitted.functionName);
            line.append("(");
            bool first = true;
            auto appendArg = [&](std::string_view expr, const char* type) {
                if (!first) line.append(", ");
                first = false;
                line.append("(");
                line.append(type);
                line.append(")(");
                line.append(expr);
                line.append(")");
            };
            if (!isStatic) appendArg("thisObject", "mj2x_ref");
            for (size_t i = 0; i < args.size(); ++i) {
                appendArg("p" + std::to_string(i), regcType(args[i]));
            }
            line.append(");");
            out.emitLine(line.build());
        }
        if (retKind == PrimitiveKind::VOID) out.emitLine("return;");
    }
    out.emitLine("}");
    out.emitBlankLine();
    return out.str();
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

}   // namespace

struct RegCBackend::Impl
{
    ca::str::Utf8StringBuilder source;
    RegCCachePool             cachePool;
    RegCCommentLevel          commentLevel = RegCCommentLevel::Summary;

    /// RegisterNatives 注册簿记：每个受保护类 -> 它的 native 方法列表。
    /// finalize 阶段据此发射每类的 register 函数 + _mj2x_reg_methods[] 表 + JNI_OnLoad。
    struct NativeMethodReg
    {
        std::string javaName;    ///< Java 方法名（原样，如 doAdd / <init> 不会出现在这，clinit/init 不转）
        std::string javaDesc;    ///< Java 描述符（如 (I)V）
        std::string cFunctionName;   ///< 绑定的 C 函数名（__mj2x_c_<hash>）
    };
    std::unordered_map<std::string, std::vector<NativeMethodReg>> regsByClass;
    std::vector<std::string>                                       classOrder;   /// 稳定遍历序
    std::unordered_map<std::string, int>                           classIds;     /// 类名 -> classId
    std::unordered_set<std::string> clinitInjected;   /// 已注入 registerNativesForClass 的类（去重）

    /// 记录一条 native 方法注册，并为首次出现的类分配 classId。
    /// compileMethod 成功路径调用。返回该类的 classId（供 injectIntoClass 注入 clinit）。
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

    /// 在 finalize 阶段发射 RegisterNatives 基础设施：
    /// 每类 register 函数、_mj2x_reg_methods[] 表、桥接 _mj2x_register_for_class、JNI_OnLoad。
    void emitRegistrationCode(ca::str::Utf8StringBuilder& out) const;
};

RegCBackend::RegCBackend()
    : impl_(std::make_unique<Impl>())
{}

RegCBackend::~RegCBackend() = default;

void RegCBackend::setCommentLevel(RegCCommentLevel level)
{
    impl_->commentLevel = level;
    impl_->cachePool.setCommentLevel(level);
}

BackendResult RegCBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                         const std::vector<Block*>& blocks)
{
    BackendResult     backendResult;
    auto*             mn         = method.methodNode;
    const std::string methodName = mn->name.to_std_string();
    const std::string methodDesc = mn->desc.to_std_string();
    const bool        isStatic   = (mn->access & Opcodes::ACC_STATIC) != 0;

    std::string reason;
    // 内部类（含 $）整体 skip 已撤销：RegC 现用 RegisterNatives 动态绑定，
    // 函数名/符号导出与 JNI 查找完全脱钩，内部类的 native 方法可以像普通类一样注册。
    if (shouldKeepBytecodeForCompatibility(blocks, reason)) {
        printf("[RegCBackend] Skipping %s.%s%s: %s\n",
               className.c_str(),
               methodName.c_str(),
               methodDesc.c_str(),
               reason.c_str());
        return backendResult;
    }

    SSABuilder ssaBuilder;
    // RegCBackend is a facade over the existing IR pipeline: L1 blocks are
    // supplied by Pipeline, then this backend owns the SSA and RegIR handoff.
    // SSA 派生串（如数组 desc）入池用局部 arena——SSA/RegIR 在本方法内同步消费完
    // （lower→transform→emit），arena 活到 emitFunction 即可；MIR 透传的 ref 仍指向
    // Pipeline 注入的 job arena（更长寿命），两者共存无冲突（spec §6.3/§6.4）。
    ca::str::Utf8StringArena ssaArena;
    auto ssa = ssaBuilder.build(
        blocks, className, methodName, methodDesc, isStatic, mn->maxLocals, ssaArena, reason);
    if (!ssa) {
        // F1: 静默 fail-closed 补可观测日志，消除 SSA 构建阶段的诊断盲区。
        printf("[RegCBackend] Skipping %s.%s%s: %s: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "SSA build failed", reason.c_str());
        return backendResult;
    }

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        // F2: 静默 fail-closed 补可观测日志，消除 RegIR lowering 阶段的诊断盲区。
        printf("[RegCBackend] Skipping %s.%s%s: %s: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "lowering failed", reason.c_str());
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    applyRegTransformPasses(*reg);

    RegCEmitter emitter(&impl_->cachePool, impl_->commentLevel);
    auto        emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        // F3: 静默 fail-closed 补可观测日志，消除 emitter 发射阶段的诊断盲区。
        printf("[RegCBackend] Skipping %s.%s%s: %s: %s\n",
               className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "emit failed", emitted.reason.c_str());
        return backendResult;
    }

    // Keep the stackless helper as the trace surface and add a thin JNI entry
    // point only for RegisterNatives binding.  The wrapper must not reintroduce
    // a StackC-style operand stack.
    std::string cFuncName;
ca::str::Utf8StringBuilder methodSource;
    methodSource.append(emitted.source.data(), emitted.source.size());
    methodSource.append("\n");
    {
        const std::string wrapper = emitNativeMethod(className, *mn, emitted, cFuncName);
        methodSource.append(wrapper.data(), wrapper.size());
    }

    // 登记 RegisterNatives：本方法绑定到 cFuncName，并为该类分配 classId。
    impl_->registerNativeMethod(className, methodName, methodDesc, cFuncName);

    backendResult.kind = BackendResult::Kind::C_SOURCE;
    backendResult.text = methodSource.build().to_std_string();
    impl_->source.append(backendResult.text.data(), backendResult.text.size());
    return backendResult;
}

/// 往 clinit 头部前插 registerNativesForClass(classId, thisClass) 调用。
/// 无 clinit 则新建一个仅含该调用 + RETURN 的 clinit。每类只注入一次（由调用方去重）。
///
/// 顺序保证：插入在最前。NativeLoaderPostprocessor 在 backend 之后跑，它的
/// emitLoadCall 也在 clinit 最前 insert，会把 NativeLoader.load 插到本调用之前，
/// 最终 clinit 顺序为 [load, registerNativesForClass, ...原有/用户 clinit...]。
/// 这保证先加载 native 库再绑定 native 方法，且绑定发生在用户静态初始化之前
/// （用户 clinit 可能调用 native 方法，必须先完成绑定）。
///
/// className 取自 clazz.classNode->name（Utf8StringRef，arena 持有），避免临时
/// std::string 生命周期的悬垂引用。
void prependRegisterNativesCall(CompilerClass& clazz, int classId)
{
    constexpr int kClinitMaxStack = 2;   // ldc int + ldc class 各占 1 slot

    MethodNode* clinit = findMethod(clazz, "<clinit>", "()V");
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

    using namespace jbytecode::tree;
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

void RegCBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                  const BackendResult& result)
{
    // Failed lowering leaves the Java bytecode intact.  This prevents a class
    // from being rewritten to native when no working RegC body was emitted.
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    auto* mn = method.methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack   = 0;
    mn->maxLocals  = 0;
    clazz.modified = true;

    // RegisterNatives：在该类首次被注入 native 方法时，往 clinit 前插
    // registerNativesForClass(classId, thisClass)。每类只注入一次。
    // classNode 为空（单元测试直调 injectIntoClass）时跳过 clinit 注入——
    // 这种场景没有类文件可改，注册基础设施仍由 finalize 正常发射。
    if (clazz.classNode == nullptr) return;
    const std::string className = clazz.classNode->name.to_std_string();
    if (className.empty()) return;
    if (impl_->classIds.find(className) != impl_->classIds.end() &&
        impl_->clinitInjected.find(className) == impl_->clinitInjected.end()) {
        prependRegisterNativesCall(clazz, impl_->classIds[className]);
        impl_->clinitInjected.insert(className);
    }
    // Native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责（见
    // postprocessor/native_loader_call.hpp），不再在 backend 内重复。
}

void RegCBackend::finalize(const CompilerIndex& index)
{
    (void)index;

    // strtol + errno back the Integer.parseInt intrinsic body emitted below.
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdbool.h");
    includes.addSystem("limits.h");
    includes.addSystem("stdint.h");
    includes.addSystem("stddef.h");
    includes.addSystem("stdarg.h");
    includes.addSystem("math.h");
    includes.addSystem("stdio.h");
    includes.addSystem("stdlib.h");
    includes.addSystem("string.h");
    includes.addSystem("errno.h");
    includes.addProject("regc/mj2x_regc_crt.h");

    ca::str::Utf8StringBuilder full;
    full.append("/* Generated by mj2x RegCBackend */\n");
    {
        const std::string renderedIncludes = includes.render();
        full.append(renderedIncludes.data(), renderedIncludes.size());
    }
    full.append("\n");
    using ca::str::ZUtf8StringRef;
    const std::string stringClassCacheName =
        impl_->cachePool.classCache(ZUtf8StringRef::from_static("java/lang/String")).to_std_string();
    const std::string objectToStringMethodCacheName = impl_->cachePool.instanceMethodCache(
        ZUtf8StringRef::from_static("java/lang/Object"), ZUtf8StringRef::from_static("toString"),
        ZUtf8StringRef::from_static("()Ljava/lang/String;")).to_std_string();
    const std::string stringValueOfFloatMethodCacheName = impl_->cachePool.staticMethodCache(
        ZUtf8StringRef::from_static("java/lang/String"), ZUtf8StringRef::from_static("valueOf"),
        ZUtf8StringRef::from_static("(F)Ljava/lang/String;")).to_std_string();
    const std::string stringValueOfDoubleMethodCacheName = impl_->cachePool.staticMethodCache(
        ZUtf8StringRef::from_static("java/lang/String"), ZUtf8StringRef::from_static("valueOf"),
        ZUtf8StringRef::from_static("(D)Ljava/lang/String;")).to_std_string();
    const std::string floatParseFloatMethodCacheName = impl_->cachePool.staticMethodCache(
        ZUtf8StringRef::from_static("java/lang/Float"), ZUtf8StringRef::from_static("parseFloat"),
        ZUtf8StringRef::from_static("(Ljava/lang/String;)F")).to_std_string();
    const std::string doubleParseDoubleMethodCacheName = impl_->cachePool.staticMethodCache(
        ZUtf8StringRef::from_static("java/lang/Double"), ZUtf8StringRef::from_static("parseDouble"),
        ZUtf8StringRef::from_static("(Ljava/lang/String;)D")).to_std_string();
    const std::string nullPointerExceptionClassCacheName =
        impl_->cachePool.classCache(ZUtf8StringRef::from_static("java/lang/NullPointerException")).to_std_string();
    const std::string classCastExceptionClassCacheName =
        impl_->cachePool.classCache(ZUtf8StringRef::from_static("java/lang/ClassCastException")).to_std_string();
    const std::string outOfMemoryErrorClassCacheName =
        impl_->cachePool.classCache(ZUtf8StringRef::from_static("java/lang/OutOfMemoryError")).to_std_string();
    const std::string stringIndexOutOfBoundsClassCacheName = impl_->cachePool.classCache(
        ZUtf8StringRef::from_static("java/lang/StringIndexOutOfBoundsException")).to_std_string();
    const std::string unsupportedOperationExceptionClassCacheName = impl_->cachePool.classCache(
        ZUtf8StringRef::from_static("java/lang/UnsupportedOperationException")).to_std_string();
    const std::string cacheDeclarations = impl_->cachePool.renderDeclarations();
    const std::string emittedSource     = impl_->source.build().to_std_string();
    emitRuntimeHelpers(full,
                       cacheDeclarations,
                       stringClassCacheName,
                       objectToStringMethodCacheName,
                       stringValueOfFloatMethodCacheName,
                       stringValueOfDoubleMethodCacheName,
                       floatParseFloatMethodCacheName,
                       doubleParseDoubleMethodCacheName,
                       nullPointerExceptionClassCacheName,
                       classCastExceptionClassCacheName,
                       outOfMemoryErrorClassCacheName,
                       stringIndexOutOfBoundsClassCacheName,
                       unsupportedOperationExceptionClassCacheName);
    emitRegCHelperPrelude(full);
    full.append(emittedSource.data(), emittedSource.size());

    // RegisterNatives 基础设施（每类 register fn、_mj2x_reg_methods[] 表、桥接、JNI_OnLoad）。
    // 无任何注册方法时（classOrder 为空）也发射一个不绑任何类的空壳 JNI_OnLoad，
    // 保持"RegC 输出总有 JNI_OnLoad"的契约，便于测试与运行时一致性。
    impl_->emitRegistrationCode(full);

    sourceCode_ = full.build().to_std_string();
    if (!cOutputPath_.empty()) {
        std::ofstream out(cOutputPath_, std::ios::binary);
        if (out) {
            out << sourceCode_;
            printf("[RegCBackend] Written: %s (%zu bytes)\n",
                   cOutputPath_.c_str(),
                   sourceCode_.size());
        }
        else {
            printf("[RegCBackend] ERROR: cannot write %s\n", cOutputPath_.c_str());
        }
    }
}

void RegCBackend::Impl::emitRegistrationCode(ca::str::Utf8StringBuilder& out) const
{
    // 照搬 StackC 的惰性逐类 RegisterNatives 发射（stackc_backend.cpp:1332-1716）：
    //   - 每类一个 static jint _mj2x_register_<class>(JNIEnv*, jclass)：建 JNINativeMethod[]
    //     并 RegisterNatives。
    //   - static _mj2x_reg_method _mj2x_reg_methods[N] 表（按 classId 索引）。
    //   - 桥接 _mj2x_register_for_class：按 class_id 分派到对应 register fn。
    //   - JNI_OnLoad：填表 + FindClass("mj2x/cloader/Loader") + RegisterNatives 绑桥接。
    CSourceStream src;
    src.emitLine("/* Native registration (RegisterNatives, lazy per-class) */");
    src.emitLine("typedef jint (*_mj2x_reg_method)(JNIEnv* env, jclass clazz);");
    src.emitLine(
        "static _mj2x_reg_method _mj2x_reg_methods[" +
        std::to_string(classOrder.empty() ? 1 : classOrder.size()) + "] = {0};");
    src.emitLine("");

    // 每类 register 函数。
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
            src.emitLine("{(char*)" + codegen::cStringLiteral(reg.javaName) + ", (char*)" +
                         codegen::cStringLiteral(reg.javaDesc) + ", (void*)" + reg.cFunctionName +
                         "},");
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

    // 桥接：按 class_id 分派到对应 register fn。
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
    src.emitLine(
        "if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native registration failed\");");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    // JNI_OnLoad：填表 + 绑桥接到 mj2x/cloader/Loader。
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
        src.emitLine("_mj2x_reg_methods[" + std::to_string(idIt->second) +
                     "] = " + registerFunctionName(className) + ";");
    }
    src.emitLine("jclass loader_class = (*env)->FindClass(env, " +
                 codegen::cStringLiteral(cloader::kClassName) + ");");
    src.emitLine("if ((*env)->ExceptionCheck(env) || loader_class == NULL) return JNI_ERR;");
    src.emitLine("static JNINativeMethod loader_methods[] = {");
    src.indent();
    src.emitLine("{(char*)" + codegen::cStringLiteral(cloader::kRegisterName) + ", (char*)" +
                 codegen::cStringLiteral(cloader::kRegisterDesc) + ", (void*)" +
                 cloader::kBridgeCFunc + "},");
    src.dedent();
    src.emitLine("};");
    src.emitLine("if ((*env)->RegisterNatives(env, loader_class, loader_methods, "
                 "sizeof(loader_methods) / sizeof(loader_methods[0])) != 0) return JNI_ERR;");
    src.emitLine("(*env)->DeleteLocalRef(env, loader_class);");
    src.emitLine("return JNI_VERSION_1_6;");
    src.dedent();
    src.emitLine("}");

    {
        const std::string rendered = src.str();
        out.append(rendered.data(), rendered.size());
    }
}

}   // namespace mj2x

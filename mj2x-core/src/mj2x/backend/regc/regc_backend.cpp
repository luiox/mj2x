#include "regc_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/regc/regc_emitter_internal.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
#include "mj2x/mir/passes/ssa_inline_passes.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/postprocessor/cloader_call.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::nativeFunctionName;
using codegen::registerFunctionName;

/// 方法唯一键（owner\nname\ndesc），用于 convertedFunctions/indyFunctions 查表。
std::string methodKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

/// 构建 resolved native dispatch 的运行时定义文本：全局转换函数表 + 按 receiver
/// 类解析/校验帮手。帮手函数经 add_func_declared 登记前向声明到 fwd（调用方
/// 需在渲染声明之前先调用本函数），定义文本由调用方追加在方法体之后。
std::string buildResolvedDispatchTable(
    const std::unordered_map<std::string, std::string>& functions,
    const std::unordered_set<std::string>& indyFunctions, codegen::CForwardDeclarations* fwd,
    bool directCalls)
{
    CSourceStream src;
    src.setForwardDeclarations(fwd);
    src.emitLine("static mj2x_regc_resolved_fn_t kRegCResolvedFunctions[] = {");

    std::vector<std::string> keys;
    for (const auto& [key, fn] : functions) {
        if (indyFunctions.count(key) != 0) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        const size_t first = key.find('\n');
        const size_t second = key.find('\n', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        const std::string owner = key.substr(0, first);
        const std::string name = key.substr(first + 1, second - first - 1);
        const std::string desc = key.substr(second + 1);
        const auto fnIt = functions.find(key);
        if (fnIt == functions.end() || fnIt->second.empty()) continue;
        // directCalls 关闭（默认）时 fn 绑 NULL：resolved 调用点命中后走 JNI
        // CallXxxMethodA（产生 JVM 栈帧，getStackTrace 语义正确，issue #814）。
        if (directCalls) {
            src.emitLine("    {" + codegen::cStringLiteral(owner) + ", " +
                         codegen::cStringLiteral(name) + ", " + codegen::cStringLiteral(desc) +
                         ", NULL, (mj2x_regc_fn_ptr_t)&" + fnIt->second + "},");
        } else {
            src.emitLine("    {" + codegen::cStringLiteral(owner) + ", " +
                         codegen::cStringLiteral(name) + ", " + codegen::cStringLiteral(desc) +
                         ", NULL, NULL},");
        }
    }
    src.emitLine("};");
    src.emitBlankLine();

    src.add_func_declared(
        "static mj2x_regc_fn_ptr_t _mj2x_regc_resolve_verified(const char* name, const char* desc, jmethodID resolved_method, jclass receiver_class, jclass* out_impl_class)",
        [](CSourceStream& s) {
            s.declare_var("const size_t", "count",
                          "sizeof(kRegCResolvedFunctions) / sizeof(kRegCResolvedFunctions[0])");
            s.declare_var("mj2x_regc_resolved_fn_t*", "best", "NULL");
            s.for_("size_t i = 0; i < count; ++i", [](CSourceStream& t) {
                t.declare_var("mj2x_regc_resolved_fn_t*", "e", "&kRegCResolvedFunctions[i]");
                t.iff("strcmp(e->name, name) != 0 || strcmp(e->desc, desc) != 0",
                      [](CSourceStream& u) { u.emitLine("continue;"); });
                t.iff("e->ownerClass == NULL", [](CSourceStream& u) {
                    u.declare_var("jclass", "local",
                                  "(*_mj2x_regc_env)->FindClass(_mj2x_regc_env, e->owner)");
                    u.iff("local == NULL", [](CSourceStream& v) { v.emitLine("continue;"); });
                    u.assign("e->ownerClass",
                             "(jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, local)");
                    u.emitLine("(*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, local);");
                    u.iff("e->ownerClass == NULL",
                          [](CSourceStream& v) { v.emitLine("continue;"); });
                });
                t.iff("!(*_mj2x_regc_env)->IsAssignableFrom(_mj2x_regc_env, e->ownerClass, receiver_class)",
                      [](CSourceStream& u) { u.emitLine("continue;"); });
                t.iff("best == NULL || ((*_mj2x_regc_env)->IsAssignableFrom(_mj2x_regc_env, best->ownerClass, e->ownerClass) && !(*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, best->ownerClass, e->ownerClass))",
                      [](CSourceStream& u) { u.assign("best", "e"); });
            });
            s.iff("best == NULL", [](CSourceStream& u) { u.ret("NULL"); });
            s.emitLine("/* 校验：JVM 实际解析出的方法 == 最具体已转换实现的声明方法。");
            s.emitLine("   中间类存在未转换 override 时两者不同，回退 JNI 保语义。 */");
            s.declare_var("jmethodID", "declared",
                          "(*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, best->ownerClass, name, desc)");
            s.iff("declared == NULL || declared != resolved_method",
                  [](CSourceStream& u) { u.ret("NULL"); });
            s.iff("out_impl_class != NULL",
                  [](CSourceStream& u) { u.assign("*out_impl_class", "best->ownerClass"); });
            s.ret("best->fn");
        });
    src.add_func_declared(
        "static jmethodID _mj2x_regc_resolved_call_site_refresh(mj2x_regc_resolved_call_site_t* site, jobject receiver, jclass receiver_class, const char* name, const char* desc)",
        [](CSourceStream& s) {
            s.emitLine("(void)receiver;");
            s.declare_var("jmethodID", "m",
                          "(*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, receiver_class, name, desc)");
            s.iff("m == NULL", [](CSourceStream& u) { u.ret("NULL"); });
            s.declare_var("jclass", "impl_class", "NULL");
            /* 目标槽：槽 0 空闲（首个调用）写槽 0，否则写槽 1 再与槽 0 交换。
               交替 receiver 时两槽各自命中，避免每轮 refresh（issue #780）。 */
            s.declare_var("const int", "use_slot1", "(site->receiverClass != NULL)");
            s.declare_var("jmethodID*", "dst_m",
                          "use_slot1 ? &site->method1 : &site->method");
            s.declare_var("mj2x_regc_fn_ptr_t*", "dst_fn",
                          "use_slot1 ? &site->fn1 : &site->fn");
            s.declare_var("jclass*", "dst_impl",
                          "use_slot1 ? &site->implClass1 : &site->implClass");
            s.declare_var("jclass*", "dst_rcv",
                          "use_slot1 ? &site->receiverClass1 : &site->receiverClass");
            s.assign("*dst_m", "m");
            s.assign("*dst_fn",
                     "_mj2x_regc_resolve_verified(name, desc, m, receiver_class, &impl_class)");
            s.iff("*dst_impl != NULL", [](CSourceStream& u) {
                u.emitLine("(*_mj2x_regc_env)->DeleteGlobalRef(_mj2x_regc_env, *dst_impl);");
            });
            /* 独立增计数：impl_class 与表条目的 ownerClass 共享同一 global ref，
               目标槽必须持有自己的引用，避免 DeleteGlobalRef 误伤表条目。 */
            s.assign("*dst_impl",
                     "(jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, impl_class)");
            s.iff("*dst_rcv != NULL", [](CSourceStream& u) {
                u.emitLine("(*_mj2x_regc_env)->DeleteGlobalRef(_mj2x_regc_env, *dst_rcv);");
            });
            s.assign("*dst_rcv",
                     "(jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, receiver_class)");
            s.iff("use_slot1", [](CSourceStream& u) {
                /* 槽 1 提升为槽 0：字段互换，global ref 计数不变（每个 ref 仍只释放一次）。 */
                u.declare_var("jmethodID", "tm", "site->method");
                u.assign("site->method", "site->method1");
                u.assign("site->method1", "tm");
                u.declare_var("mj2x_regc_fn_ptr_t", "tf", "site->fn");
                u.assign("site->fn", "site->fn1");
                u.assign("site->fn1", "tf");
                u.declare_var("jclass", "ti", "site->implClass");
                u.assign("site->implClass", "site->implClass1");
                u.assign("site->implClass1", "ti");
                u.declare_var("jclass", "tr", "site->receiverClass");
                u.assign("site->receiverClass", "site->receiverClass1");
                u.assign("site->receiverClass1", "tr");
            });
            s.ret("m");
        });
    src.emitBlankLine();
    return src.str();
}

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
// fnv1a64 / hex64 / nativeFunctionName / registerFunctionName 已收口到
// codegen::c_helpers.hpp，StackC 与 RegC 共用同一命名（见该头文件）。
// ---------------------------------------------------------------------------

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
//   [并发]    已放开（thread-local env/ctx 修复后）：历史根因是全局 JNIEnv 在多线程 native
//            并发（主线程 + 线程池线程）下互相覆盖，导致 Thread.sleep 等在 native 帧调用时
//            破坏线程池 monitor 状态抛 IllegalMonitorStateException。thread_local 修复后每个
//            线程持有独立 env/ctx，该根因消除；java/lang/Thread 与 java/util/concurrent/* 的
//            普通方法调用均为常规 JNI 调用，恢复转 native。
//   [IO]      已放开（R6-A）：InputStream/ByteArrayInputStream 的 read/available 等返回 int，
//            byte[] 读写经 ref-array helper，引用管理已吸收 native 帧内产生的 ref。
//   [System]  已放开：setSecurityManager/getSecurityManager 在 JDK17 直接抛
//            UnsupportedOperationException，native 路径调用行为一致；exit 为常规静态调用。
bool isCompatibilityRiskCall(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name)
{
    (void)owner;
    (void)name;
    return false;
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
            // invokedynamic（阶段3 自定义 bootstrap）由 SSA 层登记站点并生成
            // Intrinsic("indy:<idx>")，RegC emit 运行时解析，不再整方法保留字节码。
            if (node->kind == MIRKind::INVOKE_DYNAMIC) {
                continue;
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
/// @param safeEnvEnabled safe-env 开启时在入口首行发射表恢复调用。
/// @param safeEnvDebug   safe-env 调试观察模式（检出只记录不 fail）。
std::string emitNativeMethod(const std::string& className, const MethodNode& method,
                             const RegCEmitResult& emitted, std::string& cFuncName,
                             bool safeEnvEnabled, bool safeEnvDebug, bool traceCallLog)
{
    const std::string methodName = method.name.to_std_string();
    const std::string methodDesc = method.desc.to_std_string();
    const bool        isStatic   = (method.access & Opcodes::ACC_STATIC) != 0;
    const auto        retKind    = returnKind(methodDesc);
    const auto        args       = argumentKinds(methodDesc);

    cFuncName = nativeFunctionName(className, methodName, methodDesc);

    CSourceStream out;
    // JNI trampoline 注释：绑定 RegisterNatives 的 JNI 入口，转发给无栈 helper。
    {
        CSourceStream note;
        note.append("// JNI trampoline: ");
        note.append(className);
        note.append(".");
        note.append(methodName);
        note.append(methodDesc);
        out.emitLine(note.build());
    }
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
        if (safeEnvEnabled) {
            // 入口首行：任何 JNI 调用前恢复函数表（env 是形参，尚未写入
            // thread_local _mj2x_regc_env）。debug 模式叠加 DEBUG 位。
            CSourceStream line;
            line.append("mj2x_regc_safeenv_restore(env, ");
            if (safeEnvDebug) {
                line.append("MJ2X_SAFEENV_MODE_RESTORE | MJ2X_SAFEENV_MODE_DEBUG");
            }
            else {
                line.append("MJ2X_SAFEENV_MODE_RESTORE");
            }
            line.append(");");
            out.emitLine(line.build());
        }
        {
            CSourceStream line;
            line.append("(void)");
            line.append(isStatic ? "clazz" : "thisObject");
            line.append(";");
            out.emitLine(line.build());
        }
        out.emitLine("_mj2x_regc_env = env;");
        {
            CSourceStream line;
            line.append(isStatic ? "_mj2x_regc_owner_class = clazz;"
                                 : "_mj2x_regc_owner_class = (jclass)(*env)->GetObjectClass(env, thisObject);");
            out.emitLine(line.build());
        }
        out.emitLine("mj2x_regc_init_context(&_mj2x_regc_ctx, env, _mj2x_regc_throw_runtime);");
        if (traceCallLog) {
            // --trace-call-log:运行时开关(#894 B 类下沉后由 mj2x-rt 实现)
            out.emitLine("mj2x_regc_set_trace_call_log(1);");
        }

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
    bool                      traceCallLog = false;
    /// native-to-native 直接 C 调用（--regc-direct-calls；默认关闭保持 JVM 栈帧语义）。
    bool directCalls = false;
    /// safe-env：JNI 函数表快照 + 方法入口恢复（--regc-safe-env）。
    bool safeEnvEnabled = false;
    bool safeEnvDebug   = false;
    /// 文件级 invokedynamic 站点表（自定义 bootstrap，阶段3）：所有方法的站点在此
    /// 汇总，finalize 按全局索引生成 _mj2x_regc_indy_bsm_<i> bootstrap 函数。
    std::vector<RegFunction::InvokeDynamicSite> indySites;
    /// 按类收集的方法体文本（含 JNI wrapper），多文件模式按类分发。
    std::map<std::string, ca::str::Utf8StringBuilder> methodsByClass;

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

    /// 当前编译输入（compileMethod 传入，finalize 前有效）；仅用于调用目标
    /// 元数据查询（final 判定）。旧测试传 nullptr 时直接调用优化关闭。
    CompilerIndex* currentIndex = nullptr;
    /// 已成功转换的方法 -> regc_* 函数体名（owner\nname\ndesc 键）。
    /// 供 native-to-native 直接调用（deterministic dispatch 目标）使用。
    std::unordered_map<std::string, std::string> convertedFunctions;
    /// 预扫描验证过的 javac 合成枚举 values()/valueOf() 方法键集合
    /// （owner\nname\ndesc）。首个 compileMethod 前填充一次——方法注入
    /// native 后指令被清空，发射期无法再现场验证方法体。
    std::set<std::string> enumSyntheticMethods;
    bool                  enumSyntheticScanned = false;
    /// 含 invokedynamic 站点的方法键集合：直接调用会跳过这些目标
    /// （indy bootstrap 依赖 per-frame _mj2x_regc_owner_class）。
    std::unordered_set<std::string> indyFunctions;
    /// 已做「类内预登记」的类名集合：compileMethod 对每类首次转换时预登记
    /// 该类的全部计划方法函数名，使先转换的方法也能直调后转换的同类方法
    /// （private-this 调用只在同类内，javac 语义保证；转换顺序不保证依赖序，
    /// 如 ExceptionProbe.run 先于 risky）。实际转换失败的方法在早退路径
    /// erase 回滚，避免直调不存在的函数体。
    std::unordered_set<std::string> preRegisteredClasses;
    /// 用户自定义 intrinsic registry（issue #818）：setUserIntrinsics 构造，
    /// nullptr 表示未设置（用 builtins()）。
    std::unique_ptr<RegCIntrinsicRegistry> userIntrinsicRegistry;

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

void RegCBackend::setTraceCallLog(bool enabled)
{
    impl_->traceCallLog = enabled;
}

void RegCBackend::setDirectCalls(bool enabled)
{
    impl_->directCalls = enabled;
}

void RegCBackend::setUserIntrinsics(const UserIntrinsicConfig& config)
{
    impl_->userIntrinsicRegistry =
        std::make_unique<RegCIntrinsicRegistry>(config.intrinsics);
}

void RegCBackend::setSafeEnv(bool enabled)
{
    impl_->safeEnvEnabled = enabled;
}

void RegCBackend::setSafeEnvDebug(bool enabled)
{
    impl_->safeEnvEnabled = true;
    impl_->safeEnvDebug   = enabled;
}

BackendResult RegCBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                         const std::vector<Block*>& blocks,
                                         CompilerIndex* index)
{
    BackendResult     backendResult;
    impl_->currentIndex = index;
    // 枚举合成方法预扫描（一次）：验证 javac 合成形态的 values()/valueOf()。
    // 必须在任何方法注入 native（指令清空）之前完成，否则方法体不可见。
    if (index != nullptr && !impl_->enumSyntheticScanned) {
        impl_->enumSyntheticScanned = true;
        for (auto& [clsName, cls] : index->getClasses()) {
            if (cls.classNode == nullptr || cls.isLibrary) continue;
            const ca::str::Utf8StringRef clsRef = cls.classNode->name;
            if (!isEnumSubtypeOwner(index, clsRef)) continue;
            for (const auto& m : cls.methods) {
                auto* em = m.methodNode;
                if (em == nullptr || em->instructions.isEmpty()) continue;
                const std::string emName = em->name.to_std_string();
                const std::string emDesc = em->desc.to_std_string();
                if (emName == "values" && emDesc.rfind("()[L", 0) == 0 &&
                    isJavacEnumValuesBody(
                        em, clsRef,
                        ca::str::Utf8StringRef::from_string_view(emDesc.substr(2)))) {
                    impl_->enumSyntheticMethods.insert(methodKey(clsName, emName, emDesc));
                }
                else if (emName == "valueOf" &&
                         emDesc.rfind("(Ljava/lang/String;)L", 0) == 0 &&
                         isJavacEnumValueOfBody(em, clsRef)) {
                    impl_->enumSyntheticMethods.insert(methodKey(clsName, emName, emDesc));
                }
            }
        }
    }
    auto*             mn         = method.methodNode;
    const std::string methodName = mn->name.to_std_string();
    const std::string methodDesc = mn->desc.to_std_string();
    const bool        isStatic   = (mn->access & Opcodes::ACC_STATIC) != 0;

    std::string reason;
    // 内部类（含 $）整体 skip 已撤销：RegC 现用 RegisterNatives 动态绑定，
    // 函数名/符号导出与 JNI 查找完全脱钩，内部类的 native 方法可以像普通类一样注册。
    if (shouldKeepBytecodeForCompatibility(blocks, reason)) {
        CA_LOGT_WARN("regc", "Skipping {}.{}{}: {}\n", className.c_str(),
               methodName.c_str(),
               methodDesc.c_str(),
               reason.c_str());
        return backendResult;
    }

    // 类内预登记：本类全部计划转换方法的 regc_* 函数名提前可知（functionName
    // 只依赖 owner/name/desc）。private-this 直接调用依赖它——同类方法转换
    // 顺序不保证（如 ExceptionProbe.run 先于 risky）。失败的转换在早退路径
    // erase 回滚。
    if (index != nullptr && impl_->preRegisteredClasses.insert(className).second) {
        CompilerClass* cls = index->getClass(className);
        if (cls != nullptr) {
            for (const auto& m : cls->methods) {
                if (!m.markedForCompile) continue;
                auto                mn     = m.methodNode;
                const std::string   mnName = mn->name.to_std_string();
                const std::string   mnDesc = mn->desc.to_std_string();
                const std::string   key    = methodKey(className, mnName, mnDesc);
                impl_->convertedFunctions[key] =
                    RegCEmitter::functionNameFor(SSAFunctionSymbol{
                        className, mnName, mnDesc, {}, SSAType::UNKNOWN});
            }
        }
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
        CA_LOGT_WARN("regc", "Skipping {}.{}{}: {}: {}\n", className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "SSA build failed", reason.c_str());
        impl_->convertedFunctions.erase(methodKey(className, methodName, methodDesc));
        return backendResult;
    }

    // SSA 方法内联（性能主线：消除 JNI 往返；默认开启，--regc-no-inline 关闭）。
    // resolver：从 CompilerIndex 取被调方法 MethodNode → CodeConverter → SSABuilder。
    if (inlineEnabled_) {
        const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
            if (index == nullptr) return nullptr;
            CompilerClass* cls = index->getClass(ref.owner.to_std_string());
            if (cls == nullptr) return nullptr;
            for (const auto& m : cls->methods) {
                auto* mn = m.methodNode;
                if (mn == nullptr || mn->instructions.isEmpty()) continue;   // native/abstract
                // synchronized 方法：JVM 方法级锁在调用点自动获取，内联会丢锁语义，拒绝。
                if ((mn->access & jbytecode::core::Opcodes::ACC_SYNCHRONIZED) != 0) continue;
                if (mn->name != ref.name || mn->desc != ref.desc) continue;
                // 只有静态方法进入内联候选（invokeOpcode 检查在 pass 侧）。
                CodeConverter            converter(index->arena());
                const std::vector<Block*> calleeBlocks = converter.convert(*mn);
                SSABuilder               calleeBuilder;
                std::string              calleeReason;
                const bool               calleeStatic =
                    (mn->access & jbytecode::core::Opcodes::ACC_STATIC) != 0;
                auto calleeSsa = calleeBuilder.build(
                    calleeBlocks, ref.owner.to_std_string(), ref.name.to_std_string(),
                    ref.desc.to_std_string(), calleeStatic, mn->maxLocals, ssaArena, calleeReason);
                return calleeSsa;
            }
            return nullptr;
        };
        SsaInlineStats stats = applySsaInlining(*ssa, SsaInlineOptions{}, resolver, ssaArena);
        if (stats.inlinedCalls > 0) {
            CA_LOGT_INFO("regc", "inline {} calls in {}.{}{}: unresolvable={} exc={} size={} "
                                 "rec={} depth={} notinlinable={}\n",
                         stats.inlinedCalls, className.c_str(), methodName.c_str(),
                         methodDesc.c_str(), stats.skippedUnresolvable, stats.skippedByException,
                         stats.skippedBySize, stats.skippedByRecursion, stats.skippedByDepth,
                         stats.skippedNotInlinable);
        }
    }

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        // F2: 静默 fail-closed 补可观测日志，消除 RegIR lowering 阶段的诊断盲区。
        CA_LOGT_WARN("regc", "Skipping {}.{}{}: {}: {}\n", className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "lowering failed", reason.c_str());
        impl_->convertedFunctions.erase(methodKey(className, methodName, methodDesc));
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    applyRegTransformPasses(*reg, transformOptions_);

    // 提前登记函数名（供本方法内实例自递归/已转换目标的直接调用决策）；
    // emit 失败时回滚，避免调用方直接调用到不存在的函数体。
    const std::string funcKey = methodKey(className, methodName, methodDesc);
    impl_->convertedFunctions[funcKey] = RegCEmitter::functionNameFor(reg->symbol);

    codegen::CodeOptions emitterOptions;
    emitterOptions.commentLevel    = impl_->commentLevel;
    emitterOptions.traceCallLog    = impl_->traceCallLog;
    emitterOptions.directCalls     = impl_->directCalls;
    emitterOptions.resolvedDispatch = splitOutputDir_.empty();
    emitterOptions.runtimeProtection = runtimeProtection_;
    RegCEmitter emitter(&impl_->cachePool,
                        emitterOptions,
                        &impl_->indySites,
                        impl_->indySites.size(),
                        impl_->currentIndex,
                        &impl_->convertedFunctions,
                        &impl_->indyFunctions,
                        impl_->userIntrinsicRegistry.get(),
                        &impl_->enumSyntheticMethods);
    auto        emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        // F3: 静默 fail-closed 补可观测日志，消除 emitter 发射阶段的诊断盲区。
        CA_LOGT_WARN("regc", "Skipping {}.{}{}: {}: {}\n", className.c_str(), methodName.c_str(), methodDesc.c_str(),
               "emit failed", emitted.reason.c_str());
        impl_->convertedFunctions.erase(funcKey);
        return backendResult;
    }

    // native-to-native 直接调用登记：只有成功转换的方法才可被直接调用。
    impl_->convertedFunctions[funcKey] = emitted.functionName;
    if (!reg->indySites.empty()) {
        impl_->indyFunctions.insert(funcKey);
    }

    // Keep the stackless helper as the trace surface and add a thin JNI entry
    // point only for RegisterNatives binding.  The wrapper must not reintroduce
    // a StackC-style operand stack.
    std::string cFuncName;
ca::str::Utf8StringBuilder methodSource;
    methodSource.append(emitted.source.data(), emitted.source.size());
    methodSource.append("\n");
    {
        const std::string wrapper = emitNativeMethod(className, *mn, emitted, cFuncName,
                                                     impl_->safeEnvEnabled, impl_->safeEnvDebug,
                                                     impl_->traceCallLog);
        methodSource.append(wrapper.data(), wrapper.size());
    }

    // 登记 RegisterNatives：本方法绑定到 cFuncName，并为该类分配 classId。
    impl_->registerNativeMethod(className, methodName, methodDesc, cFuncName);

    backendResult.kind = BackendResult::Kind::C_SOURCE;
    backendResult.text = methodSource.build().to_std_string();
    impl_->source.append(backendResult.text.data(), backendResult.text.size());
    // 按类收集方法文本：多文件模式（--emit-native-dir）按类拆出 m_<Class>.c。
    impl_->methodsByClass[className].append(backendResult.text.data(),
                                            backendResult.text.size());
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

/// 发射 invokedynamic 站点（阶段3 自定义 bootstrap）的文件级 bootstrap helper。
///
/// 每个站点生成一个静态函数 `_mj2x_regc_indy_bsm_<i>(jclass owner)`：首次调用时
/// 构造 Lookup（privateLookupIn 回退 public lookup）、MethodType、bsm 静态参数并
/// 调用 bootstrap 方法，从返回的 CallSite 取 target MethodHandle 缓存为全局引用；
/// 之后直接返回缓存的 target。bootstrap 失败（异常）返回 NULL，调用方按异常路径处理。
namespace {

using codegen::CSourceStream;

/// JVM desc -> boxed 类名（invokedynamic bootstrap 参数需要 primitive Class 对象）。
std::string indyBoxedClassName(const ca::str::Utf8StringRef& desc)
{
    const std::string s = desc.to_std_string();
    if (s.empty()) return "java/lang/Object";
    switch (s[0]) {
    case 'Z': return "java/lang/Boolean";
    case 'B': return "java/lang/Byte";
    case 'C': return "java/lang/Character";
    case 'S': return "java/lang/Short";
    case 'I': return "java/lang/Integer";
    case 'J': return "java/lang/Long";
    case 'F': return "java/lang/Float";
    case 'D': return "java/lang/Double";
    default: return "java/lang/Object";
    }
}

std::string indyTypeDescriptorString(const Type& type)
{
    return type.getDescriptor().to_std_string();
}

std::string indyMethodTypeDescriptorString(const Type& type)
{
    return type.getSort() == Type::METHOD ? type.getDescriptor().to_std_string() : type.toString();
}

void emitIndyMethodType(CSourceStream& out, const std::string& var, const std::string& methodDesc)
{
    out.emitLine("jstring " + var + "_desc = (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                 codegen::cStringLiteral(methodDesc) + ");");
    out.emitLine("jclass " + var + "_class_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/Class\");");
    out.emitLine("jmethodID " + var + "_loader_mid = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, " +
                 var + "_class_cls, \"getClassLoader\", \"()Ljava/lang/ClassLoader;\");");
    out.emitLine("jobject " + var + "_loader = (jobject)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, _mj2x_regc_owner_class, " + var + "_loader_mid);");
    out.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
    out.emitLine("jclass " + var + "_mt_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/invoke/MethodType\");");
    out.emitLine("jmethodID " + var + "_from_desc_mid = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, " + var + "_mt_cls, \"fromMethodDescriptorString\", \"(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;\");");
    out.emitLine("jobject " + var + " = (jobject)(*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, " + var + "_mt_cls, " + var + "_from_desc_mid, " + var +
                 "_desc, " + var + "_loader);");
    out.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
}

void emitIndyClassObject(CSourceStream& out, const std::string& var, const std::string& desc)
{
    if (desc == "V" || desc.empty()) {
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/Void\");");
        return;
    }
    const char head = desc[0];
    const bool primitive = head == 'Z' || head == 'B' || head == 'C' || head == 'S' ||
                           head == 'I' || head == 'J' || head == 'F' || head == 'D';
    if (primitive) {
        const std::string boxed = indyBoxedClassName(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
        out.emitLine("jclass " + var + "_wrapper = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), " + codegen::cStringLiteral(boxed) + ");");
        out.emitLine("jfieldID " + var + "_type_fid = (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, " + var + "_wrapper, \"TYPE\", \"Ljava/lang/Class;\");");
        out.emitLine("jobject " + var + " = (jobject)(*_mj2x_regc_env)->GetStaticObjectField(_mj2x_regc_env, " + var + "_wrapper, " + var + "_type_fid);");
    }
    else {
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_find_class(" +
                     codegen::cStringLiteral(desc) + ");");
    }
}

void emitIndyHandle(CSourceStream& out, const std::string& var, const jbytecode::core::Handle& handle)
{
    const int    tag   = handle.getTag();
    const std::string owner = handle.getOwner().to_std_string();
    const std::string name  = handle.getName().to_std_string();
    const std::string desc  = handle.getDesc().to_std_string();
    out.emitLine("jobject " + var + " = NULL;");
    out.emitLine("{");
    {
        CSourceStream::IndentGuard g(out);
        out.emitLine("jclass " + var + "_lookup_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/invoke/MethodHandles$Lookup\");");
        out.emitLine("jclass " + var + "_owner_cls = (jclass)_mj2x_regc_find_class(" +
                     codegen::cStringLiteral(owner) + ");");
        std::string fieldDesc;
        const bool  isField = tag == Opcodes::H_GETFIELD || tag == Opcodes::H_GETSTATIC ||
                              tag == Opcodes::H_PUTFIELD || tag == Opcodes::H_PUTSTATIC;
        if (isField) {
            emitIndyClassObject(out, var + "_field_type", desc);
        }
        else {
            emitIndyMethodType(out, var + "_type", desc);
        }
        const char* findName  = "findVirtual";
        const char* findDesc =
            "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;";
        std::string args = var + "_owner_cls";
        switch (tag) {
        case Opcodes::H_GETFIELD:
            findName = "findGetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_field_type";
            break;
        case Opcodes::H_GETSTATIC:
            findName = "findStaticGetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_field_type";
            break;
        case Opcodes::H_PUTFIELD:
            findName = "findSetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_field_type";
            break;
        case Opcodes::H_PUTSTATIC:
            findName = "findStaticSetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_field_type";
            break;
        case Opcodes::H_INVOKESTATIC:
            findName = "findStatic";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_type";
            break;
        case Opcodes::H_INVOKESPECIAL:
            findName = "findSpecial";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_type, _mj2x_regc_owner_class";
            break;
        case Opcodes::H_NEWINVOKESPECIAL:
            findName = "findConstructor";
            findDesc = "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;";
            args += ", " + var + "_type";
            break;
        case Opcodes::H_INVOKEINTERFACE:
        case Opcodes::H_INVOKEVIRTUAL:
        default:
            args += ", (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                    codegen::cStringLiteral(name) + "), " + var + "_type";
            break;
        }
        out.emitLine("jmethodID " + var + "_find_mid = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, " + var + "_lookup_cls, " +
                     codegen::cStringLiteral(findName) + ", " + codegen::cStringLiteral(findDesc) +
                     ");");
        out.emitLine(var + " = (jobject)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, _mj2x_indy_lookup, " + var + "_find_mid, " + args + ");");
        out.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
    }
    out.emitLine("}");
}

/// 生成一个 bsm 静态参数表达式（String/MethodType/Class/Handle/装箱原语/varargs 数组）。
void emitIndyBsmArg(CSourceStream& out, const std::string& var, const ca::core::Any& value,
                    const bool vararg)
{
    if (vararg) {
        out.emitLine("jobject " + var + " = NULL;");
        return;
    }
    if (const auto* p = value.as<std::string>()) {
        out.emitLine("jobject " + var + " = (jobject)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                     codegen::cStringLiteral(*p) + ");");
    }
    else if (const auto* p = value.as<ca::str::Utf8StringRef>()) {
        out.emitLine("jobject " + var + " = (jobject)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                     codegen::cStringLiteral(p->to_std_string()) + ");");
    }
    else if (const auto* p = value.as<Type>()) {
        if (p->getSort() == Type::METHOD) {
            emitIndyMethodType(out, var, indyMethodTypeDescriptorString(*p));
        }
        else {
            emitIndyClassObject(out, var, indyTypeDescriptorString(*p));
        }
    }
    else if (const auto* p = value.as<jbytecode::core::Handle>()) {
        emitIndyHandle(out, var, *p);
    }
    else if (const auto* p = value.as<ca::i32>()) {
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_box_i32((int32_t)(" +
                     std::to_string(*p) + "));");
    }
    else if (const auto* p = value.as<ca::i64>()) {
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_box_i64((int64_t)(" +
                     std::to_string(*p) + "LL));");
    }
    else if (const auto* p = value.as<float>()) {
        std::ostringstream text;
        text << std::setprecision(9) << *p << "f";
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_box_f32((float)(" + text.str() +
                     "));");
    }
    else if (const auto* p = value.as<double>()) {
        std::ostringstream text;
        text << std::setprecision(17) << *p;
        out.emitLine("jobject " + var + " = (jobject)_mj2x_regc_box_f64((double)(" + text.str() +
                     "));");
    }
    else {
        out.emitLine("jobject " + var + " = NULL;");
    }
}

void emitIndyBootstrapFunction(ca::str::Utf8StringBuilder& out, size_t index,
                               const RegFunction::InvokeDynamicSite& site)
{
    CSourceStream src;
    src.emitLine("// invokedynamic site " + std::to_string(index) + ": bootstrap " +
                 (site.bsm ? site.bsm->getOwner().to_std_string() + "." + site.bsm->getName().to_std_string() : std::string("<none>")) +
                 " name=" + site.name + " desc=" + site.desc);
    src.emitLine("static jobject _mj2x_regc_indy_bsm_" + std::to_string(index) +
                 "(jclass _mj2x_regc_owner_class) {");
    {
        CSourceStream::IndentGuard g(src);
        src.emitLine("static jobject _mj2x_regc_indy_target_cache_" + std::to_string(index) +
                     " = NULL;");
        src.emitLine("jobject _mj2x_regc_indy_target = NULL;");
        src.emitLine("if (_mj2x_regc_indy_target_cache_" + std::to_string(index) + " != NULL) {");
        src.emitLine("    _mj2x_regc_indy_target = (*_mj2x_regc_env)->NewLocalRef(_mj2x_regc_env, _mj2x_regc_indy_target_cache_" + std::to_string(index) + ");");
        src.emitLine("    if (_mj2x_regc_indy_target == NULL) _mj2x_regc_indy_target_cache_" +
                     std::to_string(index) + " = NULL;");
        src.emitLine("}");
        src.emitLine("if (_mj2x_regc_indy_target == NULL) {");
        {
            CSourceStream::IndentGuard g2(src);
            src.emitLine("jclass _mj2x_indy_mh_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/invoke/MethodHandles\");");
            src.emitLine("jmethodID _mj2x_indy_lookup_mid = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, _mj2x_indy_mh_cls, \"lookup\", \"()Ljava/lang/invoke/MethodHandles$Lookup;\");");
            src.emitLine("jobject _mj2x_indy_lookup = (jobject)(*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, _mj2x_indy_mh_cls, _mj2x_indy_lookup_mid);");
            src.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
            src.emitLine("jmethodID _mj2x_indy_pli_mid = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, _mj2x_indy_mh_cls, \"privateLookupIn\", \"(Ljava/lang/Class;Ljava/lang/invoke/MethodHandles$Lookup;)Ljava/lang/invoke/MethodHandles$Lookup;\");");
            src.emitLine("if (_mj2x_regc_exception_check()) (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env);");
            src.emitLine("if (_mj2x_indy_pli_mid != NULL) {");
            src.emitLine("    jobject _mj2x_indy_pli = (jobject)(*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, _mj2x_indy_mh_cls, _mj2x_indy_pli_mid, _mj2x_regc_owner_class, _mj2x_indy_lookup);");
            src.emitLine("    if (_mj2x_regc_exception_check()) (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env);");
            src.emitLine("    else _mj2x_indy_lookup = _mj2x_indy_pli;");
            src.emitLine("}");
            src.emitLine("jstring _mj2x_indy_name = (*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, " +
                         codegen::cStringLiteral(site.name) + ");");
            emitIndyMethodType(src, "_mj2x_indy_type", site.desc);
            if (!site.bsm) {
                src.emitLine("(*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, _mj2x_regc_throw_runtime(_mj2x_regc_current_context(), \"invokedynamic without bootstrap metadata\"), \"invokedynamic without bootstrap metadata\");");
                src.emitLine("return NULL;");
            }
            else {
                const auto&       bsm      = *site.bsm;
                const std::string bsmOwner = bsm.getOwner().to_std_string();
                const std::string bsmName  = bsm.getName().to_std_string();
                const std::string bsmDesc  = bsm.getDesc().to_std_string();
                src.emitLine("jclass _mj2x_indy_bsm_cls = (jclass)_mj2x_regc_find_class(" +
                             codegen::cStringLiteral(bsmOwner) + ");");
                src.emitLine("jmethodID _mj2x_indy_bsm_mid = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, _mj2x_indy_bsm_cls, " +
                             codegen::cStringLiteral(bsmName) + ", " +
                             codegen::cStringLiteral(bsmDesc) + ");");
                const auto bsmArgTypes =
                    Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(bsmDesc.c_str()));
                std::vector<std::string> bsmExprs;
                size_t                   bsmArgIndex = 0;
                for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
                    if (i == 0) {
                        bsmExprs.push_back("_mj2x_indy_lookup");
                        continue;
                    }
                    if (i == 1) {
                        bsmExprs.push_back("_mj2x_indy_name");
                        continue;
                    }
                    if (i == 2) {
                        bsmExprs.push_back("_mj2x_indy_type");
                        continue;
                    }
                    const auto& formal = bsmArgTypes[i];
                    const bool  isTrailingObjectArray =
                        i + 1 == bsmArgTypes.size() && formal.getSort() == Type::ARRAY &&
                        indyTypeDescriptorString(formal) == "[Ljava/lang/Object;";
                    if (isTrailingObjectArray) {
                        const size_t remaining =
                            bsmArgIndex < site.bsmArgs.size() ? site.bsmArgs.size() - bsmArgIndex : 0;
                        src.emitLine("jclass _mj2x_indy_obj_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/Object\");");
                        src.emitLine("jobjectArray _mj2x_indy_bsm_varargs = (jobjectArray)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, " +
                                     std::to_string(remaining) + ", _mj2x_indy_obj_cls, NULL);");
                        for (size_t j = 0; j < remaining; ++j) {
                            const std::string argVar = "_mj2x_indy_bsm_vararg_" + std::to_string(j);
                            emitIndyBsmArg(src, argVar, site.bsmArgs[bsmArgIndex + j], false);
                            src.emitLine("(*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, _mj2x_indy_bsm_varargs, " + std::to_string(j) + ", " +
                                         argVar + ");");
                        }
                        bsmArgIndex = site.bsmArgs.size();
                        bsmExprs.push_back("_mj2x_indy_bsm_varargs");
                        continue;
                    }
                    if (bsmArgIndex >= site.bsmArgs.size()) {
                        bsmExprs.push_back("NULL");
                        continue;
                    }
                    const std::string argVar = "_mj2x_indy_bsm_arg_" + std::to_string(bsmArgIndex);
                    emitIndyBsmArg(src, argVar, site.bsmArgs[bsmArgIndex], false);
                    bsmExprs.push_back(argVar);
                    ++bsmArgIndex;
                }
                std::string bsmCall = "(*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, _mj2x_indy_bsm_cls, _mj2x_indy_bsm_mid";
                for (const auto& expr : bsmExprs) {
                    bsmCall += ", ";
                    bsmCall += expr;
                }
                bsmCall += ")";
                src.emitLine("jobject _mj2x_indy_callsite = (jobject)" + bsmCall + ";");
                src.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
                src.emitLine("jclass _mj2x_indy_callsite_cls = (jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), \"java/lang/invoke/CallSite\");");
                src.emitLine("jmethodID _mj2x_indy_get_target_mid = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, _mj2x_indy_callsite_cls, \"getTarget\", \"()Ljava/lang/invoke/MethodHandle;\");");
                src.emitLine("_mj2x_regc_indy_target = (jobject)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, _mj2x_indy_callsite, _mj2x_indy_get_target_mid);");
                src.emitLine("if (_mj2x_regc_exception_check()) { return NULL; }");
                src.emitLine("if (_mj2x_regc_indy_target != NULL) {");
                src.emitLine("    jobject _mj2x_regc_indy_target_global = (*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, _mj2x_regc_indy_target);");
                src.emitLine("    if (_mj2x_regc_indy_target_global != NULL) _mj2x_regc_indy_target_cache_" + std::to_string(index) +
                             " = _mj2x_regc_indy_target_global;");
                src.emitLine("}");
            }
        }
        src.emitLine("}");
        src.emitLine("return _mj2x_regc_indy_target;");
    }
    src.emitLine("}");
    src.emitBlankLine();
    out.append(src.build().data(), src.build().size());
}

}   // namespace

/// @brief 生成代码后处理器：拆分多文件模式（--emit-native-dir）用的文本改写。
///
/// @details
/// 单文件模式（--emit-cpp）保持全 static 自包含；多文件模式需要跨 TU 符号，
/// 故对文件级 `static` 做两类改写：
///   - 函数/变量定义：去掉 `static`（外部链接，声明进 api.h）；
///   - 前置声明：`static X f(...);` -> `X f(...);`（隐式 extern）。
/// 函数体内部的 static 局部变量（如 indy target 缓存）必须保留，因此扫描时
/// 追踪大括号深度并跳过字符串字面量。返回值是改写字（声明已收集到 decls）。
namespace {

/// 文件级 static 符号改写（多文件模式）：函数/变量定义去 `static`（外部链接，
/// 声明收集进 decls）；前置声明 `static X f(...);` 去 static（隐式 extern）。
/// 函数体内的 static 局部变量保留（追踪大括号深度，跳过字符串/字符字面量与注释）。
std::string splitRewriteStatic(std::string_view text, std::vector<std::string>& decls)
{
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    int    braceDepth = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            size_t     start = i;
            ++i;
            while (i < text.size()) {
                if (text[i] == '\\') { i += 2; continue; }
                if (text[i] == quote) { ++i; break; }
                ++i;
            }
            out.append(text.substr(start, i - start));
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            size_t start = i;
            while (i < text.size() && text[i] != '\n') ++i;
            out.append(text.substr(start, i - start));
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            i = i + 2 < text.size() ? i + 2 : text.size();
            out.append(text.substr(start, i - start));
            continue;
        }
        if (c == '{') ++braceDepth;
        else if (c == '}') { if (braceDepth > 0) --braceDepth; }
        // 词边界：前一个字符必须是标识符外字符，否则形如 `xstatic ` 的
        // 文件级标识符（理论命中）会被误改写。
        const bool atWordStart = i == 0 || !(text[i - 1] == '_' || isalnum(static_cast<unsigned char>(text[i - 1])));
        if (braceDepth == 0 && atWordStart && text.compare(i, 7, "static ") == 0) {
            size_t j = i + 7;
            // thread_local 族：static thread_local / static __declspec(thread) /
            // static _Thread_local。
            static const std::pair<std::string_view, size_t> kThreadKws[] = {
                {"thread_local ", 13}, {"__declspec(thread) ", 19}, {"_Thread_local ", 14}};
            bool threadLocal = false;
            for (const auto& [kw, len] : kThreadKws) {
                if (text.compare(j, len, kw) == 0) {
                    threadLocal = true;
                    // 声明（进 api.h）：thread_local 是 C++ 关键字，三种形态必须条件化。
                    size_t k = j + len;
                    while (k < text.size() && text[k] != ';' && text[k] != '=' && text[k] != '\n')
                        ++k;
                    std::string typeName = std::string(text.substr(j + len, k - (j + len)));
                    while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
                        typeName.pop_back();
                    decls.push_back(
                        "#if defined(__cplusplus)\nextern thread_local " + typeName +
                        ";\n#elif defined(_MSC_VER)\nextern __declspec(thread) " + typeName +
                        ";\n#else\nextern _Thread_local " + typeName + ";\n#endif");
                    // 定义（去 static，不带 extern——extern 声明不能带初始化值）：
                    // <kw> <type> <name> [= ...;]
                    out.append(kw);
                    out.append(text.substr(j + len, k - (j + len)));
                    i = k;
                    continue;
                }
            }
            if (threadLocal) continue;
            // 函数定义 / 前置声明：`static <ret> <name>(...)[; | {`。
            size_t parenPos = text.find('(', j);
            if (parenPos != std::string_view::npos) {
                bool dotOrEq = false;
                for (size_t k = j; k < parenPos; ++k) {
                    if (text[k] == ';' || text[k] == '=') { dotOrEq = true; break; }
                }
                if (!dotOrEq) {
                    // 签名到匹配 ')'。
                    size_t sigEnd = parenPos + 1;
                    int    depth  = 1;
                    while (sigEnd < text.size() && depth > 0) {
                        if (text[sigEnd] == '(') ++depth;
                        else if (text[sigEnd] == ')') --depth;
                        ++sigEnd;
                    }
                    const std::string sig = std::string(text.substr(j, sigEnd - j));
                    decls.push_back(sig + ";");
                    out.append(sig);
                    i = sigEnd;
                    // 后随 ';' 是前置声明，声明本身保留分号。
                    if (i < text.size() && text[i] == ';') {
                        out.append(";");
                        ++i;
                    }
                    continue;
                }
            }
            // 变量定义：`static <type> <name>[ = ...; ]`。
            size_t declEnd = j;
            while (declEnd < text.size() && text[declEnd] != ';' && text[declEnd] != '\n' &&
                   text[declEnd] != '=') { ++declEnd; }
            const std::string varDecl = std::string(text.substr(j, declEnd - j));
            decls.push_back(varDecl + ";");
            out.append(varDecl);
            if (declEnd < text.size() && text[declEnd] == ';') {
                out.append(";");
                i = declEnd + 1;
            }
            else if (declEnd < text.size() && text[declEnd] == '=') {
                size_t lineEnd = declEnd;
                while (lineEnd < text.size() && text[lineEnd] != '\n') ++lineEnd;
                out.append(text.substr(declEnd, lineEnd - declEnd));
                out.append("\n");
                i = lineEnd;
            }
            else {
                i = declEnd;
            }
            continue;
        }
        out.append(1, c);
        ++i;
    }
    return out;
}

void writeFileIfNeeded(const std::filesystem::path& path, const std::string& content,
                       const std::string& label)
{
    std::ofstream out(path, std::ios::binary);
    if (out) {
        out << content;
        CA_LOGT_INFO("regc", "Written: {} ({} bytes)\n", label.c_str(), content.size());
    }
    else {
        CA_LOGT_ERROR("regc", "cannot write {}\n", label.c_str());
    }
}

}   // namespace

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
    includes.addProject("regc/mj2x_regc_runtime.h");
    includes.addProject("regc/mj2x_regc_safe_env.h");
    if (runtimeProtection_.enabled()) {
        includes.addProject("common/mj2x_rt_protect.h");
    }

    ca::str::Utf8StringBuilder full;
    full.append("/* Generated by mj2x RegCBackend */\n");
    {
        const std::string renderedIncludes = includes.render();
        full.append(renderedIncludes.data(), renderedIncludes.size());
    }
    full.append("\n");
    using ca::str::ZUtf8StringRef;
    const std::string cacheDeclarations = impl_->cachePool.renderDeclarations();
    const std::string emittedSource     = impl_->source.build().to_std_string();
    // 前向声明注册表：add_func_declared 定义的帮手在此登记。resolved dispatch 的
    // 帮手必须先登记（buildResolvedDispatchTable 生成定义时登记），声明随后在
    // emitRuntimeHelpers 渲染到翻译单元头部，方法体无需手工维护声明顺序。
    codegen::CForwardDeclarations fwd;
    std::string                   resolvedDispatchText;
    if (splitOutputDir_.empty()) {
        resolvedDispatchText = buildResolvedDispatchTable(impl_->convertedFunctions,
                                                          impl_->indyFunctions,
                                                          &fwd,
                                                          impl_->directCalls);
    }
    emitRuntimeHelpers(full,
                       cacheDeclarations,
                       &fwd);
    emitRegCHelperPrelude(full);
    for (size_t i = 0; i < impl_->indySites.size(); ++i) {
        emitIndyBootstrapFunction(full, i, impl_->indySites[i]);
    }
    full.append(emittedSource.data(), emittedSource.size());

    // resolved native dispatch 定义（表 + 帮手）追加在所有方法体之后；前向声明
    // 已由 fwd 渲染到头部（emitRuntimeHelpers 内），方法体可先引用。多文件模式
    // 无全局表，emitter 侧 resolvedDispatch 已关闭，这里也不追加。
    if (!resolvedDispatchText.empty()) {
        full.append(resolvedDispatchText.data(), resolvedDispatchText.size());
    }

    // RegisterNatives 基础设施（每类 register fn、_mj2x_reg_methods[] 表、桥接、JNI_OnLoad）。
    // 无任何注册方法时（classOrder 为空）也发射一个不绑任何类的空壳 JNI_OnLoad，
    // 保持"RegC 输出总有 JNI_OnLoad"的契约，便于测试与运行时一致性。
    ca::str::Utf8StringBuilder entryBuilder;
    impl_->emitRegistrationCode(entryBuilder);
    const std::string entryText = entryBuilder.build().to_std_string();

    if (splitOutputDir_.empty()) {
        full.append(entryText.data(), entryText.size());
        sourceCode_ = full.build().to_std_string();
        if (!cOutputPath_.empty()) {
            writeFileIfNeeded(std::filesystem::path(cOutputPath_), sourceCode_, cOutputPath_);
        }
        return;
    }

    // ---- 多文件模式（--emit-native-dir）：头/源分类，库代码与用户代码分离 ----
    //   mj2x_regc_api.h     全部 extern 声明（cache/helpers/方法/indy/trampoline/thread_local）
    //   mj2x_regc_runtime.c 库代码：cache 变量 + runtime helpers + indy bootstrap
    //   m_<Class>.c         用户代码：每类一个文件（方法体 + JNI wrapper）
    //   mj2x_regc_entry.c   注册表 + JNI_OnLoad
    const std::filesystem::path dir(splitOutputDir_);
    std::error_code              ec;
    std::filesystem::create_directories(dir, ec);

    const std::string runtimeFull = full.build().to_std_string();
    // runtime 段 = includes..indy bootstrap（emittedSource 之前的部分）。
    const size_t runtimeEnd = runtimeFull.rfind("/* Generated by mj2x RegCBackend */",
                                                runtimeFull.size() - emittedSource.size());
    const size_t methodsStart = runtimeFull.size() - emittedSource.size();
    const std::string runtimeText = runtimeFull.substr(0, methodsStart);
    (void)runtimeEnd;

    std::vector<std::string> runtimeDecls;
    std::string              runtimeRewritten = splitRewriteStatic(runtimeText, runtimeDecls);

    // api.h 头（includes 与实现共享）。
    ca::str::Utf8StringBuilder api;
    api.append("/* Generated by mj2x RegCBackend - shared declarations */\n");
    api.append("#pragma once\n");
    {
        codegen::IncludeSet apiIncludes;
        apiIncludes.addSystem("jni.h");
        apiIncludes.addSystem("stdbool.h");
        apiIncludes.addSystem("limits.h");
        apiIncludes.addSystem("stdint.h");
        apiIncludes.addSystem("stddef.h");
        apiIncludes.addSystem("stdarg.h");
        apiIncludes.addSystem("math.h");
        apiIncludes.addSystem("stdio.h");
        apiIncludes.addSystem("stdlib.h");
        apiIncludes.addSystem("string.h");
        apiIncludes.addSystem("errno.h");
        apiIncludes.addProject("regc/mj2x_regc_crt.h");
        apiIncludes.addProject("regc/mj2x_regc_runtime.h");
        apiIncludes.addProject("regc/mj2x_regc_safe_env.h");
        const std::string rendered = apiIncludes.render();
        api.append(rendered.data(), rendered.size());
    }
    api.append("\n");
    auto appendApiDecls = [&](const std::vector<std::string>& decls) {
        for (const auto& decl : decls) {
            if (decl.rfind("#if", 0) == 0) {
                api.append(decl.data(), decl.size());
            }
            else {
                api.append("extern ");
                api.append(decl.data(), decl.size());
            }
            api.append("\n");
        }
    };
    appendApiDecls(runtimeDecls);
    api.append("\n");

    // 用户方法按类拆文件；每类文件的 static 改写 + 声明并入 api.h。
    std::map<std::string, std::string> methodFiles;
    std::vector<std::string>           methodDecls;
    for (auto& [className, builder] : impl_->methodsByClass) {
        const std::string methodText = builder.build().to_std_string();
        methodFiles[className] = splitRewriteStatic(methodText, methodDecls);
    }
    appendApiDecls(methodDecls);
    const std::string apiText = api.build().to_std_string();
    writeFileIfNeeded(dir / "mj2x_regc_api.h", apiText, (dir / "mj2x_regc_api.h").string());

    writeFileIfNeeded(dir / "mj2x_regc_runtime.c", runtimeRewritten,
                      (dir / "mj2x_regc_runtime.c").string());

    for (auto& [className, text] : methodFiles) {
        std::string safeName = className;
        for (auto& ch : safeName) {
            if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        }
        std::string fileBody = "/* Generated by mj2x RegCBackend - methods of " + className + " */\n";
        fileBody += "#include \"mj2x_regc_api.h\"\n\n";
        fileBody += text;
        writeFileIfNeeded(dir / ("m_" + safeName + ".c"), fileBody,
                          (dir / ("m_" + safeName + ".c")).string());
    }

    std::string entryBody = "/* Generated by mj2x RegCBackend - native registration */\n";
    entryBody += "#include \"mj2x_regc_api.h\"\n\n";
    entryBody += entryText;
    writeFileIfNeeded(dir / "mj2x_regc_entry.c", entryBody,
                      (dir / "mj2x_regc_entry.c").string());

    sourceCode_ = runtimeFull;
    sourceCode_ += entryText;
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

    // 桥接：按 class_id 分派到对应 register fn。
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
    if (safeEnvEnabled) {
        // safe-env：尽可能早捕获 + 归属校验 JNI 函数表。启动期 agent 已污染时
        // 默认 fail closed（返回 JNI_ERR 使库加载失败）；debug 模式只告警继续。
        src.emitLine("if (mj2x_regc_safeenv_capture(env, " +
                     std::string(safeEnvDebug
                                     ? "MJ2X_SAFEENV_MODE_RESTORE | MJ2X_SAFEENV_MODE_DEBUG"
                                     : "MJ2X_SAFEENV_MODE_RESTORE") +
                     ") != MJ2X_REGC_STATUS_OK) return JNI_ERR;");
    }
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
    src.emitLine("if ((*env)->RegisterNatives(env, loader_class, loader_methods, sizeof(loader_methods) / sizeof(loader_methods[0])) != 0) return JNI_ERR;");
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

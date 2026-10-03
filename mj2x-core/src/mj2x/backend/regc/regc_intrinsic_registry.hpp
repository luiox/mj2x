/// @file regc_intrinsic_registry.hpp
/// @brief Registry of JDK call sites that RegC can lower as native intrinsics.
#pragma once

#include "regc_intrinsic_matcher.hpp"

#include "libca/str/utf8_string.hpp"
#include "mj2x/core/user_intrinsics.hpp"

#include <string>
#include <vector>

namespace mj2x {

/// @brief Intrinsic lowering decision for one RegC call site.
enum class RegCIntrinsicDecisionKind
{
    Emit,
    Fallback,
    Unsupported,
};

/// @brief How the emitter should form the matched intrinsic call expression.
enum class RegCIntrinsicEmitKind
{
    RuntimeFunction,
    RegCLocalHelper,
    WrapperValueOf,
    WrapperUnbox,
    RandomInit,
    NativeObjectMethod,
    StringMethod,
    StringRuntimeMethod,
    StringBuilderInit,
    StringBuilderAppend,
    StringBuilderMethod,
    /// Object.getClass()：语义等价 JNI GetObjectClass（返回 receiver 运行时类），
    /// 发射为 GetObjectClass 表达式而非 resolved dispatch（省 GetObjectClass +
    /// IsSameObject 检查 + DeleteLocalRef 每调用 2-3 个 JNI）。GetObjectClass
    /// 本身不抛异常；NULL receiver 的 NPE 由发射点处理。
    ObjectGetClass,
    /// 用户自定义 intrinsic（issue #818）：直接调用户 symbol。
    /// 与 RuntimeFunction 不同：始终保留调用后 ExceptionCheck 与 local-ref 帧
    /// （isPureRuntimeIntrinsic 只豁免 RuntimeFunction）；passContext 时第一
    /// 参数传 `_mj2x_regc_current_context()->env`（JNIEnv*，三后端一致）；
    /// passReceiver 时 instance 调用点在 env 后传 receiver（RegIR args[0]，
    /// REF 寄存器原样直传）。
    UserIntrinsic,
};

/// @brief Native object state carried by wrapper-class intrinsics.
enum class RegCIntrinsicNativeObjectKind
{
    None,
    Int,
    Long,
    Short,
    Byte,
    Float,
    Double,
    Bool,
    Char,
    Random,
    StringBuilder,
    StringBuffer,
};

/// @brief 发射端直调预检位（#879 Y4 解耦）：随权威表（kEntries）单源，
/// RegC/LLVM 两发射端按位发射同语义预检，不再各写一份符号/形态硬编码。
enum class RegCIntrinsicPrecheck
{
    /// 无预检（聚合初始化零值 = 缺省）。
    None,
    /// 末参 <= 0 → 发射 IllegalArgumentException 预检（fail-closed：
    /// crt 直调无异常通道，JVM 语义应抛异常）。Random.nextInt(bound) 专用。
    PositiveBound,
};

/// @brief Registry lookup result consumed by the RegC emitter.
///
/// @details
/// `Fallback` means the generic `_mj2x_regc_call_*` helper remains correct.
/// `Unsupported` means the call looked like a supported intrinsic signature but
/// its RegIR shape is malformed, so emitting generic code would hide a compiler
/// bug.  `reason` is populated for both cases when useful.
struct RegCIntrinsicDecision
{
    RegCIntrinsicDecisionKind     kind             = RegCIntrinsicDecisionKind::Fallback;
    RegCIntrinsicEmitKind         emitKind         = RegCIntrinsicEmitKind::RuntimeFunction;
    RegCIntrinsicNativeObjectKind nativeObjectKind = RegCIntrinsicNativeObjectKind::None;
    std::string                   symbol;
    std::string                   reason;
    bool                          passContext  = false;
    bool                          passReceiver = false;
    /// 命中条目的发射端预检位（matchNormalizedCall 从 entry 透传；用户条目恒 None）。
    RegCIntrinsicPrecheck         precheck     = RegCIntrinsicPrecheck::None;
};

/// @brief 内建 intrinsic 身份视图（#1027 B2 单源）：kEntries 的只读投影。
///
/// @details 权威表唯一在 regc_intrinsic_registry.cpp 的 kEntries（RegC 发射属
/// 性 + LLVM 直调能力位 llvmDirect 一体）。llvm_intrinsic_registry 由此派生
/// LLVM 类型映射文本，不再维护手工对齐副本——新增 intrinsic 只改本表。
struct RegCIntrinsicIdentity
{
    const char*                   owner;
    const char*                   name;
    const char*                   desc;
    const char*                   symbol;
    RegCIntrinsicEmitKind         emitKind;
    RegCIntrinsicNativeObjectKind nativeObjectKind;
    bool                          llvmDirect;
    /// 发射端预检位（#879 Y4）：随 kEntries 单源，llvm 侧派生表透传。
    RegCIntrinsicPrecheck         precheck;
};

/// @brief 内建表全量身份（kEntries 顺序，173 条；含 llvmDirect=false 条目）。
std::vector<RegCIntrinsicIdentity> builtinIntrinsicTable();

/// @brief Lookup table for RegC JDK intrinsics.
///
/// @details
/// The first slice covers the already live-validated Math and
/// `Integer.parseInt(String)` paths.  Later wrapper and Random support should
/// register here instead of adding one-off emitter branches.
class RegCIntrinsicRegistry
{
public:
    /// @brief Shared process-wide registry for the built-in JDK intrinsic set.
    static const RegCIntrinsicRegistry& builtins();

    /// @brief 默认构造：仅 builtins（无用户条目）。
    RegCIntrinsicRegistry() = default;

    /// @brief 构造带用户自定义 intrinsic（issue #818）的 registry 实例。
    /// @details 用户条目优先于 builtins 匹配（同名同签名时用户覆盖）。
    /// emitKind 恒为 UserIntrinsic（保留调用后 ExceptionCheck 与 local-ref 帧）；
    /// passContext 时第一参数传 JNIEnv*（三后端一致）；passReceiver 条目匹配
    /// instance 调用点并在 env 后传 receiver（static 调用点匹配到 passReceiver
    /// 条目判 Unsupported——静态方法无 receiver，配置校验错误）。
    explicit RegCIntrinsicRegistry(const std::vector<UserIntrinsic>& userEntries);

    /// @brief Decide how one RegIR call should be emitted.
    RegCIntrinsicDecision matchCall(const RegInstruction& ins) const;

    /// @brief Emit C declarations required by the registered intrinsic surface.
    void emitDeclarations(ca::str::Utf8StringBuilder& out) const;

private:
    RegCIntrinsicDecision matchNormalizedCall(const RegCIntrinsicCall& call) const;
    RegCIntrinsicDecision matchUserCall(const RegCIntrinsicCall& call) const;

    /// 用户 intrinsic 条目（RegCIntrinsicEntry 同构：emitKind 恒 UserIntrinsic）。
    struct UserEntry
    {
        std::string owner;
        std::string name;
        std::string desc;
        std::string symbol;
        bool        passContext  = false;
        bool        passReceiver = false;
    };
    std::vector<UserEntry> userEntries_;
};

}   // namespace mj2x

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
    /// 参数传 `_mj2x_regc_current_context()->env`（JNIEnv*，三后端一致）。
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
    bool                          passContext = false;
};

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
    /// passContext 时第一参数传 JNIEnv*（三后端一致）。
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
        bool        passContext = false;
    };
    std::vector<UserEntry> userEntries_;
};

}   // namespace mj2x

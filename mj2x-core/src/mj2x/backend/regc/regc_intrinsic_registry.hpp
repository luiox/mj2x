/**
 * @file regc_intrinsic_registry.hpp
 * @brief Registry of JDK call sites that RegC can lower as native intrinsics.
 */
#pragma once

#include "regc_intrinsic_matcher.hpp"

#include <iosfwd>
#include <string>

namespace mj2x {

/**
 * @brief Intrinsic lowering decision for one RegC call site.
 */
enum class RegCIntrinsicDecisionKind {
    Emit,
    Fallback,
    Unsupported,
};

/**
 * @brief How the emitter should form the matched intrinsic call expression.
 */
enum class RegCIntrinsicEmitKind {
    RuntimeFunction,
    RegCLocalHelper,
    WrapperValueOf,
    WrapperUnbox,
    RandomInit,
    NativeObjectMethod,
};

/**
 * @brief Native object state carried by wrapper-class intrinsics.
 */
enum class RegCIntrinsicNativeObjectKind {
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
};

/**
 * @brief Registry lookup result consumed by the RegC emitter.
 *
 * @details
 * `Fallback` means the generic `_mj2x_regc_call_*` helper remains correct.
 * `Unsupported` means the call looked like a supported intrinsic signature but
 * its RegIR shape is malformed, so emitting generic code would hide a compiler
 * bug.  `reason` is populated for both cases when useful.
 */
struct RegCIntrinsicDecision {
    RegCIntrinsicDecisionKind kind = RegCIntrinsicDecisionKind::Fallback;
    RegCIntrinsicEmitKind emitKind = RegCIntrinsicEmitKind::RuntimeFunction;
    RegCIntrinsicNativeObjectKind nativeObjectKind = RegCIntrinsicNativeObjectKind::None;
    std::string symbol;
    std::string reason;
    bool passContext = false;
};

/**
 * @brief Lookup table for RegC JDK intrinsics.
 *
 * @details
 * The first slice covers the already live-validated Math and
 * `Integer.parseInt(String)` paths.  Later wrapper and Random support should
 * register here instead of adding one-off emitter branches.
 */
class RegCIntrinsicRegistry {
public:
    /**
     * @brief Shared process-wide registry for the built-in JDK intrinsic set.
     */
    static const RegCIntrinsicRegistry& builtins();

    /**
     * @brief Decide how one RegIR call should be emitted.
     */
    RegCIntrinsicDecision matchCall(const RegInstruction& ins) const;

    /**
     * @brief Emit C declarations required by the registered intrinsic surface.
     */
    void emitDeclarations(std::ostream& out) const;

private:
    RegCIntrinsicDecision matchNormalizedCall(const RegCIntrinsicCall& call) const;
};

} // namespace mj2x

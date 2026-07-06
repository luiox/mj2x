/**
 * @file regc_runtime_abi.hpp
 * @brief Stable R3 runtime ABI used by generated RegC translation units.
 *
 * @details
 * RegC-generated native code is moving helper semantics out of each generated
 * C file and into the R3 runtime.  This header defines the first narrow ABI
 * slice: a per-entry context, integer division/remainder helpers with JVM trap
 * semantics, and a barrier hook for later policy checks.
 *
 * The context stores the JVM environment as an opaque pointer rather than
 * including JNI headers.  This keeps `mj2x-runtime-core` usable without a JDK
 * include path while generated JNI adapters can still pass their `JNIEnv*`
 * through the ABI.  Java exception materialization is delegated to a callback
 * supplied by the generated adapter.
 */
#pragma once

#include <cstdint>

extern "C" {

/**
 * @brief Java exception callback supplied by the generated JNI adapter.
 *
 * @param env Opaque VM environment handle originally passed to the context.
 * @param className JVM binary exception class name such as
 *        `java/lang/ArithmeticException`.
 * @param message Optional Java exception message.
 */
using mj2x_regc_throw_callback = void (*)(void* env, const char* className, const char* message);

/**
 * @brief Per-wrapper runtime context shared by RegC runtime helper calls.
 *
 * Generated JNI wrappers initialize this object on entry and then stackless
 * RegC helper functions pass it to runtime-backed helpers.  The context is
 * intentionally small and ABI-stable so later R3 state can be added behind a
 * versioned extension instead of changing existing helper signatures.
 */
struct mj2x_regc_context {
    void* env;
    mj2x_regc_throw_callback throwException;
};

/**
 * @brief Initialize a RegC runtime context for one generated JNI wrapper entry.
 */
void mj2x_regc_init_context(mj2x_regc_context* context,
                            void* env,
                            mj2x_regc_throw_callback throwException);

/**
 * @brief Replace the opaque VM environment while preserving installed callbacks.
 */
void mj2x_regc_set_env(mj2x_regc_context* context, void* env);

/**
 * @brief Return the opaque VM environment stored in a context.
 */
void* mj2x_regc_env(const mj2x_regc_context* context);

/**
 * @brief Throw Java `ArithmeticException` through the context callback.
 */
void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message);

/**
 * @brief JVM-compatible signed 32-bit integer division.
 *
 * Division by zero reports `ArithmeticException` and returns zero so generated
 * code can immediately run its normal pending-exception dispatch.  The
 * `INT_MIN / -1` overflow case follows JVM semantics and returns `INT_MIN`.
 */
std::int32_t mj2x_regc_div_i32(mj2x_regc_context* context, std::int32_t lhs, std::int32_t rhs);

/**
 * @brief JVM-compatible signed 64-bit integer division.
 */
std::int64_t mj2x_regc_div_i64(mj2x_regc_context* context, std::int64_t lhs, std::int64_t rhs);

/**
 * @brief JVM-compatible signed 32-bit integer remainder.
 */
std::int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, std::int32_t lhs, std::int32_t rhs);

/**
 * @brief JVM-compatible signed 64-bit integer remainder.
 */
std::int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, std::int64_t lhs, std::int64_t rhs);

/**
 * @brief Runtime policy barrier reserved for later R3 checks.
 *
 * The initial implementation is intentionally side-effect free.  It establishes
 * the ABI call site that later anti-debug, anti-JVMTI, or key-state checks can
 * occupy without changing generated RegC function bodies.
 */
void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);

}  // extern "C"

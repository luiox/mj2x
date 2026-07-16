/// @file intrinsic_math.hpp
/// @brief `java.lang.Math` native intrinsic entry points.
///
/// @details
/// RegC-emitted code recognizes `java.lang.Math.*` static calls and lowers them
/// to these extern-C entry points instead of JNI static-method dispatch, so a
/// protected method using `Math.sin` / `Math.abs` / `Math.max` evaluates the
/// math entirely in the native runtime without touching the JVM boundary.
///
/// The integer and `abs` family simply forward to C operators.  The floating
/// `min/max` family intentionally diverges from `fmin/fmax`: JLS 20.151/20.153
/// specify that a NaN operand yields NaN, whereas ISO C `fmin/fmax` prefer the
/// non-NaN argument.  We replicate the JDK rule so protected samples match the
/// original Java behavior.
///
/// `floor`/`ceil`/`round`/`sqrt`/`pow` and the trig family forward to the C
/// standard library `<math.h>`; Java and ISO C share the same IEEE 754 base
/// semantics for these.  `random()` is backed by the runtime's Java-compatible
/// PRNG so E2E results stay reproducible against `java.lang.Math.random`.
#pragma once

#include <cstdint>

extern "C" {

// --- abs --------------------------------------------------------------------

/// @brief `Math.abs(int)`.
std::int32_t mj2x_math_abs_i32(std::int32_t value);

/// @brief `Math.abs(long)`.
std::int64_t mj2x_math_abs_i64(std::int64_t value);

/// @brief `Math.abs(float)`.  Returns NaN for NaN input, matching the JDK.
float mj2x_math_abs_f32(float value);

/// @brief `Math.abs(double)`.  Returns NaN for NaN input, matching the JDK.
double mj2x_math_abs_f64(double value);

// --- min / max (JLS NaN-propagation) ----------------------------------------

/// @brief `Math.min(int)`.
std::int32_t mj2x_math_min_i32(std::int32_t lhs, std::int32_t rhs);

/// @brief `Math.max(int)`.
std::int32_t mj2x_math_max_i32(std::int32_t lhs, std::int32_t rhs);

/// @brief `Math.min(long)`.
std::int64_t mj2x_math_min_i64(std::int64_t lhs, std::int64_t rhs);

/// @brief `Math.max(long)`.
std::int64_t mj2x_math_max_i64(std::int64_t lhs, std::int64_t rhs);

/// @brief `Math.min(float)`.  Returns NaN if either operand is NaN.
float mj2x_math_min_f32(float lhs, float rhs);

/// @brief `Math.max(float)`.  Returns NaN if either operand is NaN.
float mj2x_math_max_f32(float lhs, float rhs);

/// @brief `Math.min(double)`.  Returns NaN if either operand is NaN.
double mj2x_math_min_f64(double lhs, double rhs);

/// @brief `Math.max(double)`.  Returns NaN if either operand is NaN.
double mj2x_math_max_f64(double lhs, double rhs);

// --- transcendental / rounding (forward to C math.h) -----------------------

/// @brief `Math.sin(double)`.
double mj2x_math_sin(double value);

/// @brief `Math.cos(double)`.
double mj2x_math_cos(double value);

/// @brief `Math.tan(double)`.
double mj2x_math_tan(double value);

/// @brief `Math.sqrt(double)`.
double mj2x_math_sqrt(double value);

/// @brief `Math.pow(double, double)`.
double mj2x_math_pow(double base, double exponent);

/// @brief `Math.floor(double)`.
double mj2x_math_floor(double value);

/// @brief `Math.ceil(double)`.
double mj2x_math_ceil(double value);

/// @brief `Math.round(double)`.
///
/// @details
/// JDK semantics round half toward positive infinity (`Math.floor(x + 0.5)`),
/// returning `long`.  NaN and infinities follow the documented JDK rules:
/// NaN -> 0, +Inf -> Long.MAX_VALUE, -Inf -> Long.MIN_VALUE.
std::int64_t mj2x_math_round(double value);

/// @brief `Math.random()`.
///
/// @details
/// Returns a `double` in `[0.0, 1.0)` backed by the runtime's Java-compatible
/// PRNG.  The shared per-thread/per-instance state is owned by the random
/// module; this entry point lets `Math.random()` intrinsic share that stream.
double mj2x_math_random();

}   // extern "C"

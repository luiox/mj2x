/**
 * @file intrinsic_wrapper.hpp
 * @brief JDK primitive wrapper-class intrinsic structs and accessors.
 *
 * @details
 * R3 runtime counterpart of the RegC JDK-intrinsic surface.  RegC-emitted code
 * recognizes calls such as `Integer.valueOf(I)` or `box.intValue()` and lowers
 * them to direct calls into this module instead of JNI method dispatch, so the
 * generated translation unit keeps the JVM boundary out of arithmetic on boxed
 * primitives.
 *
 * Each JDK wrapper class maps to a small plain-data struct that carries the
 * unboxed scalar.  This first slice models value semantics only: identity,
 * `equals`, `hashCode`, `toString`, and `parseInt` live in the String intrinsic
 * module because they need the JNI string boundary.  The Integer cache follows
 * the JDK [-128, 127] window through a pre-populated static array.
 *
 * All entry points are `extern "C"` with the `mj2x_intrinsic_` prefix so the
 * RegC emitter can name them from generated C without a C++ mangling
 * dependency.  Types use fixed-width `<cstdint>` aliases to match the existing
 * regc runtime ABI.
 */
#pragma once

#include <cstdint>

extern "C" {

// --- Native struct mirrors of java.lang wrapper instances -------------------

/// @brief Native mirror of `java.lang.Integer`.
struct mj2x_int
{
    std::int32_t value;
};

/// @brief Native mirror of `java.lang.Long`.
struct mj2x_long
{
    std::int64_t value;
};

/// @brief Native mirror of `java.lang.Short`.
struct mj2x_short
{
    std::int16_t value;
};

/// @brief Native mirror of `java.lang.Byte`.
struct mj2x_byte
{
    std::int8_t value;
};

/// @brief Native mirror of `java.lang.Float`.
struct mj2x_float
{
    float value;
};

/// @brief Native mirror of `java.lang.Double`.
struct mj2x_double
{
    double value;
};

/// @brief Native mirror of `java.lang.Boolean` (stores 0/1).
struct mj2x_bool
{
    std::int8_t value;
};

/// @brief Native mirror of `java.lang.Character`.
struct mj2x_char
{
    std::uint16_t value;
};

// --- Integer ----------------------------------------------------------------

/**
 * @brief `Integer.valueOf(int)`: returns the cached instance for [-128, 127].
 *
 * @details
 * The cache is a process-wide static table that mirrors the JDK lazily-built
 * `Integer.IntegerCache`.  Out-of-window values are returned as fresh value
 * structs; later GC work will attach heap ownership to those instances.
 */
mj2x_int mj2x_int_value_of(std::int32_t value);

/// @brief `Integer.intValue()`.
std::int32_t mj2x_int_int_value(mj2x_int box);

// --- Long -------------------------------------------------------------------

/// @brief `Long.valueOf(long)`.
mj2x_long mj2x_long_value_of(std::int64_t value);

/// @brief `Long.longValue()`.
std::int64_t mj2x_long_long_value(mj2x_long box);

// --- Short ------------------------------------------------------------------

/// @brief `Short.valueOf(short)`.
mj2x_short mj2x_short_value_of(std::int16_t value);

/// @brief `Short.shortValue()`.
std::int16_t mj2x_short_short_value(mj2x_short box);

// --- Byte -------------------------------------------------------------------

/// @brief `Byte.valueOf(byte)`.
mj2x_byte mj2x_byte_value_of(std::int8_t value);

/// @brief `Byte.byteValue()`.
std::int8_t mj2x_byte_byte_value(mj2x_byte box);

// --- Float ------------------------------------------------------------------

/// @brief `Float.valueOf(float)`.
mj2x_float mj2x_float_value_of(float value);

/// @brief `Float.floatValue()`.
float mj2x_float_float_value(mj2x_float box);

// --- Double -----------------------------------------------------------------

/// @brief `Double.valueOf(double)`.
mj2x_double mj2x_double_value_of(double value);

/// @brief `Double.doubleValue()`.
double mj2x_double_double_value(mj2x_double box);

// --- Boolean ----------------------------------------------------------------

/**
 * @brief `Boolean.valueOf(boolean)`.
 *
 * @details
 * Mirrors the JDK preallocated TRUE/FALSE instances through two static slots so
 * callers that compare by value stay consistent regardless of the boxing site.
 */
mj2x_bool mj2x_bool_value_of(std::int8_t value);

/// @brief `Boolean.booleanValue()`.
std::int8_t mj2x_bool_bool_value(mj2x_bool box);

// --- Character --------------------------------------------------------------

/// @brief `Character.valueOf(char)`.
mj2x_char mj2x_char_value_of(std::uint16_t value);

/// @brief `Character.charValue()`.
std::uint16_t mj2x_char_char_value(mj2x_char box);

}   // extern "C"

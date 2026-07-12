/**
 * @file intrinsic_random.hpp
 * @brief `java.util.Random` Java-compatible LCG intrinsic.
 *
 * @details
 * RegC-emitted code recognizes `new java.util.Random(seed)` followed by
 * `nextInt()/nextLong()/nextDouble()/...` and lowers them to these entry
 * points so the PRNG runs entirely in native code without JNI method dispatch.
 *
 * The implementation mirrors `java.util.Random` bit-for-bit (the legacy LCG
 * branch, which is what `new Random(seed)` uses when no `RandomFrom` hook is
 * installed): 48-bit state, multiplier 0x5DEECE66D, addend 0xB, mask
 * (2^48 - 1).  nextInt/nextLong/nextDouble/nextBoolean/nextInt(bound) follow
 * the JDK source verbatim so that `new Random(seed)` produces the identical
 * sequence in protected native code and in the original Java.
 *
 * The struct is intentionally single-threaded, matching the JDK class's lack
 * of internal synchronization in its core `next()` path.
 */
#pragma once

#include <cstdint>

extern "C" {

/// @brief Native mirror of `java.util.Random` carrying the 48-bit LCG state.
struct mj2x_random
{
    /// @brief Internal scrambled seed; only the low 48 bits are live.
    std::uint64_t seed;
};

/**
 * @brief `new Random(seed)` initialization.
 *
 * @details
 * Applies the JDK scramble `seed = (seed ^ 0x5DEECE66D) & ((1<<48) - 1)`.
 */
void mj2x_random_init(mj2x_random* state, std::int64_t seed);

/**
 * @brief JDK `Random.next(bits)` core step.
 *
 * @details
 * Advances the LCG and returns the high `bits` of the 48-bit state as a
 * 32-bit int, matching the JDK protected `next(int)` method.  Exposed because
 * the integer LCG-based Random methods are all built on top of it.
 */
std::int32_t mj2x_random_next(mj2x_random* state, int bits);

/// @brief `nextInt()`.
std::int32_t mj2x_random_next_int(mj2x_random* state);

/**
 * @brief `nextInt(bound)`.
 *
 * @details
 * Throws `IllegalArgumentException` (via the regc context callback, when wired)
 * for non-positive bound and returns the JDK's rejection-sampling result
 * otherwise.  Power-of-two bounds take the fast mask path.
 */
std::int32_t mj2x_random_next_int_bound(mj2x_random* state, std::int32_t bound);

/// @brief `nextLong()`.
std::int64_t mj2x_random_next_long(mj2x_random* state);

/// @brief `nextBoolean()`.
std::int8_t mj2x_random_next_boolean(mj2x_random* state);

/// @brief `nextFloat()` in [0.0, 1.0).
float mj2x_random_next_float(mj2x_random* state);

/// @brief `nextDouble()` in [0.0, 1.0).
double mj2x_random_next_double(mj2x_random* state);

}   // extern "C"

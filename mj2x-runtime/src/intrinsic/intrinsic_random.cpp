#include "intrinsic/intrinsic_random.hpp"

#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint64_t kMult = 0x5DEECE66DULL;
constexpr std::uint64_t kAdd  = 0xBULL;
constexpr std::uint64_t kMask = (1ULL << 48) - 1ULL;

// Java nextDouble unit divisor: 1.0 / 2^53.
constexpr double kDoubleUnit = 1.0 / static_cast<double>(1ULL << 53);
// Java nextFloat unit divisor: 1.0 / 2^24.
constexpr float kFloatUnit = 1.0f / static_cast<float>(1 << 24);

}   // namespace

extern "C" {

void mj2x_random_init(mj2x_random* state, std::int64_t seed)
{
    if (state == nullptr) return;
    // JDK scramble: seed = (seed ^ 0x5DEECE66D) & ((1 << 48) - 1).
    state->seed = (static_cast<std::uint64_t>(seed) ^ kMult) & kMask;
}

std::int32_t mj2x_random_next(mj2x_random* state, int bits)
{
    if (state == nullptr) return 0;
    std::uint64_t oldSeed  = state->seed;
    std::uint64_t nextSeed = (oldSeed * kMult + kAdd) & kMask;
    state->seed            = nextSeed;
    return static_cast<std::int32_t>(nextSeed >> (48 - bits));
}

std::int32_t mj2x_random_next_int(mj2x_random* state)
{
    return mj2x_random_next(state, 32);
}

std::int32_t mj2x_random_next_int_bound(mj2x_random* state, std::int32_t bound)
{
    // JDK throws IllegalArgumentException for bound <= 0.  This runtime has no
    // JVM context to materialize the exception from, so return a sentinel that
    // the emitter-side adapter (wired in a later phase) maps to a throw.
    if (bound <= 0) return INT32_MIN;

    // Power-of-two fast path: use the high bits of next(31).
    if ((bound & -bound) == bound) {
        std::int32_t next31 = mj2x_random_next(state, 31);
        // JDK: (int)((bound * (long)next31) >> 31)
        return static_cast<std::int32_t>((static_cast<std::int64_t>(bound) * next31) >> 31);
    }

    // Rejection sampling: avoid modular bias when bound is not a power of two.
    std::int32_t bits;
    std::int32_t value;
    do {
        bits  = mj2x_random_next(state, 31);
        value = bits % bound;
    } while (bits - value + (bound - 1) < 0);
    return value;
}

std::int64_t mj2x_random_next_long(mj2x_random* state)
{
    // JDK: ((long)next(32) << 32) + next(32).  The cast through uint64 keeps
    // sign-extension consistent with the JDK source's int->long promotion.
    std::int32_t high = mj2x_random_next(state, 32);
    std::int32_t low  = mj2x_random_next(state, 32);
    return (static_cast<std::int64_t>(high) << 32) + low;
}

std::int8_t mj2x_random_next_boolean(mj2x_random* state)
{
    return mj2x_random_next(state, 1) != 0 ? 1 : 0;
}

float mj2x_random_next_float(mj2x_random* state)
{
    return static_cast<float>(mj2x_random_next(state, 24)) * kFloatUnit;
}

double mj2x_random_next_double(mj2x_random* state)
{
    std::uint64_t high = static_cast<std::uint32_t>(mj2x_random_next(state, 26));
    std::uint32_t low  = static_cast<std::uint32_t>(mj2x_random_next(state, 27));
    return static_cast<double>((high << 27) + low) * kDoubleUnit;
}

}   // extern "C"

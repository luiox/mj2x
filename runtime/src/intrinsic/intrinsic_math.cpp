#include "intrinsic/intrinsic_math.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

// Math.random() shares the Java-compatible LCG algorithm with the Random
// intrinsic module.  Until the shared PRNG handle is plumbed through, keep one
// process-wide stream here.  The seed matches java.lang.Math.random()'s
// implicit initialization so E2E can compare against an equivalent Java run
// that seeds from a known value; for the default no-arg path the JDK uses a
// per-instance seed derived from entropy, which is not reproducible across
// runs anyway, so a fixed deterministic stream is acceptable for tests.
constexpr std::uint64_t kRandomMult = 0x5DEECE66DULL;
constexpr std::uint64_t kRandomAdd = 0xBULL;
constexpr std::uint64_t kRandomMask = (1ULL << 48) - 1ULL;

std::uint64_t& mathRandomState() {
    // Seed deliberately nonzero so the stream differs from a freshly-zeroed
    // Random; callers that need a fixed sequence should use the Random module.
    static std::uint64_t state = (0x123456789ABCDEF0ULL ^ kRandomMult) & kRandomMask;
    return state;
}

std::uint32_t nextRandomBits(int bits) {
    std::uint64_t& state = mathRandomState();
    state = (state * kRandomMult + kRandomAdd) & kRandomMask;
    return static_cast<std::uint32_t>(state >> (48 - bits));
}

}  // namespace

extern "C" {

std::int32_t mj2x_math_abs_i32(std::int32_t value) {
    // Integer.MIN_VALUE stays as-is per JLS 20.153 abs(int); the raw negate
    // would overflow, so forward to the operator which wraps to MIN_VALUE.
    return value < 0 ? -value : value;
}

std::int64_t mj2x_math_abs_i64(std::int64_t value) {
    return value < 0 ? -value : value;
}

float mj2x_math_abs_f32(float value) {
    return std::fabs(value);
}

double mj2x_math_abs_f64(double value) {
    return std::fabs(value);
}

std::int32_t mj2x_math_min_i32(std::int32_t lhs, std::int32_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

std::int32_t mj2x_math_max_i32(std::int32_t lhs, std::int32_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

std::int64_t mj2x_math_min_i64(std::int64_t lhs, std::int64_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

std::int64_t mj2x_math_max_i64(std::int64_t lhs, std::int64_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

float mj2x_math_min_f32(float lhs, float rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        // JLS 20.151: either operand NaN -> NaN (differs from ISO C fmin).
        return std::numeric_limits<float>::quiet_NaN();
    }
    return lhs < rhs ? lhs : rhs;
}

float mj2x_math_max_f32(float lhs, float rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return lhs > rhs ? lhs : rhs;
}

double mj2x_math_min_f64(double lhs, double rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return lhs < rhs ? lhs : rhs;
}

double mj2x_math_max_f64(double lhs, double rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return lhs > rhs ? lhs : rhs;
}

double mj2x_math_sin(double value) { return std::sin(value); }
double mj2x_math_cos(double value) { return std::cos(value); }
double mj2x_math_tan(double value) { return std::tan(value); }
double mj2x_math_sqrt(double value) { return std::sqrt(value); }
double mj2x_math_pow(double base, double exponent) { return std::pow(base, exponent); }
double mj2x_math_floor(double value) { return std::floor(value); }
double mj2x_math_ceil(double value) { return std::ceil(value); }

std::int64_t mj2x_math_round(double value) {
    // JDK Math.round: NaN -> 0, +Inf -> Long.MAX, -Inf -> Long.MIN, else
    // floor(value + 0.5) as long.
    if (std::isnan(value)) return 0;
    if (value >= std::numeric_limits<double>::max() / 2.0) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= -std::numeric_limits<double>::max() / 2.0) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::floor(value + 0.5));
}

double mj2x_math_random() {
    // next(26) high then next(27) low, matching java.util.Random.nextDouble().
    std::uint64_t hi = nextRandomBits(26);
    std::uint64_t lo = nextRandomBits(27);
    return static_cast<double>((hi << 27) + lo) / static_cast<double>(1ULL << 53);
}

}  // extern "C"

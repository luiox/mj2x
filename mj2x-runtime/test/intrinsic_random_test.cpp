#include "intrinsic/intrinsic_random.hpp"

#include <gmock/gmock.h>

#include <cstdint>

namespace {

// Reference values captured from `new java.util.Random(42)` on Corretto 17.0.5.
// They pin the runtime LCG to the exact JDK bit-for-bit sequence so a protected
// native Random matches the original Java behavior.
constexpr std::int64_t kSeed                  = 42;
constexpr std::int32_t kRefNextIntUnbounded[] = {
    -1170105035,
    234785527,
    -1360544799,
    205897768,
    1325939940,
};
constexpr std::int64_t kRefNextLong[] = {
    -5025562857975149833LL,
    -5843495416241995736LL,
};
constexpr double kRefNextDouble[] = {
    0.7275636800328681,
    0.6832234717598454,
};
constexpr std::int8_t  kRefNextBoolean[]     = {1, 0};   // true, false
constexpr std::int32_t kRefNextIntBound100[] = {30, 63};
constexpr std::int32_t kRefNextIntBound16    = 11;

TEST(IntrinsicRandomTest, NextIntMatchesJdkSequenceBitForBit)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    for (std::int32_t expected : kRefNextIntUnbounded) {
        EXPECT_EQ(mj2x_random_next_int(&state), expected);
    }
}

TEST(IntrinsicRandomTest, NextLongMatchesJdkSequenceBitForBit)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    for (std::int64_t expected : kRefNextLong) {
        EXPECT_EQ(mj2x_random_next_long(&state), expected);
    }
}

TEST(IntrinsicRandomTest, NextDoubleMatchesJdkSequenceBitForBit)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    for (double expected : kRefNextDouble) {
        EXPECT_DOUBLE_EQ(mj2x_random_next_double(&state), expected);
    }
}

TEST(IntrinsicRandomTest, NextBooleanMatchesJdkSequence)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    for (std::int8_t expected : kRefNextBoolean) {
        EXPECT_EQ(mj2x_random_next_boolean(&state), expected);
    }
}

TEST(IntrinsicRandomTest, NextIntBoundMatchesJdkSequence)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    for (std::int32_t expected : kRefNextIntBound100) {
        EXPECT_EQ(mj2x_random_next_int_bound(&state, 100), expected);
    }
}

TEST(IntrinsicRandomTest, NextIntPowerOfTwoBoundMatchesJdkFastPath)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    EXPECT_EQ(mj2x_random_next_int_bound(&state, 16), kRefNextIntBound16);
}

TEST(IntrinsicRandomTest, NonPositiveBoundReturnsSentinel)
{
    mj2x_random state;
    mj2x_random_init(&state, kSeed);
    // No JVM context available to throw IllegalArgumentException here; the
    // emitter adapter is responsible for turning this sentinel into a throw.
    EXPECT_EQ(mj2x_random_next_int_bound(&state, 0), INT32_MIN);
    EXPECT_EQ(mj2x_random_next_int_bound(&state, -5), INT32_MIN);
}

TEST(IntrinsicRandomTest, DeterministicForSameSeed)
{
    mj2x_random a;
    mj2x_random b;
    mj2x_random_init(&a, 12345);
    mj2x_random_init(&b, 12345);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(mj2x_random_next_int(&a), mj2x_random_next_int(&b));
    }
}

}   // namespace

#include "intrinsic/intrinsic_math.hpp"

#include <gmock/gmock.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kPInf = std::numeric_limits<double>::infinity();
constexpr double kNInf = -std::numeric_limits<double>::infinity();

TEST(IntrinsicMathTest, AbsHandlesNegativesAndExtremes) {
    EXPECT_EQ(mj2x_math_abs_i32(-5), 5);
    EXPECT_EQ(mj2x_math_abs_i32(5), 5);
    EXPECT_EQ(mj2x_math_abs_i32(0), 0);
    // Integer.MIN_VALUE wraps back to MIN_VALUE per JLS (no overflow trap).
    EXPECT_EQ(mj2x_math_abs_i32(INT32_MIN), INT32_MIN);

    EXPECT_EQ(mj2x_math_abs_i64(-7LL), 7LL);
    EXPECT_EQ(mj2x_math_abs_i64(INT64_MIN), INT64_MIN);

    EXPECT_FLOAT_EQ(mj2x_math_abs_f32(-3.25f), 3.25f);
    EXPECT_DOUBLE_EQ(mj2x_math_abs_f64(-2.5), 2.5);
    EXPECT_TRUE(std::isnan(mj2x_math_abs_f64(kNan)));
}

TEST(IntrinsicMathTest, IntegerMinMaxPickExtremes) {
    EXPECT_EQ(mj2x_math_min_i32(-3, 7), -3);
    EXPECT_EQ(mj2x_math_max_i32(-3, 7), 7);
    EXPECT_EQ(mj2x_math_min_i64(INT64_MIN, 1), INT64_MIN);
    EXPECT_EQ(mj2x_math_max_i64(INT64_MAX, 1), INT64_MAX);
}

TEST(IntrinsicMathTest, FloatingMinMaxPropagateNanJlsStyle) {
    // JLS: either operand NaN -> NaN.  This is the deliberate divergence from
    // ISO C fmin/fmax which would return the non-NaN argument.
    EXPECT_TRUE(std::isnan(mj2x_math_min_f64(kNan, 5.0)));
    EXPECT_TRUE(std::isnan(mj2x_math_max_f64(5.0, kNan)));
    EXPECT_TRUE(std::isnan(mj2x_math_min_f32(kNan, 5.0f)));
    EXPECT_TRUE(std::isnan(mj2x_math_max_f32(5.0f, kNan)));

    EXPECT_DOUBLE_EQ(mj2x_math_min_f64(2.5, 3.5), 2.5);
    EXPECT_DOUBLE_EQ(mj2x_math_max_f64(2.5, 3.5), 3.5);
}

TEST(IntrinsicMathTest, TranscendentalsAndPowMatchCLibrary) {
    EXPECT_NEAR(mj2x_math_sin(1.0), std::sin(1.0), 1e-15);
    EXPECT_NEAR(mj2x_math_cos(1.0), std::cos(1.0), 1e-15);
    EXPECT_NEAR(mj2x_math_tan(0.5), std::tan(0.5), 1e-15);
    EXPECT_NEAR(mj2x_math_sqrt(2.0), std::sqrt(2.0), 1e-15);
    EXPECT_NEAR(mj2x_math_pow(2.0, 10.0), std::pow(2.0, 10.0), 1e-12);
}

TEST(IntrinsicMathTest, FloorCeilRoundHalfTowardPositiveInfinity) {
    EXPECT_DOUBLE_EQ(mj2x_math_floor(2.7), 2.0);
    EXPECT_DOUBLE_EQ(mj2x_math_floor(-2.7), -3.0);
    EXPECT_DOUBLE_EQ(mj2x_math_ceil(2.2), 3.0);
    EXPECT_DOUBLE_EQ(mj2x_math_ceil(-2.2), -2.0);

    // JDK Math.round is floor(x + 0.5): +0.5 rounds up, -0.5 rounds to 0.
    EXPECT_EQ(mj2x_math_round(2.5), 3);
    EXPECT_EQ(mj2x_math_round(-2.5), -2);
    EXPECT_EQ(mj2x_math_round(2.4), 2);
    EXPECT_EQ(mj2x_math_round(-2.6), -3);

    // Boundary rules: NaN -> 0, +Inf -> Long.MAX, -Inf -> Long.MIN.
    EXPECT_EQ(mj2x_math_round(kNan), 0);
    EXPECT_EQ(mj2x_math_round(kPInf), INT64_MAX);
    EXPECT_EQ(mj2x_math_round(kNInf), INT64_MIN);
}

TEST(IntrinsicMathTest, RandomStaysInUnitHalfOpenInterval) {
    for (int i = 0; i < 64; ++i) {
        double value = mj2x_math_random();
        EXPECT_GE(value, 0.0);
        EXPECT_LT(value, 1.0);
    }
    // Two consecutive calls are deterministic for a fixed process seed and
    // differ from each other once the stream advances.
    double a = mj2x_math_random();
    double b = mj2x_math_random();
    EXPECT_NE(a, b);
}

}  // namespace

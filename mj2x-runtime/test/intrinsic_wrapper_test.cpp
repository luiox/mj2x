#include "intrinsic/intrinsic_wrapper.hpp"

#include <gmock/gmock.h>

#include <cstdint>
#include <limits>

namespace {

TEST(IntrinsicWrapperTest, IntegerValueOfReturnsCachedSlotInsideWindow) {
    // The cache mirrors the JDK [-128, 127] window: the boxed value carries the
    // original int and a valueOf within the window must be stable across calls.
    mj2x_int a = mj2x_int_value_of(0);
    mj2x_int b = mj2x_int_value_of(0);
    EXPECT_EQ(a.value, 0);
    EXPECT_EQ(b.value, 0);

    mj2x_int low = mj2x_int_value_of(-128);
    mj2x_int high = mj2x_int_value_of(127);
    EXPECT_EQ(low.value, -128);
    EXPECT_EQ(high.value, 127);
}

TEST(IntrinsicWrapperTest, IntegerValueOfOutsideWindowStillCarriesValue) {
    mj2x_int over = mj2x_int_value_of(128);
    mj2x_int under = mj2x_int_value_of(-129);
    EXPECT_EQ(over.value, 128);
    EXPECT_EQ(under.value, -129);
}

TEST(IntrinsicWrapperTest, IntegerIntValueUnboxes) {
    EXPECT_EQ(mj2x_int_int_value(mj2x_int_value_of(42)), 42);
    EXPECT_EQ(mj2x_int_int_value(mj2x_int_value_of(-7)), -7);
    EXPECT_EQ(mj2x_int_int_value(mj2x_int{INT32_MIN}), INT32_MIN);
}

TEST(IntrinsicWrapperTest, LongBoxUnboxRoundTrip) {
    EXPECT_EQ(mj2x_long_long_value(mj2x_long_value_of(INT64_C(9000000000000))), INT64_C(9000000000000));
    EXPECT_EQ(mj2x_long_long_value(mj2x_long{INT64_MIN}), INT64_MIN);
    EXPECT_EQ(mj2x_long_long_value(mj2x_long{INT64_MAX}), INT64_MAX);
}

TEST(IntrinsicWrapperTest, ShortByteBoxUnboxPreserveWidth) {
    EXPECT_EQ(mj2x_short_short_value(mj2x_short_value_of(static_cast<std::int16_t>(-320))), -320);
    EXPECT_EQ(mj2x_short_short_value(mj2x_short{INT16_MIN}), INT16_MIN);

    EXPECT_EQ(mj2x_byte_byte_value(mj2x_byte_value_of(static_cast<std::int8_t>(-12))), -12);
    EXPECT_EQ(mj2x_byte_byte_value(mj2x_byte{INT8_MAX}), INT8_MAX);
}

TEST(IntrinsicWrapperTest, FloatDoubleBoxUnboxRoundTrip) {
    EXPECT_FLOAT_EQ(mj2x_float_float_value(mj2x_float_value_of(3.5f)), 3.5f);
    EXPECT_DOUBLE_EQ(mj2x_double_double_value(mj2x_double_value_of(2.25)), 2.25);
    // NaN/Infinity survive because the box stores the raw float bits.
    const double kNan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(std::isnan(mj2x_double_double_value(mj2x_double_value_of(kNan))));
}

TEST(IntrinsicWrapperTest, BooleanValueOfNormalizesToZeroOrOne) {
    EXPECT_EQ(mj2x_bool_bool_value(mj2x_bool_value_of(0)), 0);
    EXPECT_EQ(mj2x_bool_bool_value(mj2x_bool_value_of(1)), 1);
    // Any non-zero source boolean collapses to the cached TRUE slot.
    EXPECT_EQ(mj2x_bool_bool_value(mj2x_bool_value_of(42)), 1);
}

TEST(IntrinsicWrapperTest, CharacterBoxUnboxPreservesUtf16Unit) {
    EXPECT_EQ(mj2x_char_char_value(mj2x_char_value_of(static_cast<std::uint16_t>(0x4E2D))),
              static_cast<std::uint16_t>(0x4E2D));
    EXPECT_EQ(mj2x_char_char_value(mj2x_char{0xFFFF}), 0xFFFF);
}

}  // namespace

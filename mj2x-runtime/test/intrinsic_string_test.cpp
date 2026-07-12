#include "intrinsic/intrinsic_string.hpp"

#include <gmock/gmock.h>

#include <cstdint>

namespace {

TEST(IntrinsicStringTest, Utf16ViewLengthAndCharAt)
{
    const std::uint16_t text[] = {'h', 'i', 0x4E2D};
    auto                view   = mj2x_utf16_view_of(text, 3);

    mj2x_string_status status = MJ2X_STRING_UNSUPPORTED_ESCAPE;
    EXPECT_EQ(mj2x_string_length(view, &status), 3);
    EXPECT_EQ(status, MJ2X_STRING_OK);

    EXPECT_EQ(mj2x_string_char_at(view, 0, &status), static_cast<std::uint16_t>('h'));
    EXPECT_EQ(status, MJ2X_STRING_OK);
    EXPECT_EQ(mj2x_string_char_at(view, 2, &status), static_cast<std::uint16_t>(0x4E2D));
    EXPECT_EQ(status, MJ2X_STRING_OK);
}

TEST(IntrinsicStringTest, EmptyViewMayUseNullData)
{
    auto view = mj2x_utf16_view_of(nullptr, 0);

    mj2x_string_status status = MJ2X_STRING_UNSUPPORTED_ESCAPE;
    EXPECT_EQ(mj2x_string_length(view, &status), 0);
    EXPECT_EQ(status, MJ2X_STRING_OK);
}

TEST(IntrinsicStringTest, InvalidAndOutOfBoundsFailClosed)
{
    auto invalid = mj2x_utf16_view_of(nullptr, 2);

    mj2x_string_status status = MJ2X_STRING_OK;
    EXPECT_EQ(mj2x_string_length(invalid, &status), 0);
    EXPECT_EQ(status, MJ2X_STRING_UNSUPPORTED_NULL);

    const std::uint16_t text[] = {'x'};
    auto                view   = mj2x_utf16_view_of(text, 1);
    EXPECT_EQ(mj2x_string_char_at(view, 1, &status), 0);
    EXPECT_EQ(status, MJ2X_STRING_UNSUPPORTED_BOUNDS);
}

TEST(IntrinsicStringTest, StringBuilderIsExplicitlyUnsupported)
{
    EXPECT_EQ(mj2x_stringbuilder_unsupported_status(), MJ2X_STRING_UNSUPPORTED_MUTABLE_BUILDER);
}

}   // namespace

#include <gmock/gmock.h>

#include "stringpool/string_pool.hpp"

#include <string>
#include <vector>

TEST(RuntimeStringPoolTest, EncodesAndDecodesUtf8WithEntryLabel)
{
    std::vector<std::uint8_t> rootKey = {0x10, 0x20, 0x30, 0x40};
    const std::string         plain   = "mj2x-regc-runtime";

    auto cipher = mj2x::runtime::stringpool::xorWithDerivedStream(
        rootKey,
        "sample.entry.1",
        reinterpret_cast<const std::uint8_t*>(plain.data()),
        plain.size());
    EXPECT_NE(std::string(cipher.begin(), cipher.end()), plain);

    mj2x::runtime::stringpool::StringPoolEntry entry{
        cipher.data(),
        cipher.size(),
        "sample.entry.1",
    };
    EXPECT_EQ(mj2x::runtime::stringpool::decodeUtf8(rootKey, entry), plain);
}

TEST(RuntimeStringPoolTest, DifferentLabelsProduceDifferentStreams)
{
    std::vector<std::uint8_t> rootKey = {1, 2, 3, 4};
    const std::string         plain   = "same text";

    auto left = mj2x::runtime::stringpool::xorWithDerivedStream(
        rootKey, "left", reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
    auto right = mj2x::runtime::stringpool::xorWithDerivedStream(
        rootKey, "right", reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
    EXPECT_NE(left, right);
}

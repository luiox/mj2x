#include <gmock/gmock.h>

#include "kernel/runtime_kernel.hpp"

#include <iomanip>
#include <sstream>
#include <vector>

namespace {

std::string hex(const std::vector<std::uint8_t>& data)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : data) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::vector<std::uint8_t> seq(std::uint8_t first, std::uint8_t lastInclusive)
{
    std::vector<std::uint8_t> out;
    for (std::uint8_t value = first;; ++value) {
        out.push_back(value);
        if (value == lastInclusive) break;
    }
    return out;
}

}   // namespace

TEST(RuntimeKernelTest, HkdfSha256MatchesRfc5869Vector)
{
    std::vector<std::uint8_t> ikm(22, 0x0b);
    auto                      salt = seq(0x00, 0x0c);
    auto                      info = seq(0xf0, 0xf9);

    auto okm = mj2x::runtime::kernel::hkdfSha256(salt, ikm, info, 42);
    EXPECT_EQ(hex(okm),
              "3cb25f25faacd57a90434f64d0362f2a"
              "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865");
}

TEST(RuntimeKernelTest, VerifyHmacRejectsChangedData)
{
    std::vector<std::uint8_t> key  = {1, 2, 3, 4};
    std::vector<std::uint8_t> data = {5, 6, 7};
    auto tag = mj2x::runtime::crypto::hmacSha256(key.data(), key.size(), data.data(), data.size());
    EXPECT_TRUE(mj2x::runtime::kernel::verifyHmacSha256(key, data, tag));
    data[1] ^= 0x55;
    EXPECT_FALSE(mj2x::runtime::kernel::verifyHmacSha256(key, data, tag));
}

TEST(RuntimeKernelTest, SecureWipeClearsBuffer)
{
    std::vector<std::uint8_t> secret = {9, 8, 7, 6};
    mj2x::runtime::kernel::secureWipe(secret.data(), secret.size());
    EXPECT_THAT(secret, ::testing::ElementsAre(0, 0, 0, 0));
}

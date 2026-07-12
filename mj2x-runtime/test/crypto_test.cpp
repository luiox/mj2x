#include <gmock/gmock.h>

#include "crypto/sha256.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string hex(const std::uint8_t* data, std::size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

std::vector<std::uint8_t> bytes(const std::string& text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}   // namespace

TEST(RuntimeCryptoTest, Sha256MatchesKnownVectors)
{
    auto empty = mj2x::runtime::crypto::sha256(nullptr, 0);
    EXPECT_EQ(hex(empty.data(), empty.size()),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");

    auto abcInput = bytes("abc");
    auto abc      = mj2x::runtime::crypto::sha256(abcInput);
    EXPECT_EQ(hex(abc.data(), abc.size()),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
}

TEST(RuntimeCryptoTest, HmacSha256MatchesKnownVector)
{
    auto key = bytes("key");
    auto msg = bytes("The quick brown fox jumps over the lazy dog");
    auto tag = mj2x::runtime::crypto::hmacSha256(key.data(), key.size(), msg.data(), msg.size());
    EXPECT_EQ(hex(tag.data(), tag.size()),
              "f7bc83f430538424b13298e6aa6fb143"
              "ef4d59a14946175997479dbc2d1a3cd8");
}

TEST(RuntimeCryptoTest, ConstantTimeEqualChecksContent)
{
    const std::uint8_t lhs[]   = {1, 2, 3};
    const std::uint8_t rhs[]   = {1, 2, 3};
    const std::uint8_t other[] = {1, 2, 4};
    EXPECT_TRUE(mj2x::runtime::crypto::constantTimeEqual(lhs, rhs, 3));
    EXPECT_FALSE(mj2x::runtime::crypto::constantTimeEqual(lhs, other, 3));
}

TEST(RuntimeCryptoTest, Aes128EncryptBlockMatchesKnownVector)
{
    mj2x::runtime::crypto::Aes128Key key = {
        0x00,
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0a,
        0x0b,
        0x0c,
        0x0d,
        0x0e,
        0x0f,
    };
    mj2x::runtime::crypto::AesBlock plain = {
        0x00,
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xaa,
        0xbb,
        0xcc,
        0xdd,
        0xee,
        0xff,
    };
    auto cipher = mj2x::runtime::crypto::aes128EncryptBlock(key, plain);
    EXPECT_EQ(hex(cipher.data(), cipher.size()), "69c4e0d86a7b0430d8cdb78070b4c55a");
}

TEST(RuntimeCryptoTest, Aes128CtrXorRoundTrips)
{
    mj2x::runtime::crypto::Aes128Key key = {
        0x2b,
        0x7e,
        0x15,
        0x16,
        0x28,
        0xae,
        0xd2,
        0xa6,
        0xab,
        0xf7,
        0x15,
        0x88,
        0x09,
        0xcf,
        0x4f,
        0x3c,
    };
    mj2x::runtime::crypto::AesBlock counter = {
        0xf0,
        0xf1,
        0xf2,
        0xf3,
        0xf4,
        0xf5,
        0xf6,
        0xf7,
        0xf8,
        0xf9,
        0xfa,
        0xfb,
        0xfc,
        0xfd,
        0xfe,
        0xff,
    };
    auto                      plain = bytes("runtime aes ctr data");
    std::vector<std::uint8_t> cipher(plain.size());
    std::vector<std::uint8_t> decoded(plain.size());

    mj2x::runtime::crypto::aes128CtrXor(key, counter, plain.data(), cipher.data(), plain.size());
    EXPECT_NE(cipher, plain);
    mj2x::runtime::crypto::aes128CtrXor(key, counter, cipher.data(), decoded.data(), cipher.size());
    EXPECT_EQ(decoded, plain);
}

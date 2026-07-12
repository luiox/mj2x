#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace mj2x::runtime::crypto {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::array<std::uint8_t, 256> kAesSbox = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

constexpr std::array<std::uint8_t, 11> kAesRcon = {
    0x00,
    0x01,
    0x02,
    0x04,
    0x08,
    0x10,
    0x20,
    0x40,
    0x80,
    0x1b,
    0x36,
};

std::uint32_t rotr(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

std::uint32_t loadBe32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
           (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}

void storeBe32(std::uint32_t value, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>(value >> 24u);
    out[1] = static_cast<std::uint8_t>(value >> 16u);
    out[2] = static_cast<std::uint8_t>(value >> 8u);
    out[3] = static_cast<std::uint8_t>(value);
}

struct Sha256State
{
    std::array<std::uint32_t, 8> h = {
        0x6a09e667u,
        0xbb67ae85u,
        0x3c6ef372u,
        0xa54ff53au,
        0x510e527fu,
        0x9b05688cu,
        0x1f83d9abu,
        0x5be0cd19u,
    };
    std::array<std::uint8_t, kSha256BlockSize> buffer{};
    std::uint64_t                              totalSize = 0;
    std::size_t                                buffered  = 0;
};

void compress(Sha256State& state, const std::uint8_t block[kSha256BlockSize])
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = loadBe32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3u);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10u);
        w[i]                   = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = state.h[0];
    auto b = state.h[1];
    auto c = state.h[2];
    auto d = state.h[3];
    auto e = state.h[4];
    auto f = state.h[5];
    auto g = state.h[6];
    auto h = state.h[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch    = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
    state.h[5] += f;
    state.h[6] += g;
    state.h[7] += h;
}

void update(Sha256State& state, const std::uint8_t* data, std::size_t size)
{
    if (size == 0) return;
    state.totalSize += static_cast<std::uint64_t>(size);
    std::size_t offset = 0;

    if (state.buffered != 0) {
        const std::size_t take = std::min(size, kSha256BlockSize - state.buffered);
        std::memcpy(state.buffer.data() + state.buffered, data, take);
        state.buffered += take;
        offset += take;
        if (state.buffered == kSha256BlockSize) {
            compress(state, state.buffer.data());
            state.buffered = 0;
        }
    }

    while (offset + kSha256BlockSize <= size) {
        compress(state, data + offset);
        offset += kSha256BlockSize;
    }

    if (offset < size) {
        state.buffered = size - offset;
        std::memcpy(state.buffer.data(), data + offset, state.buffered);
    }
}

Sha256Digest finish(Sha256State& state)
{
    const std::uint64_t bitSize    = state.totalSize * 8u;
    state.buffer[state.buffered++] = 0x80u;
    if (state.buffered > 56) {
        while (state.buffered < kSha256BlockSize) state.buffer[state.buffered++] = 0;
        compress(state, state.buffer.data());
        state.buffered = 0;
    }
    while (state.buffered < 56) state.buffer[state.buffered++] = 0;
    for (int i = 7; i >= 0; --i) {
        state.buffer[state.buffered++] = static_cast<std::uint8_t>(bitSize >> (i * 8));
    }
    compress(state, state.buffer.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state.h.size(); ++i) {
        storeBe32(state.h[i], digest.data() + i * 4);
    }
    return digest;
}

std::uint8_t xtime(std::uint8_t value)
{
    return static_cast<std::uint8_t>((value << 1u) ^ ((value & 0x80u) ? 0x1bu : 0x00u));
}

void aesAddRoundKey(std::uint8_t state[16], const std::array<std::uint8_t, 176>& roundKeys,
                    int round)
{
    const std::size_t offset = static_cast<std::size_t>(round) * 16u;
    for (std::size_t i = 0; i < 16; ++i) {
        state[i] ^= roundKeys[offset + i];
    }
}

void aesSubBytes(std::uint8_t state[16])
{
    for (std::size_t i = 0; i < 16; ++i) {
        state[i] = kAesSbox[state[i]];
    }
}

void aesShiftRows(std::uint8_t state[16])
{
    std::uint8_t t = state[1];
    state[1]       = state[5];
    state[5]       = state[9];
    state[9]       = state[13];
    state[13]      = t;

    t              = state[2];
    std::uint8_t u = state[6];
    state[2]       = state[10];
    state[6]       = state[14];
    state[10]      = t;
    state[14]      = u;

    t         = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7]  = state[3];
    state[3]  = t;
}

void aesMixColumns(std::uint8_t state[16])
{
    for (std::size_t c = 0; c < 4; ++c) {
        std::uint8_t*      col = state + c * 4;
        const std::uint8_t a0  = col[0];
        const std::uint8_t a1  = col[1];
        const std::uint8_t a2  = col[2];
        const std::uint8_t a3  = col[3];
        const std::uint8_t x   = static_cast<std::uint8_t>(a0 ^ a1 ^ a2 ^ a3);
        col[0] ^= x ^ xtime(static_cast<std::uint8_t>(a0 ^ a1));
        col[1] ^= x ^ xtime(static_cast<std::uint8_t>(a1 ^ a2));
        col[2] ^= x ^ xtime(static_cast<std::uint8_t>(a2 ^ a3));
        col[3] ^= x ^ xtime(static_cast<std::uint8_t>(a3 ^ a0));
    }
}

std::array<std::uint8_t, 176> aesExpandKey(const Aes128Key& key)
{
    std::array<std::uint8_t, 176> roundKeys{};
    std::memcpy(roundKeys.data(), key.data(), key.size());

    std::size_t                 bytes     = 16;
    int                         rconIndex = 1;
    std::array<std::uint8_t, 4> temp{};
    while (bytes < roundKeys.size()) {
        for (std::size_t i = 0; i < 4; ++i) temp[i] = roundKeys[bytes - 4 + i];
        if (bytes % 16 == 0) {
            const std::uint8_t first = temp[0];
            temp[0] = static_cast<std::uint8_t>(kAesSbox[temp[1]] ^ kAesRcon[rconIndex++]);
            temp[1] = kAesSbox[temp[2]];
            temp[2] = kAesSbox[temp[3]];
            temp[3] = kAesSbox[first];
        }
        for (std::size_t i = 0; i < 4; ++i) {
            roundKeys[bytes] = static_cast<std::uint8_t>(roundKeys[bytes - 16] ^ temp[i]);
            ++bytes;
        }
    }
    return roundKeys;
}

void incrementCounter(AesBlock& counter)
{
    for (int i = static_cast<int>(counter.size()) - 1; i >= 0; --i) {
        if (++counter[static_cast<std::size_t>(i)] != 0) break;
    }
}

}   // namespace

Sha256Digest sha256(const std::uint8_t* data, std::size_t size)
{
    Sha256State state;
    update(state, data, size);
    return finish(state);
}

Sha256Digest hmacSha256(const std::uint8_t* key, std::size_t keySize, const std::uint8_t* data,
                        std::size_t dataSize)
{
    std::array<std::uint8_t, kSha256BlockSize> keyBlock{};
    if (keySize > kSha256BlockSize) {
        auto hashedKey = sha256(key, keySize);
        std::memcpy(keyBlock.data(), hashedKey.data(), hashedKey.size());
    }
    else if (keySize != 0) {
        std::memcpy(keyBlock.data(), key, keySize);
    }

    std::array<std::uint8_t, kSha256BlockSize> inner{};
    std::array<std::uint8_t, kSha256BlockSize> outer{};
    for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
        inner[i] = keyBlock[i] ^ 0x36u;
        outer[i] = keyBlock[i] ^ 0x5cu;
    }

    Sha256State innerState;
    update(innerState, inner.data(), inner.size());
    update(innerState, data, dataSize);
    auto innerDigest = finish(innerState);

    Sha256State outerState;
    update(outerState, outer.data(), outer.size());
    update(outerState, innerDigest.data(), innerDigest.size());
    return finish(outerState);
}

bool constantTimeEqual(const std::uint8_t* lhs, const std::uint8_t* rhs, std::size_t size)
{
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < size; ++i) {
        diff |= static_cast<std::uint8_t>(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

Sha256Digest sha256(const std::vector<std::uint8_t>& data)
{
    return sha256(data.data(), data.size());
}

AesBlock aes128EncryptBlock(const Aes128Key& key, const AesBlock& block)
{
    const auto roundKeys = aesExpandKey(key);
    AesBlock   out       = block;

    aesAddRoundKey(out.data(), roundKeys, 0);
    for (int round = 1; round <= 9; ++round) {
        aesSubBytes(out.data());
        aesShiftRows(out.data());
        aesMixColumns(out.data());
        aesAddRoundKey(out.data(), roundKeys, round);
    }
    aesSubBytes(out.data());
    aesShiftRows(out.data());
    aesAddRoundKey(out.data(), roundKeys, 10);
    return out;
}

void aes128CtrXor(const Aes128Key& key, AesBlock counter, const std::uint8_t* input,
                  std::uint8_t* output, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto        stream = aes128EncryptBlock(key, counter);
        const std::size_t take   = std::min<std::size_t>(stream.size(), size - offset);
        for (std::size_t i = 0; i < take; ++i) {
            output[offset + i] = static_cast<std::uint8_t>(input[offset + i] ^ stream[i]);
        }
        offset += take;
        incrementCounter(counter);
    }
}

}   // namespace mj2x::runtime::crypto

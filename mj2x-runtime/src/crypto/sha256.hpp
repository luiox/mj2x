/// @defgroup runtime_crypto 运行时加密原语
///
/// 为 R3 提供可移植加密底层。保持在 mj2x 内部，使保护样本不继承
/// OpenSSL/Windows CNG/JDK provider 依赖。
///
/// ## 设计
///
/// 首个切片实现 SHA-256、HMAC-SHA256、AES-128 block 加密、AES-128 CTR XOR
/// 和常量时间比较。SHA/HMAC 供给运行时内核的 HKDF 和资源验证。
/// AES-CTR 为未来资源和字符串池格式提供无依赖的流原语。
/// 模块是值导向的：调用者传入字节缓冲区并接收固定大小输出。
/// 秘密材料的所有权和擦除保持在外部内核模块中。

/// @file sha256.hpp
/// @brief Small SHA-256 and HMAC-SHA256 primitives for the mj2x R3 runtime.
///
/// @details
/// The runtime must not depend on OpenSSL or platform crypto providers because
/// protected samples need deterministic, portable behavior across developer
/// machines.  This header exposes value-oriented helpers only: callers pass byte
/// spans and receive fixed-size digests.  Secret material ownership stays with
/// the caller; use the kernel wipe helper for buffers that must be cleared.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mj2x::runtime::crypto {

/// @brief SHA-256 digest byte count.
constexpr std::size_t kSha256DigestSize = 32;

/// @brief SHA-256 block byte count.
constexpr std::size_t kSha256BlockSize = 64;

/// @brief Fixed-size SHA-256 digest container.
using Sha256Digest = std::array<std::uint8_t, kSha256DigestSize>;

/// @brief AES-128 key byte count.
constexpr std::size_t kAes128KeySize = 16;

/// @brief AES block byte count.
constexpr std::size_t kAesBlockSize = 16;

/// @brief Fixed-size AES-128 key container.
using Aes128Key = std::array<std::uint8_t, kAes128KeySize>;

/// @brief Fixed-size AES block container.
using AesBlock = std::array<std::uint8_t, kAesBlockSize>;

/// @brief Compute SHA-256 over a contiguous byte buffer.
///
/// @param data Pointer to the first byte.  May be null when `size == 0`.
/// @param size Number of bytes to hash.
/// @return The 32-byte SHA-256 digest.
Sha256Digest sha256(const std::uint8_t* data, std::size_t size);

/// @brief Compute HMAC-SHA256.
///
/// @param key Pointer to key bytes.  May be null when `keySize == 0`.
/// @param keySize Number of key bytes.
/// @param data Pointer to message bytes.  May be null when `dataSize == 0`.
/// @param dataSize Number of message bytes.
/// @return The 32-byte HMAC-SHA256 tag.
Sha256Digest hmacSha256(const std::uint8_t* key, std::size_t keySize, const std::uint8_t* data,
                        std::size_t dataSize);

/// @brief Constant-time equality check for equal-length byte buffers.
///
/// @return true when both buffers contain identical bytes.
bool constantTimeEqual(const std::uint8_t* lhs, const std::uint8_t* rhs, std::size_t size);

/// @brief Convenience overload for vector-backed data.
Sha256Digest sha256(const std::vector<std::uint8_t>& data);

/// @brief Encrypt one 16-byte block with AES-128.
///
/// @param key 16-byte AES-128 key.
/// @param block 16-byte plaintext block.
/// @return Ciphertext block.
AesBlock aes128EncryptBlock(const Aes128Key& key, const AesBlock& block);

/// @brief XOR data with AES-128 CTR keystream.
///
/// @details
/// The counter block is incremented as a big-endian 128-bit integer after each
/// block.  Input and output may point to the same buffer.
void aes128CtrXor(const Aes128Key& key, AesBlock counter, const std::uint8_t* input,
                  std::uint8_t* output, std::size_t size);

}   // namespace mj2x::runtime::crypto

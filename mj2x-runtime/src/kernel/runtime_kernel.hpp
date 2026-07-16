/// @defgroup runtime_kernel 运行时内核
///
/// 密钥派生、资源完整性和秘密内存卫生的小型共享运行时层。
/// 其他运行时模块应使用它而不是各自发明本地密钥调度。
///
/// ## 设计
///
/// 使用 HKDF-SHA256 加显式 salt/input key material/info 字节。
/// 使 RegC、string pool 和未来受保护资源拥有公共域分离模式，
/// 同时保持测试确定性。HMAC 验证使用 crypto 模块的常量时间比较。
/// 临时秘密缓冲区通过 volatile 字节存储清除。

/// @file runtime_kernel.hpp
/// @brief R3 runtime key-derivation and memory hygiene helpers.
///
/// @details
/// The kernel module owns small, deterministic primitives that other runtime
/// modules share: HKDF-SHA256 for per-sample/per-string keys, HMAC verification
/// for protected resources, and volatile wiping for temporary secret buffers.
/// It deliberately does not own process-global state yet; callers pass all seed
/// and context bytes explicitly so tests and generated samples stay reproducible.
#pragma once

#include "crypto/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mj2x::runtime::kernel {

/// @brief Overwrite a byte buffer through volatile stores.
///
/// @param data Pointer to bytes to clear.  A null pointer is ignored.
/// @param size Number of bytes to clear.
void secureWipe(void* data, std::size_t size);

/// @brief HKDF-SHA256 extract step.
///
/// @param salt Optional salt bytes.
/// @param inputKeyMaterial Input key material bytes.
/// @return Pseudorandom key suitable for `hkdfExpand`.
crypto::Sha256Digest hkdfExtract(const std::vector<std::uint8_t>& salt,
                                 const std::vector<std::uint8_t>& inputKeyMaterial);

/// @brief HKDF-SHA256 expand step.
///
/// @param prk Pseudorandom key from `hkdfExtract`.
/// @param info Context string for domain separation.
/// @param outputSize Number of output bytes required.
/// @return Derived key stream of exactly `outputSize` bytes.
std::vector<std::uint8_t> hkdfExpand(const crypto::Sha256Digest&      prk,
                                     const std::vector<std::uint8_t>& info, std::size_t outputSize);

/// @brief Convenience HKDF-SHA256 extract+expand.
std::vector<std::uint8_t> hkdfSha256(const std::vector<std::uint8_t>& salt,
                                     const std::vector<std::uint8_t>& inputKeyMaterial,
                                     const std::vector<std::uint8_t>& info, std::size_t outputSize);

/// @brief Verify an HMAC-SHA256 tag in constant time.
bool verifyHmacSha256(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& data,
                      const crypto::Sha256Digest& expected);

/// @brief Convert a narrow ASCII label to bytes for HKDF/HMAC domain separation.
std::vector<std::uint8_t> labelBytes(const std::string& label);

}   // namespace mj2x::runtime::kernel

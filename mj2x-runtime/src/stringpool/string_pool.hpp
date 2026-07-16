/// @defgroup runtime_stringpool 运行时字符串池
///
/// 为 RegC 和保护样本提供第一个 R3 string-pool 解码器。
/// 从生成的 native 代码中移除明文和静态 XOR 密钥。
///
/// ## 设计
///
/// 每个字符串条目有加密字节和标签。运行时使用 HKDF-SHA256 从运行时根密钥和条目
/// 标签推导出每条目密钥流，然后与载荷 XOR。相同变换是对称的，因此测试和 fixture
/// 生成可创建确定性的加密条目。
///
/// ## 边界
///
/// 当前格式有意保持小型：无压缩、无记录表 ABI、无认证元数据。
/// 未来格式可在保持相同标签基密钥分离的同时添加这些特性。

/// @file string_pool.hpp
/// @brief Key-derived string-pool decoder for protected RegC/runtime samples.
///
/// @details
/// R3 string pools should not store clear text or a static XOR key in generated
/// C.  This module derives a per-entry byte stream from HKDF-SHA256 and XORs it
/// with the encrypted payload.  The format remains intentionally simple for the
/// first runtime slice; later samples can add compression or authenticated
/// records while reusing the same domain-separated key derivation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mj2x::runtime::stringpool {

/// @brief Inputs required to decode one encrypted string-pool entry.
struct StringPoolEntry
{
    const std::uint8_t* cipherText = nullptr;
    std::size_t         size       = 0;
    std::string         label;
};

/// @brief Decode one UTF-8 string-pool entry.
///
/// @param rootKey Runtime root key material.
/// @param entry Encrypted bytes and label.
/// @return Decoded UTF-8 bytes as a string.
std::string decodeUtf8(const std::vector<std::uint8_t>& rootKey, const StringPoolEntry& entry);

/// @brief Encode/decode helper used by tests and sample generation.
///
/// @details
/// XOR with the same derived stream is symmetric, so this helper can produce
/// deterministic fixtures without a separate offline tool.
std::vector<std::uint8_t> xorWithDerivedStream(const std::vector<std::uint8_t>& rootKey,
                                               const std::string& label, const std::uint8_t* input,
                                               std::size_t size);

}   // namespace mj2x::runtime::stringpool

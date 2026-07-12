/**
 * @file string_pool.hpp
 * @brief Key-derived string-pool decoder for protected RegC/runtime samples.
 *
 * @details
 * R3 string pools should not store clear text or a static XOR key in generated
 * C.  This module derives a per-entry byte stream from HKDF-SHA256 and XORs it
 * with the encrypted payload.  The format remains intentionally simple for the
 * first runtime slice; later samples can add compression or authenticated
 * records while reusing the same domain-separated key derivation.
 */
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

/**
 * @brief Decode one UTF-8 string-pool entry.
 *
 * @param rootKey Runtime root key material.
 * @param entry Encrypted bytes and label.
 * @return Decoded UTF-8 bytes as a string.
 */
std::string decodeUtf8(const std::vector<std::uint8_t>& rootKey, const StringPoolEntry& entry);

/**
 * @brief Encode/decode helper used by tests and sample generation.
 *
 * @details
 * XOR with the same derived stream is symmetric, so this helper can produce
 * deterministic fixtures without a separate offline tool.
 */
std::vector<std::uint8_t> xorWithDerivedStream(const std::vector<std::uint8_t>& rootKey,
                                               const std::string& label, const std::uint8_t* input,
                                               std::size_t size);

}   // namespace mj2x::runtime::stringpool

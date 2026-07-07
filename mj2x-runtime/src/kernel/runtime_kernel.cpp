#include "runtime_kernel.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mj2x::runtime::kernel {

void secureWipe(void* data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    auto* p = static_cast<volatile std::uint8_t*>(data);
    while (size-- != 0) {
        *p++ = 0;
    }
}

crypto::Sha256Digest hkdfExtract(const std::vector<std::uint8_t>& salt,
                                 const std::vector<std::uint8_t>& inputKeyMaterial) {
    std::vector<std::uint8_t> defaultSalt;
    const auto& effectiveSalt = salt.empty() ? defaultSalt : salt;
    defaultSalt.resize(crypto::kSha256DigestSize, 0);
    return crypto::hmacSha256(
        effectiveSalt.data(),
        effectiveSalt.size(),
        inputKeyMaterial.data(),
        inputKeyMaterial.size());
}

std::vector<std::uint8_t> hkdfExpand(const crypto::Sha256Digest& prk,
                                     const std::vector<std::uint8_t>& info,
                                     std::size_t outputSize) {
    std::vector<std::uint8_t> output;
    output.reserve(outputSize);

    std::vector<std::uint8_t> previous;
    std::uint8_t counter = 1;
    while (output.size() < outputSize) {
        std::vector<std::uint8_t> block;
        block.reserve(previous.size() + info.size() + 1);
        block.insert(block.end(), previous.begin(), previous.end());
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter++);

        auto digest = crypto::hmacSha256(prk.data(), prk.size(), block.data(), block.size());
        previous.assign(digest.begin(), digest.end());
        const std::size_t take = std::min(previous.size(), outputSize - output.size());
        output.insert(output.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(take));

        // RFC 5869 caps SHA-256 HKDF output at 255 blocks.  Stop deterministically
        // if a caller requests more than the runtime format should ever need.
        if (counter == 0 && output.size() < outputSize) break;
    }

    secureWipe(previous.data(), previous.size());
    return output;
}

std::vector<std::uint8_t> hkdfSha256(const std::vector<std::uint8_t>& salt,
                                     const std::vector<std::uint8_t>& inputKeyMaterial,
                                     const std::vector<std::uint8_t>& info,
                                     std::size_t outputSize) {
    auto prk = hkdfExtract(salt, inputKeyMaterial);
    auto out = hkdfExpand(prk, info, outputSize);
    secureWipe(prk.data(), prk.size());
    return out;
}

bool verifyHmacSha256(const std::vector<std::uint8_t>& key,
                      const std::vector<std::uint8_t>& data,
                      const crypto::Sha256Digest& expected) {
    auto actual = crypto::hmacSha256(key.data(), key.size(), data.data(), data.size());
    const bool ok = crypto::constantTimeEqual(actual.data(), expected.data(), actual.size());
    secureWipe(actual.data(), actual.size());
    return ok;
}

std::vector<std::uint8_t> labelBytes(const std::string& label) {
    return std::vector<std::uint8_t>(label.begin(), label.end());
}

}  // namespace mj2x::runtime::kernel

#include "string_pool.hpp"

#include "kernel/runtime_kernel.hpp"

namespace mj2x::runtime::stringpool {

std::vector<std::uint8_t> xorWithDerivedStream(const std::vector<std::uint8_t>& rootKey,
                                               const std::string& label,
                                               const std::uint8_t* input,
                                               std::size_t size) {
    std::vector<std::uint8_t> salt = kernel::labelBytes("mj2x:r3:stringpool");
    std::vector<std::uint8_t> info = kernel::labelBytes(label);
    auto stream = kernel::hkdfSha256(salt, rootKey, info, size);

    std::vector<std::uint8_t> output(size);
    for (std::size_t i = 0; i < size; ++i) {
        output[i] = static_cast<std::uint8_t>(input[i] ^ stream[i]);
    }
    kernel::secureWipe(stream.data(), stream.size());
    return output;
}

std::string decodeUtf8(const std::vector<std::uint8_t>& rootKey,
                       const StringPoolEntry& entry) {
    if (entry.cipherText == nullptr && entry.size != 0) return {};
    auto plain = xorWithDerivedStream(rootKey, entry.label, entry.cipherText, entry.size);
    return std::string(plain.begin(), plain.end());
}

}  // namespace mj2x::runtime::stringpool

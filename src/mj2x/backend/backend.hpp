#pragma once

#include <string_view>
#include <vector>
#include <cstdint>

namespace mj2x {

struct BackendResult {
    enum class Kind { NONE, C_SOURCE };
    Kind kind = Kind::NONE;
    std::vector<uint8_t> data;
    std::string text;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string_view name() const = 0;
};

} // namespace mj2x

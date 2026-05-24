#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mj2x {

struct BackendResult {
    enum class Kind { NONE, C_SOURCE, BINARY };
    Kind kind = Kind::NONE;
    std::vector<uint8_t> data;
    std::string text;
};

} // namespace mj2x

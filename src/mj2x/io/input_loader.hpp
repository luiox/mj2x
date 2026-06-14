#pragma once

#include "mj2x/core/compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

class InputLoader {
public:
    InputLoader();
    ~InputLoader();

    std::unique_ptr<CompilerIndex> load(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mj2x


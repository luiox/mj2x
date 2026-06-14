#pragma once

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

class Preprocessor {
public:
    virtual ~Preprocessor() = default;
    virtual void process(CompilerIndex& index) = 0;
};

} // namespace mj2x


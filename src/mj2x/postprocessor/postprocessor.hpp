#pragma once

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

class Postprocessor {
public:
    virtual ~Postprocessor() = default;
    virtual void process(CompilerIndex& index) = 0;
};

} // namespace mj2x

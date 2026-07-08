#pragma once

#include "mj2x/postprocessor/postprocessor.hpp"

namespace mj2x {

class CLoaderPostprocessor : public Postprocessor {
public:
    void process(CompilerIndex& index) override;
};

} // namespace mj2x

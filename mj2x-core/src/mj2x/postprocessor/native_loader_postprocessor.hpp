#pragma once

#include "mj2x/postprocessor/postprocessor.hpp"

#include <string>

namespace mj2x {

class NativeLoaderPostprocessor : public Postprocessor
{
public:
    explicit NativeLoaderPostprocessor(std::string resourcePath);

    void process(CompilerIndex& index) override;

private:
    std::string resourcePath_;
};

}   // namespace mj2x

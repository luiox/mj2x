#pragma once

#include "mj2x/core/compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

class OutputWriter {
public:
    OutputWriter();
    ~OutputWriter();

    void write(const std::string& path, CompilerIndex& index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mj2x

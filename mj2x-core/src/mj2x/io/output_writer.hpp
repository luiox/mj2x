#pragma once

#include "mj2x/core/compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

class OutputWriter
{
public:
    OutputWriter();
    ~OutputWriter();

    /// @brief Write a compiler index back to `.class` or `.jar` output.
    ///
    /// @details
    /// JAR output preserves the input entry order and non-class resources while
    /// replacing compiled class entries with rewritten class bytes.  MVP output
    /// uses stored ZIP entries, so signed JAR metadata is preserved as bytes but
    /// existing signatures are not expected to remain valid after class changes.
    void write(const std::string& path, CompilerIndex& index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace mj2x

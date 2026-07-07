#pragma once

#include "mj2x/core/compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

class InputLoader {
public:
    InputLoader();
    ~InputLoader();

    /**
     * @brief Load a `.class` file or a `.jar` archive into a compiler index.
     *
     * @details
     * Single-class input produces one `CompilerClass`.  JAR input parses every
     * class entry and records non-class entries as passthrough resources so the
     * output writer can preserve archive-level MVP behavior.  Signed-JAR
     * signature metadata and JARs without root class entries fail closed in the
     * MVP path because mj2x cannot preserve signature validity after class
     * rewriting.
     */
    std::unique_ptr<CompilerIndex> load(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mj2x


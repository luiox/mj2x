/**
 * @file include_set.hpp
 * @brief Deduplicating include directive manager for generated C TUs.
 *
 * @details
 * Each generated translation unit previously hard-coded its `#include` block
 * as inline string literals scattered across `finalize` / `emitHelperPrelude`.
 * Adding a header meant remembering every emit site, and duplicate or
 * mis-ordered includes were easy to introduce.
 *
 * `IncludeSet` records system (`<jni.h>`) and project (`"regc/abi.hpp"`)
 * headers separately, deduplicates them, and renders a stable block with
 * system headers first.  Backends build the set during emission and render it
 * once when assembling the final source.
 */
#pragma once

#include <set>
#include <string>
#include <string_view>

namespace mj2x::codegen {

/// @brief Deduplicating, ordered include directive manager.
class IncludeSet {
public:
    /// @brief Add an angle-bracket system header such as `jni.h` (no brackets).
    void addSystem(std::string_view header) {
        systemHeaders_.insert(std::string(header));
    }

    /// @brief Add a quote project header such as `regc/abi.hpp` (no quotes).
    void addProject(std::string_view header) {
        projectHeaders_.insert(std::string(header));
    }

    /// @brief Render the `#include` block: system headers first, then project.
    /// @details Each header appears once.  A trailing newline separates the
    ///          block from the following source.  Returns an empty string when
    ///          no headers were registered.
    std::string render() const {
        std::string out;
        for (const auto& h : systemHeaders_) {
            out += "#include <";
            out += h;
            out += ">\n";
        }
        for (const auto& h : projectHeaders_) {
            out += "#include \"";
            out += h;
            out += "\"\n";
        }
        return out;
    }

    /// @brief True if neither system nor project headers have been added.
    bool empty() const { return systemHeaders_.empty() && projectHeaders_.empty(); }

private:
    std::set<std::string> systemHeaders_;
    std::set<std::string> projectHeaders_;
};

}  // namespace mj2x::codegen

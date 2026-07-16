/// @file c_source.hpp
/// @brief Indent-aware C source stream for backends that emit C/C++.
///
/// @details
/// Generated C translation units used to be assembled by writing into a raw
/// `std::ostringstream` with hand-counted leading spaces (`out << "    ..."`).
/// That made indentation easy to drift, blocks hard to follow, and diffs noisy.
///
/// `CSourceStream` owns the current indent level so emitters write logical
/// content (`emitLine`) instead of literal whitespace.  Raw fragments that must
/// not be re-indented (e.g. an externally formatted block) still have an escape
/// hatch via `emitRaw`.  The stream is a drop-in sink: callers flush it to a
/// string when the translation unit is complete.
#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace mj2x::codegen {

/// @brief Indent-aware sink for generated C/C++ source text.
class CSourceStream
{
public:
    /// @brief Append one logical line at the current indent, terminated by `\n`.
    void emitLine(std::string_view line)
    {
        writeIndent();
        out_ << line << '\n';
    }

    /// @brief Append text verbatim with no indent and no trailing newline.
    /// @details Use for pre-formatted blocks or fragments where the caller
    ///          controls all whitespace.
    void emitRaw(std::string_view text) { out_ << text; }

    /// @brief Increase the indent level by one step (four spaces per step).
    void indent() { ++indentLevel_; }

    /// @brief Decrease the indent level, clamped at zero.
    void dedent()
    {
        if (indentLevel_ > 0) --indentLevel_;
    }

    /// @brief Render the accumulated source as a single string.
    std::string str() const { return out_.str(); }

    /// @brief Current indent level (number of four-space steps).
    int indentLevel() const { return indentLevel_; }

private:
    void writeIndent()
    {
        for (int i = 0; i < indentLevel_; ++i) out_ << "    ";
    }

    std::ostringstream out_;
    int                indentLevel_ = 0;
};

}   // namespace mj2x::codegen

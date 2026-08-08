/// @file reg_dump.hpp
/// @brief Text dump for Register IR tests and review.
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include "libca/str/utf8_string.hpp"

#include <string>

namespace mj2x {

/// @brief Dump a RegFunction without JVM stack-machine anchor names.
std::string dumpRegIR(const RegFunction& fn);

/// @brief Render one RegIR instruction as a single-line comment body.
/// @details No trailing newline.  JVM symbol bytes are escaped (ASCII-safe,
///          traceable) so the text is safe to embed in generated comments.
///          RegC 的 `RegCCommentLevel::Detailed` 直接复用本渲染：每条指令生成的
///          C 代码前以 `// ` 前缀注入本行（见 reg_codegen_syntax.md §4.2）。
ca::str::Utf8String regInstructionLine(const RegInstruction& ins);

/// @brief Render a block terminator as comment-ready lines, each ending with '\n'.
/// @details RegC Detailed 级把每行加 `// ` 前缀作为终结器溯源注释注入（多行，
///          因终结器含 successor 列表）。
ca::str::Utf8String regTerminatorLines(const RegBlock& block);

}   // namespace mj2x

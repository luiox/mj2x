/// @file reg_dce.hpp
/// @brief Conservative dead-code elimination for register IR.
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include "libca/core/datatype.hpp"

namespace mj2x {

/// @brief Remove unused, non-throwing pure register instructions.
///
/// @details
/// The pass preserves every instruction with an observable effect and treats
/// terminators, successor copies, and exception-edge copies as register uses.
/// It iterates to a fixed point so a dead producer chain is removed together.
/// Division, remainder, and reference casts are intentionally retained because
/// their Java semantics may throw even when their result is unused.
ca::usize eliminateDeadRegInstructions(RegFunction& function);

}   // namespace mj2x

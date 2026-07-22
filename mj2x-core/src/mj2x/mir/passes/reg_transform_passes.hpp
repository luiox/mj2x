/// @file reg_transform_passes.hpp
/// @brief R4 protection-oriented transforms for Register IR.
///
/// @details
/// The passes operate on stackless RegIR before RegC emission. They preserve
/// edge copies and effect ordering, and only flatten control flow when the
/// complete function has no exception or observable-effect path.
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include "libca/core/datatype.hpp"

namespace mj2x {

/// @brief Select which R4 transforms are applied to a RegIR function.
struct RegTransformOptions
{
    bool enableMba                   = true;
    bool enableOpaquePredicates      = true;
    bool enableControlFlowFlattening = true;
};

/// @brief Per-function transform counters used by tests and diagnostics.
struct RegTransformStats
{
    ca::usize mbaRewrites          = 0;
    ca::usize opaquePredicates     = 0;
    ca::usize cffEdgeSetters       = 0;
    bool      controlFlowFlattened = false;
};

/// @brief Apply the enabled R4 transforms to one RegIR function.
///
/// MBA rewrites pure i32/i64 addition with an equivalent bitwise identity.
/// Opaque predicates are merged into integer/boolean branches. CFF replaces
/// eligible pure CFG edges with state-dispatcher transitions. Functions with
/// exceptions, memory access, calls, barriers, throw terminators, or malformed
/// CFG targets are deliberately excluded from CFF.
RegTransformStats applyRegTransformPasses(RegFunction&               function,
                                          const RegTransformOptions& options = {});

}   // namespace mj2x

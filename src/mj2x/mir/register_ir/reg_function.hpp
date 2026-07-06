/**
 * @file reg_function.hpp
 * @brief Register IR function container.
 *
 * @details
 * RegFunction owns all RegBlocks for one Java method.  It keeps the original
 * SSA function symbol so later RegC emission can reuse owner/name/descriptor
 * metadata without re-reading bytecode structures.
 */
#pragma once

#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/ssa/ssa_function.hpp"

#include <memory>
#include <vector>

namespace mj2x {

/// @brief L3 register-form function produced from one SSAFunction.
struct RegFunction {
    SSAFunctionSymbol symbol;
    std::vector<RegValue> parameters;
    std::vector<std::unique_ptr<RegBlock>> blocks;
    RegBlock* entry = nullptr;
    /**
     * @brief Exception regions lowered from SSA.
     *
     * @details
     * Each region preserves exception-table order.  RegC uses this data to
     * dispatch pending JNI exceptions from protected blocks into the matching
     * handler while materializing the handler's synthetic exception register.
     */
    std::vector<RegExceptionRegion> exceptionRegions;
};

/**
 * @brief Validate RegIR structural invariants required before RegC.
 *
 * @details
 * This currently verifies the invariant that RegIR exposes no JVM stack-slot
 * operand kind.  Because the operand enum has no such kind, the check also acts
 * as a traversal smoke test over every instruction, terminator, and edge copy.
 */
bool validateRegIRNoStackSlots(const RegFunction& fn, std::string& reason);

} // namespace mj2x

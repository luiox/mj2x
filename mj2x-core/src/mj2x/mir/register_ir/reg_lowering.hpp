/**
 * @file reg_lowering.hpp
 * @brief Lower block-argument SSA into flat Register IR.
 *
 * @details
 * RegLowering assigns every non-literal SSA value to a virtual register and
 * eliminates block arguments into explicit edge copies.  It does not perform
 * optimization or physical register allocation; RegC can treat each RegId as a
 * C local variable in the next stage.
 */
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/ssa/ssa_function.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief SSA to RegIR lowering entry point.
class RegLowering
{
public:
    /**
     * @brief Convert a stackless SSAFunction into RegIR.
     *
     * @param fn Source SSA function. It must outlive the call only.
     * @param reason Receives a fail-closed reason on malformed SSA.
     * @return Owned RegFunction, or nullptr when the SSA graph is invalid.
     */
    std::unique_ptr<RegFunction> lower(const SSAFunction& fn, std::string& reason);
};

}   // namespace mj2x

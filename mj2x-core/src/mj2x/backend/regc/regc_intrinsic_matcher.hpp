/**
 * @file regc_intrinsic_matcher.hpp
 * @brief Stable call-site view used by the RegC intrinsic registry.
 */
#pragma once

#include "mj2x/mir/register_ir/reg_instruction.hpp"

#include <cstddef>
#include <string>

namespace mj2x {

/**
 * @brief Normalized JVM call signature for RegC intrinsic lookup.
 *
 * @details
 * The matcher deliberately preserves the original invoke opcode and argument
 * count.  Registry decisions can then distinguish "not an intrinsic" from
 * "known intrinsic shape but malformed RegIR arguments" and report a useful
 * unsupported reason.
 */
struct RegCIntrinsicCall {
    std::string owner;
    std::string name;
    std::string desc;
    int invokeOpcode = 0;
    std::size_t argCount = 0;

    /**
     * @brief Return true when the bytecode call is INVOKESTATIC.
     */
    bool isStatic() const;

    /**
     * @brief Human-readable `owner.name desc` signature for diagnostics.
     */
    std::string displayName() const;
};

/**
 * @brief Extracts intrinsic lookup keys from RegIR call instructions.
 */
class RegCIntrinsicCallMatcher {
public:
    /**
     * @brief Build a normalized call-site view from one RegIR call.
     */
    static RegCIntrinsicCall fromInstruction(const RegInstruction& ins);
};

} // namespace mj2x

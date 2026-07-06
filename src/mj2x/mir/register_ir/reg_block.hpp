/**
 * @file reg_block.hpp
 * @brief Register IR basic blocks and terminators.
 *
 * @details
 * RegBlock has no block-argument list.  SSA block arguments are lowered into
 * RegCopy transfers stored on the predecessor edge that supplies the value.
 */
#pragma once

#include "mj2x/mir/register_ir/reg_instruction.hpp"
#include "mj2x/mir/ssa/ssa_ids.hpp"

#include <string>
#include <variant>
#include <vector>

namespace mj2x {

struct RegBlock;

/// @brief A control-flow edge plus the copies that materialize eliminated block args.
struct RegSuccessor {
    RegBlock* block = nullptr;
    std::vector<RegCopy> copies;
};

struct RegJumpTerminator {
    RegSuccessor target;
};

struct RegBranchTerminator {
    RegOperand condition;
    RegSuccessor trueTarget;
    RegSuccessor falseTarget;
};

struct RegSwitchCase {
    int64_t key = 0;
    RegSuccessor target;
};

struct RegSwitchTerminator {
    RegOperand value;
    std::vector<RegSwitchCase> cases;
    RegSuccessor defaultTarget;
};

struct RegReturnTerminator {
    bool hasValue = false;
    RegOperand value;
};

struct RegThrowTerminator {
    RegOperand value;
};

struct RegUnreachableTerminator {};

using RegTerminator = std::variant<
    RegJumpTerminator,
    RegBranchTerminator,
    RegSwitchTerminator,
    RegReturnTerminator,
    RegThrowTerminator,
    RegUnreachableTerminator>;

/// @brief A basic block with flat register instructions and a single terminator.
struct RegBlock {
    SSABlockId id;
    int l1LabelId = -1;
    std::vector<RegInstruction> instructions;
    RegTerminator terminator = RegUnreachableTerminator{};

    explicit RegBlock(SSABlockId id_) : id(id_) {}
};

/**
 * @brief One lowered JVM exception-table entry.
 *
 * @details
 * RegIR has no block arguments, so `exceptionValue` is the virtual register
 * corresponding to SSA's synthetic handler exception arg.  RegC assigns that
 * register immediately before jumping to `handler` after a successful
 * `_mj2x_regc_exception_matches` check.
 */
struct RegExceptionRegion {
    std::vector<RegBlock*> protectedBlocks; ///< Blocks covered by the handler.
    RegBlock* handler = nullptr;            ///< Handler entry block.
    RegValue exceptionValue{};              ///< Register holding caught object.
    std::string caughtType;                 ///< Empty means catch-all/finally.
};

} // namespace mj2x

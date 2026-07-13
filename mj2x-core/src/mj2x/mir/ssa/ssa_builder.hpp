/**
 * @file ssa_builder.hpp
 * @brief L1 MIR blocks to block-argument SSA conversion.
 *
 * @details
 * SSABuilder is the boundary where JVM stack bytecode becomes a value-oriented
 * IR for RegC and later optimization passes.  The builder intentionally keeps
 * operand-stack simulation private to construction time: stack shuffles such as
 * POP, DUP, DUP2_X*, and SWAP update the temporary FrameState with JVM
 * category-1/category-2 rules, while emitted SSA instructions only reference
 * stable SSAValue objects.  This is the mechanism that removes the old StackC
 * cstack/sp anchor from downstream IR.
 *
 * The construction algorithm is documented in `mj2x/doc_ai/ssa_algorithm.md`.
 * In short, it:
 *   1. computes a reaching-def view over local slots,
 *   2. creates SSABlock arguments for values that must cross block boundaries,
 *   3. lowers each MIR node into stackless SSA instructions or terminators.
 *
 * Unsupported or semantically ambiguous bytecode is rejected with a reason
 * string rather than approximated silently.
 */
#pragma once

#include "mj2x/frontend/block.hpp"
#include "mj2x/mir/reaching_def.hpp"
#include "mj2x/mir/ssa/ssa_function.hpp"

#include <memory>
#include <string>

namespace mj2x {

/**
 * @brief Builds an SSAFunction from CodeConverter L1 blocks.
 *
 * @details
 * The generated function uses block arguments instead of explicit phi nodes.
 * Method parameters are bound by JVM local slot, so wide `long`/`double`
 * parameters occupy the expected slot spacing even though each SSA value is a
 * single object.  The builder owns no input blocks; all output state is owned by
 * the returned SSAFunction.
 */
class SSABuilder
{
public:
    /**
     * @brief Convert one method's L1 blocks into SSA.
     *
     * @param blocks Blocks produced by CodeConverter; they must outlive this call.
     * @param ownerName JVM internal class name that owns the method.
     * @param methodName JVM method name.
     * @param methodDesc JVM method descriptor.
     * @param isStatic Whether the method has no implicit `this` parameter.
     * @param methodMaxLocals Local slot count used to size the construction frame.
     * @param reason Receives a human-readable fail-closed reason on failure.
     * @return A fully-owned SSAFunction, or nullptr if the method is unsupported.
     */
    std::unique_ptr<SSAFunction> build(const std::vector<Block*>& blocks,
                                       const std::string& ownerName, const std::string& methodName,
                                       const std::string& methodDesc, bool isStatic,
                                       int methodMaxLocals, std::string& reason);

private:
    /// @brief Method-level exception table carried by CodeConverter on the first block.
    std::vector<ExceptionHandler> exceptionTable(const std::vector<Block*>& blocks) const;

    /// @brief Stage A: build the reaching-def view consumed by computeReachingDef().
    std::vector<ReachingDefBlock> buildReachingDefView(const std::vector<Block*>& blocks);

    /// @brief Collect explicit branch/switch targets from one L1 block.
    void collectOutEdges(Block* b, std::vector<int>& targets);

    /// @brief Stage B: create SSABlocks and their block-argument local slots.
    void buildBlocks(SSAFunction& fn, const std::vector<Block*>& l1blocks,
                     const ReachingDefResult& rd);

    /// @brief Stage C: lower MIR nodes while simulating the JVM operand stack.
    bool translateBlocks(SSAFunction& fn, const std::vector<Block*>& l1blocks,
                         const ReachingDefResult& rd, int methodMaxLocals, std::string& reason);
};

}   // namespace mj2x

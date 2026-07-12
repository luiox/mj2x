#include "ssa_block.hpp"

namespace mj2x {

// 从 terminator 推后继块。用 std::visit 遍历变体。
std::vector<SSABlock*> SSABlock::successors() const
{
    std::vector<SSABlock*> result;
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, SSAJumpTerminator>) {
                if (t.target.block) result.push_back(t.target.block);
            }
            else if constexpr (std::is_same_v<T, SSABranchTerminator>) {
                if (t.trueTarget.block) result.push_back(t.trueTarget.block);
                if (t.falseTarget.block) result.push_back(t.falseTarget.block);
            }
            else if constexpr (std::is_same_v<T, SSASwitchTerminator>) {
                for (const auto& c : t.cases)
                    if (c.target.block) result.push_back(c.target.block);
                if (t.defaultTarget.block) result.push_back(t.defaultTarget.block);
            }
            // Return / Throw / Unreachable 无后继。
        },
        terminator);
    return result;
}

}   // namespace mj2x

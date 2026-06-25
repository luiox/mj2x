// mj2x SSA builder — L1 Block[] → SSAFunction (block-argument SSA)。
//
// 算法见 doc/SSA_ALGORITHM.md。三阶段：
//   A. 从 L1 Block[] 构造 ReachingDefBlock 视图 + 跑 reaching-def
//   B. 搭块结构 + 入口 frame shape（决定 block args）
//   C. 逐块翻译（FrameState 模拟栈，吸收 DUP/SWAP，不产栈 IR）
//
// 关键：栈操作（DUP/SWAP/...）只在 builder-local FrameState 里动，**不产任何 SSA 节点**。
// 这是"消除 cstack 锚点"的机制。详见 IR_DESIGN.md §10、SSA_ALGORITHM.md §5.2。
#pragma once

#include "mj2x/frontend/block.hpp"
#include "mj2x/mir/reaching_def.hpp"
#include "mj2x/mir/ssa/ssa_function.hpp"

#include <memory>
#include <string>

namespace mj2x {

class SSABuilder {
public:
    // 从 L1 的 Block[] 构造一个方法的 SSAFunction。ownerName/desc 供 symbol。
    // 返回 nullptr = fail-closed（带 try-catch / 不支持的指令），reason 填拒绝原因。
    //
    // blocks 由调用方（CodeConverter::convert）产出，builder 不持有它们的生命周期。
    // methodMaxLocals 用于 FrameState 的 locals 数组大小。
    std::unique_ptr<SSAFunction> build(const std::vector<Block*>& blocks,
                                       const std::string& ownerName,
                                       const std::string& methodName,
                                       const std::string& methodDesc,
                                       bool isStatic,
                                       int methodMaxLocals,
                                       std::string& reason);

private:
    // 阶段 A：从 L1 blocks 构造 ReachingDefBlock 视图 + 跑 reaching-def。
    std::vector<ReachingDefBlock> buildReachingDefView(const std::vector<Block*>& blocks);
    void collectOutEdges(Block* b, std::vector<int>& targets);

    // 阶段 B：建 SSABlock[]，决定 args。
    void buildBlocks(SSAFunction& fn,
                     const std::vector<Block*>& l1blocks,
                     const ReachingDefResult& rd);

    // 阶段 C：逐块翻译。
    bool translateBlocks(SSAFunction& fn,
                         const std::vector<Block*>& l1blocks,
                         const ReachingDefResult& rd,
                         std::string& reason);
};

} // namespace mj2x

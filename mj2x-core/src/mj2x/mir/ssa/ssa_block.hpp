// mj2x SSA IR — 基本块。
// 详见 doc/IR_DESIGN.md §3。instructions 不含控制转移；所有出边归 terminator。
#pragma once

#include "ssa_instruction.hpp"
#include "ssa_terminator.hpp"
#include "ssa_value.hpp"

#include <memory>
#include <vector>

namespace mj2x {

struct SSABlock {
    SSABlockId id;
    std::vector<SSABlockArg> args;                 // 入口参数 = 隐式 φ
    std::vector<std::unique_ptr<SSAInstruction>> instructions;
    SSATerminator terminator = SSAUnreachableTerminator{};

    // 前驱/后继关系（CFG）。后继从 terminator 推；前驱构造期填。
    std::vector<SSABlock*> predecessors;
    // 关联的 L1 Block label id（builder 用来对齐 CodeConverter 的 CFG）。
    int l1LabelId = -1;

    explicit SSABlock(SSABlockId id_) : id(id_) {}

    // 便利：取出后继块（不含传值），供遍历用。
    std::vector<SSABlock*> successors() const;
};

} // namespace mj2x

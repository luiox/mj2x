// mj2x SSA IR — 块出口终结符（6 类）+ SSASuccessor + SSABlockArg 容器。
// 详见 doc/IR_DESIGN.md §8。块内不允许控制转移，所有出边归 terminator 拥有。
#pragma once

#include "ssa_ids.hpp"
#include "ssa_value.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mj2x {

struct SSABlock;

// 一条出边：跳到 block，并给后继块的 args 传值（phi 语义，详见 IR_DESIGN §6）。
struct SSASuccessor
{
    SSABlock*              block = nullptr;
    std::vector<SSAValue*> args;   // 个数必须等于后继 block->args.size()
};

// ---- Terminator 变体 ----
struct SSAJumpTerminator
{
    SSASuccessor target;
};
struct SSABranchTerminator
{
    SSAValue*    condition = nullptr;   // SSACompareInstruction 的 bool result
    SSASuccessor trueTarget;
    SSASuccessor falseTarget;
};
struct SSASwitchCase
{
    int64_t      key = 0;
    SSASuccessor target;
};
struct SSASwitchTerminator
{
    SSAValue*                  value = nullptr;
    std::vector<SSASwitchCase> cases;
    SSASuccessor               defaultTarget;
};
struct SSAReturnTerminator
{
    SSAValue* value = nullptr;   // null = void return
};
struct SSAThrowTerminator
{
    SSAValue* value = nullptr;
};
struct SSAUnreachableTerminator
{};

// tagged union 终结符。RTTI 禁用，故用 variant 而非虚函数。
using SSATerminator =
    std::variant<SSAJumpTerminator, SSABranchTerminator, SSASwitchTerminator, SSAReturnTerminator,
                 SSAThrowTerminator, SSAUnreachableTerminator>;

}   // namespace mj2x

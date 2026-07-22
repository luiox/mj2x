#include "mj2x/mir/register_ir/reg_dce.hpp"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace mj2x {
namespace {

using RegisterSet = std::unordered_set<uint32_t>;

void markUse(const RegOperand& operand, RegisterSet& used)
{
    if (operand.kind == RegOperandKind::Register) used.insert(operand.reg.value);
}

void markCopyUses(const std::vector<RegCopy>& copies, RegisterSet& used)
{
    for (const auto& copy : copies) markUse(copy.source, used);
}

void markSuccessorUses(const RegSuccessor& successor, RegisterSet& used)
{
    markCopyUses(successor.copies, used);
}

void markInstructionUses(const RegInstruction& instruction, RegisterSet& used)
{
    markUse(instruction.copy.source, used);
    markUse(instruction.value, used);
    markUse(instruction.lhs, used);
    markUse(instruction.rhs, used);
    markUse(instruction.receiver, used);
    markUse(instruction.index, used);
    for (const auto& argument : instruction.args) markUse(argument, used);
}

void markTerminatorUses(const RegTerminator& terminator, RegisterSet& used)
{
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                markSuccessorUses(value.target, used);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                markUse(value.condition, used);
                markSuccessorUses(value.trueTarget, used);
                markSuccessorUses(value.falseTarget, used);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                markUse(value.value, used);
                for (const auto& entry : value.cases) markSuccessorUses(entry.target, used);
                markSuccessorUses(value.defaultTarget, used);
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (value.hasValue) markUse(value.value, used);
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                markUse(value.value, used);
            }
        },
        terminator);
}

RegisterSet collectUsedRegisters(const RegFunction& function)
{
    RegisterSet used;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions) markInstructionUses(instruction, used);
        markTerminatorUses(block->terminator, used);
    }
    for (const auto& region : function.exceptionRegions)
        for (const auto& edge : region.edges) markCopyUses(edge.copies, used);
    return used;
}

bool hasObservableEffect(const SSAEffect& effect)
{
    return effect.mayThrow || effect.readsMemory || effect.writesMemory ||
           effect.readsExternalState || effect.writesExternalState || effect.callsExternal ||
           effect.resolvesExternal || effect.isBarrier;
}

bool canRemove(const RegInstruction& instruction)
{
    if (!instruction.hasDest || hasObservableEffect(instruction.effect)) return false;
    switch (instruction.kind) {
    case RegInstructionKind::Copy:
    case RegInstructionKind::Unary:
    case RegInstructionKind::Compare: return true;
    case RegInstructionKind::Binary:
        return instruction.binaryOp != SSABinaryOp::Div && instruction.binaryOp != SSABinaryOp::Rem;
    case RegInstructionKind::Convert: return instruction.convertKind == SSAConvertKind::Numeric;
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier: return false;
    }
    return false;
}

}   // namespace

ca::usize eliminateDeadRegInstructions(RegFunction& function)
{
    ca::usize removed = 0;
    while (true) {
        const RegisterSet used = collectUsedRegisters(function);
        ca::usize         roundRemoved = 0;
        for (auto& block : function.blocks) {
            auto& instructions = block->instructions;
            const auto end = std::remove_if(instructions.begin(), instructions.end(), [&](const auto& instruction) {
                const bool isDead = canRemove(instruction) &&
                                    used.find(instruction.dest.id.value) == used.end();
                if (isDead) ++roundRemoved;
                return isDead;
            });
            instructions.erase(end, instructions.end());
        }
        removed += roundRemoved;
        if (roundRemoved == 0) return removed;
    }
}

}   // namespace mj2x

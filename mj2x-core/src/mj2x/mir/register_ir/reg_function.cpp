#include "reg_function.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

namespace mj2x {

namespace {

bool operandHasStackSlot(const RegOperand& operand)
{
    switch (operand.kind) {
    case RegOperandKind::Register:
    case RegOperandKind::IntLiteral:
    case RegOperandKind::FloatLiteral:
    case RegOperandKind::NullLiteral:
    case RegOperandKind::Opaque: return false;
    }
    return true;
}

bool copyHasStackSlot(const RegCopy& copy)
{
    return operandHasStackSlot(copy.source);
}

bool instructionHasStackSlot(const RegInstruction& ins)
{
    if (ins.kind == RegInstructionKind::Copy && copyHasStackSlot(ins.copy)) return true;
    if (operandHasStackSlot(ins.value) || operandHasStackSlot(ins.lhs) ||
        operandHasStackSlot(ins.rhs) || operandHasStackSlot(ins.receiver) ||
        operandHasStackSlot(ins.index)) {
        return true;
    }
    for (const auto& arg : ins.args) {
        if (operandHasStackSlot(arg)) return true;
    }
    return false;
}

bool successorHasStackSlot(const RegSuccessor& successor)
{
    for (const auto& copy : successor.copies) {
        if (copyHasStackSlot(copy)) return true;
    }
    return false;
}

bool terminatorHasStackSlot(const RegTerminator& terminator)
{
    return std::visit(
        [](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                return successorHasStackSlot(t.target);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                return operandHasStackSlot(t.condition) || successorHasStackSlot(t.trueTarget) ||
                       successorHasStackSlot(t.falseTarget);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (operandHasStackSlot(t.value) || successorHasStackSlot(t.defaultTarget))
                    return true;
                for (const auto& c : t.cases) {
                    if (successorHasStackSlot(c.target)) return true;
                }
                return false;
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                return t.hasValue && operandHasStackSlot(t.value);
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                return operandHasStackSlot(t.value);
            }
            else {
                return false;
            }
        },
        terminator);
}

}   // namespace

bool validateRegIRNoStackSlots(const RegFunction& fn, std::string& reason)
{
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (instructionHasStackSlot(ins)) {
                std::ostringstream os;
                os << "RegIR instruction in block b" << block->id.value
                   << " references a stack-slot operand";
                reason = os.str();
                return false;
            }
        }
        if (terminatorHasStackSlot(block->terminator)) {
            std::ostringstream os;
            os << "RegIR terminator in block b" << block->id.value
               << " references a stack-slot operand";
            reason = os.str();
            return false;
        }
    }
    reason.clear();
    return true;
}

}   // namespace mj2x

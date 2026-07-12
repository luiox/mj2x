#include "reg_lowering.hpp"

#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace mj2x {

namespace {

using ValueMap = std::unordered_map<const SSAValue*, RegOperand>;
using BlockMap = std::unordered_map<const SSABlock*, RegBlock*>;

RegValue regValueFor(const SSAValue& value)
{
    return RegValue{RegId{value.id.value}, value.type};
}

bool sameRegister(const RegOperand& source, RegValue dest)
{
    return source.kind == RegOperandKind::Register && source.reg == dest.id &&
           source.type == dest.type;
}

bool mapValue(const SSAValue* value, ValueMap& values, std::string& reason, RegOperand& out)
{
    if (!value) {
        reason = "null SSA value operand";
        return false;
    }

    switch (value->valueKind) {
    case SSAValueKind::Parameter:
    case SSAValueKind::BlockArg:
    case SSAValueKind::InstructionResult:
    case SSAValueKind::Opaque:
    {
        auto it = values.find(value);
        if (it != values.end()) {
            out = it->second;
            return true;
        }

        if (value->valueKind == SSAValueKind::Opaque) {
            auto* opaque = static_cast<const SSAOpaqueValue*>(value);
            out          = RegOperand::opaque(value->type, opaque->tag);
            return true;
        }

        std::ostringstream os;
        os << "SSA value v" << value->id.value << " is used before RegIR definition";
        reason = os.str();
        return false;
    }
    case SSAValueKind::IntLiteral:
    {
        auto* literal = static_cast<const SSAIntLiteral*>(value);
        out           = RegOperand::fromInt(literal->value, literal->type);
        return true;
    }
    case SSAValueKind::FloatLiteral:
    {
        auto* literal = static_cast<const SSAFloatLiteral*>(value);
        out           = RegOperand::fromFloat(literal->value, literal->type);
        return true;
    }
    case SSAValueKind::NullLiteral: out = RegOperand::nullRef(); return true;
    }

    reason = "unknown SSA value kind";
    return false;
}

bool mapValueVector(const std::vector<SSAValue*>& values, ValueMap& valueMap, std::string& reason,
                    std::vector<RegOperand>& out)
{
    out.clear();
    out.reserve(values.size());
    for (auto* value : values) {
        RegOperand operand;
        if (!mapValue(value, valueMap, reason, operand)) return false;
        out.push_back(std::move(operand));
    }
    return true;
}

bool lowerSuccessor(const SSASuccessor& source, const BlockMap& blocks, ValueMap& values,
                    std::string& reason, RegSuccessor& out)
{
    auto blockIt = blocks.find(source.block);
    if (blockIt == blocks.end()) {
        reason = "SSA successor references unknown block";
        return false;
    }

    out.block = blockIt->second;
    out.copies.clear();

    if (source.block && source.args.size() != source.block->args.size()) {
        reason = "SSA successor argument count does not match target block arguments";
        return false;
    }

    // Block arguments are phi-like values.  Keep the copy on this exact edge so
    // branch/switch successors can pass different values without clobbering
    // each other in the predecessor block.
    for (size_t i = 0; i < source.args.size(); ++i) {
        RegOperand operand;
        if (!mapValue(source.args[i], values, reason, operand)) return false;

        RegValue dest = regValueFor(source.block->args[i]);
        if (!sameRegister(operand, dest)) {
            out.copies.push_back(RegCopy{dest, operand});
        }
    }

    return true;
}

bool assignDest(const SSAInstruction& source, ValueMap& values, RegInstruction& out)
{
    if (!source.hasResult) {
        out.hasDest = false;
        return true;
    }

    RegValue dest          = regValueFor(source.result);
    out.hasDest            = true;
    out.dest               = dest;
    values[&source.result] = RegOperand::fromRegister(dest);
    return true;
}

bool lowerInstruction(const SSAInstruction& source, ValueMap& values, std::string& reason,
                      RegInstruction& out)
{
    out.effect = source.effect;
    assignDest(source, values, out);

    switch (source.kind) {
    case SSAInstructionKind::Unary:
    {
        auto* ins   = static_cast<const SSAUnaryInstruction*>(&source);
        out.kind    = RegInstructionKind::Unary;
        out.unaryOp = ins->op;
        return mapValue(ins->value, values, reason, out.value);
    }
    case SSAInstructionKind::Binary:
    {
        auto* ins    = static_cast<const SSABinaryInstruction*>(&source);
        out.kind     = RegInstructionKind::Binary;
        out.binaryOp = ins->op;
        return mapValue(ins->lhs, values, reason, out.lhs) &&
               mapValue(ins->rhs, values, reason, out.rhs);
    }
    case SSAInstructionKind::Compare:
    {
        auto* ins            = static_cast<const SSACompareInstruction*>(&source);
        out.kind             = RegInstructionKind::Compare;
        out.comparePredicate = ins->predicate;
        return mapValue(ins->lhs, values, reason, out.lhs) &&
               mapValue(ins->rhs, values, reason, out.rhs);
    }
    case SSAInstructionKind::Convert:
    {
        auto* ins             = static_cast<const SSAConvertInstruction*>(&source);
        out.kind              = RegInstructionKind::Convert;
        out.convertKind       = ins->convertKind;
        out.targetType        = ins->targetType;
        out.referenceTypeDesc = ins->referenceTypeDesc;
        return mapValue(ins->value, values, reason, out.value);
    }
    case SSAInstructionKind::LoadField:
    {
        auto* ins = static_cast<const SSALoadFieldInstruction*>(&source);
        out.kind  = RegInstructionKind::LoadField;
        out.field = ins->field;
        if (!ins->field.isStatic) {
            return mapValue(ins->receiver, values, reason, out.receiver);
        }
        return true;
    }
    case SSAInstructionKind::StoreField:
    {
        auto* ins = static_cast<const SSAStoreFieldInstruction*>(&source);
        out.kind  = RegInstructionKind::StoreField;
        out.field = ins->field;
        if (!mapValue(ins->value, values, reason, out.value)) return false;
        if (!ins->field.isStatic) {
            return mapValue(ins->receiver, values, reason, out.receiver);
        }
        return true;
    }
    case SSAInstructionKind::ArrayLoad:
    {
        auto* ins       = static_cast<const SSAArrayLoadInstruction*>(&source);
        out.kind        = RegInstructionKind::ArrayLoad;
        out.elementType = ins->elementType;
        return mapValue(ins->array, values, reason, out.receiver) &&
               mapValue(ins->index, values, reason, out.index);
    }
    case SSAInstructionKind::ArrayStore:
    {
        auto* ins       = static_cast<const SSAArrayStoreInstruction*>(&source);
        out.kind        = RegInstructionKind::ArrayStore;
        out.elementType = ins->elementType;
        return mapValue(ins->array, values, reason, out.receiver) &&
               mapValue(ins->index, values, reason, out.index) &&
               mapValue(ins->value, values, reason, out.value);
    }
    case SSAInstructionKind::Call:
    {
        auto* ins  = static_cast<const SSACallInstruction*>(&source);
        out.kind   = RegInstructionKind::Call;
        out.method = ins->method;
        return mapValueVector(ins->args, values, reason, out.args);
    }
    case SSAInstructionKind::Allocate:
    {
        auto* ins          = static_cast<const SSAAllocateInstruction*>(&source);
        out.kind           = RegInstructionKind::Allocate;
        out.allocationKind = ins->allocationKind;
        out.typeDesc       = ins->typeDesc;
        return mapValueVector(ins->args, values, reason, out.args);
    }
    case SSAInstructionKind::Intrinsic:
    {
        auto* ins         = static_cast<const SSAIntrinsicInstruction*>(&source);
        out.kind          = RegInstructionKind::Intrinsic;
        out.intrinsicName = ins->name;
        return mapValueVector(ins->args, values, reason, out.args);
    }
    case SSAInstructionKind::Barrier:
    {
        auto* ins         = static_cast<const SSABarrierInstruction*>(&source);
        out.kind          = RegInstructionKind::Barrier;
        out.barrierReason = ins->reason;
        return true;
    }
    }

    reason = "unknown SSA instruction kind";
    return false;
}

bool lowerTerminator(const SSATerminator& source, const BlockMap& blocks, ValueMap& values,
                     std::string& reason, RegTerminator& out)
{
    return std::visit(
        [&](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, SSAJumpTerminator>) {
                RegJumpTerminator lowered;
                if (!lowerSuccessor(t.target, blocks, values, reason, lowered.target)) return false;
                out = std::move(lowered);
                return true;
            }
            else if constexpr (std::is_same_v<T, SSABranchTerminator>) {
                RegBranchTerminator lowered;
                if (!mapValue(t.condition, values, reason, lowered.condition)) return false;
                if (!lowerSuccessor(t.trueTarget, blocks, values, reason, lowered.trueTarget))
                    return false;
                if (!lowerSuccessor(t.falseTarget, blocks, values, reason, lowered.falseTarget))
                    return false;
                out = std::move(lowered);
                return true;
            }
            else if constexpr (std::is_same_v<T, SSASwitchTerminator>) {
                RegSwitchTerminator lowered;
                if (!mapValue(t.value, values, reason, lowered.value)) return false;
                for (const auto& c : t.cases) {
                    RegSwitchCase loweredCase;
                    loweredCase.key = c.key;
                    if (!lowerSuccessor(c.target, blocks, values, reason, loweredCase.target))
                        return false;
                    lowered.cases.push_back(std::move(loweredCase));
                }
                if (!lowerSuccessor(t.defaultTarget, blocks, values, reason, lowered.defaultTarget))
                    return false;
                out = std::move(lowered);
                return true;
            }
            else if constexpr (std::is_same_v<T, SSAReturnTerminator>) {
                RegReturnTerminator lowered;
                lowered.hasValue = t.value != nullptr;
                if (t.value && !mapValue(t.value, values, reason, lowered.value)) return false;
                out = lowered;
                return true;
            }
            else if constexpr (std::is_same_v<T, SSAThrowTerminator>) {
                RegThrowTerminator lowered;
                if (!mapValue(t.value, values, reason, lowered.value)) return false;
                out = lowered;
                return true;
            }
            else if constexpr (std::is_same_v<T, SSAUnreachableTerminator>) {
                out = RegUnreachableTerminator{};
                return true;
            }
            else {
                reason = "unknown SSA terminator kind";
                return false;
            }
        },
        source);
}

}   // namespace

std::unique_ptr<RegFunction> RegLowering::lower(const SSAFunction& fn, std::string& reason)
{
    auto reg    = std::make_unique<RegFunction>();
    reg->symbol = fn.symbol;

    ValueMap values;
    BlockMap blocks;

    for (const auto& param : fn.parameters) {
        RegValue value = regValueFor(*param);
        reg->parameters.push_back(value);
        values[param.get()] = RegOperand::fromRegister(value);
    }

    // First pass creates every RegBlock and every block-arg register mapping so
    // terminators can reference forward blocks and loop headers.
    for (const auto& sourceBlock : fn.blocks) {
        auto block       = std::make_unique<RegBlock>(sourceBlock->id);
        block->l1LabelId = sourceBlock->l1LabelId;
        RegBlock* raw    = block.get();
        if (sourceBlock.get() == fn.entry) reg->entry = raw;
        blocks[sourceBlock.get()] = raw;
        reg->blocks.push_back(std::move(block));

        for (const auto& arg : sourceBlock->args) {
            RegValue value = regValueFor(arg);
            values[&arg]   = RegOperand::fromRegister(value);
        }
    }

    // Second pass lowers block bodies and terminators after all values that may
    // appear as edge-copy destinations have stable RegIds.
    for (const auto& sourceBlock : fn.blocks) {
        RegBlock* targetBlock = blocks[sourceBlock.get()];
        for (const auto& sourceInstruction : sourceBlock->instructions) {
            RegInstruction lowered;
            if (!lowerInstruction(*sourceInstruction, values, reason, lowered)) {
                return nullptr;
            }
            targetBlock->instructions.push_back(std::move(lowered));
        }

        if (!lowerTerminator(
                sourceBlock->terminator, blocks, values, reason, targetBlock->terminator)) {
            return nullptr;
        }
    }

    for (const auto& sourceRegion : fn.exceptionRegions) {
        if (!sourceRegion.handler || !sourceRegion.exceptionValue) {
            reason = "SSA exception region is missing handler metadata";
            return nullptr;
        }
        RegExceptionRegion loweredRegion;
        loweredRegion.caughtType = sourceRegion.caughtType;
        auto handlerIt           = blocks.find(sourceRegion.handler);
        if (handlerIt == blocks.end()) {
            reason = "SSA exception region references unknown handler block";
            return nullptr;
        }
        loweredRegion.handler        = handlerIt->second;
        loweredRegion.exceptionValue = regValueFor(*sourceRegion.exceptionValue);
        for (auto* protectedBlock : sourceRegion.protectedBlocks) {
            auto blockIt = blocks.find(protectedBlock);
            if (blockIt == blocks.end()) {
                reason = "SSA exception region references unknown protected block";
                return nullptr;
            }
            loweredRegion.protectedBlocks.push_back(blockIt->second);
        }
        reg->exceptionRegions.push_back(std::move(loweredRegion));
    }

    if (!validateRegIRNoStackSlots(*reg, reason)) return nullptr;
    return reg;
}

}   // namespace mj2x

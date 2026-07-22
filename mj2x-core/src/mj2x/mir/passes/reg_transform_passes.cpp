#include "mj2x/mir/passes/reg_transform_passes.hpp"

#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mj2x {
namespace {

bool hasObservableEffect(const SSAEffect& effect)
{
    return effect.mayThrow || effect.readsMemory || effect.writesMemory ||
           effect.readsExternalState || effect.writesExternalState || effect.callsExternal ||
           effect.resolvesExternal || effect.isBarrier;
}

RegInstruction makeBinary(RegValue dest, RegOperand lhs, RegOperand rhs, SSABinaryOp op)
{
    RegInstruction instruction;
    instruction.kind     = RegInstructionKind::Binary;
    instruction.hasDest  = true;
    instruction.dest     = dest;
    instruction.lhs      = std::move(lhs);
    instruction.rhs      = std::move(rhs);
    instruction.binaryOp = op;
    instruction.effect   = SSAEffect::pure();
    return instruction;
}

RegInstruction makeCompare(RegValue dest, RegOperand lhs, RegOperand rhs,
                           SSAComparePredicate predicate)
{
    RegInstruction instruction;
    instruction.kind             = RegInstructionKind::Compare;
    instruction.hasDest          = true;
    instruction.dest             = dest;
    instruction.lhs              = std::move(lhs);
    instruction.rhs              = std::move(rhs);
    instruction.comparePredicate = predicate;
    instruction.effect           = SSAEffect::pure();
    return instruction;
}

RegInstruction makeCopy(RegValue dest, RegOperand source)
{
    RegInstruction instruction;
    instruction.kind    = RegInstructionKind::Copy;
    instruction.hasDest = true;
    instruction.dest    = dest;
    instruction.copy    = RegCopy{dest, std::move(source)};
    instruction.effect  = SSAEffect::pure();
    return instruction;
}

void observeOperandRegister(const RegOperand& operand, ca::u32& maximum, bool& found)
{
    if (operand.kind != RegOperandKind::Register) return;
    if (!found || operand.reg.value > maximum) maximum = operand.reg.value;
    found = true;
}

void observeValueRegister(RegValue value, ca::u32& maximum, bool& found)
{
    if (!found || value.id.value > maximum) maximum = value.id.value;
    found = true;
}

void observeCopies(const std::vector<RegCopy>& copies, ca::u32& maximum, bool& found)
{
    for (const auto& copy : copies) {
        observeValueRegister(copy.dest, maximum, found);
        observeOperandRegister(copy.source, maximum, found);
    }
}

void observeSuccessor(const RegSuccessor& successor, ca::u32& maximum, bool& found)
{
    observeCopies(successor.copies, maximum, found);
}

void observeInstructionRegisters(const RegInstruction& instruction, ca::u32& maximum, bool& found)
{
    if (instruction.hasDest) observeValueRegister(instruction.dest, maximum, found);
    switch (instruction.kind) {
    case RegInstructionKind::Copy:
        observeValueRegister(instruction.copy.dest, maximum, found);
        observeOperandRegister(instruction.copy.source, maximum, found);
        break;
    case RegInstructionKind::Unary:
    case RegInstructionKind::Convert:
        observeOperandRegister(instruction.value, maximum, found);
        break;
    case RegInstructionKind::Binary:
    case RegInstructionKind::Compare:
        observeOperandRegister(instruction.lhs, maximum, found);
        observeOperandRegister(instruction.rhs, maximum, found);
        break;
    case RegInstructionKind::LoadField:
        observeOperandRegister(instruction.receiver, maximum, found);
        break;
    case RegInstructionKind::StoreField:
        observeOperandRegister(instruction.receiver, maximum, found);
        observeOperandRegister(instruction.value, maximum, found);
        break;
    case RegInstructionKind::ArrayLoad:
        observeOperandRegister(instruction.receiver, maximum, found);
        observeOperandRegister(instruction.index, maximum, found);
        break;
    case RegInstructionKind::ArrayStore:
        observeOperandRegister(instruction.receiver, maximum, found);
        observeOperandRegister(instruction.index, maximum, found);
        observeOperandRegister(instruction.value, maximum, found);
        break;
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
        for (const auto& argument : instruction.args)
            observeOperandRegister(argument, maximum, found);
        break;
    case RegInstructionKind::Barrier: break;
    }
}

void observeTerminatorRegisters(const RegTerminator& terminator, ca::u32& maximum, bool& found)
{
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                observeSuccessor(value.target, maximum, found);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                observeOperandRegister(value.condition, maximum, found);
                observeSuccessor(value.trueTarget, maximum, found);
                observeSuccessor(value.falseTarget, maximum, found);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                observeOperandRegister(value.value, maximum, found);
                for (const auto& item : value.cases) observeSuccessor(item.target, maximum, found);
                observeSuccessor(value.defaultTarget, maximum, found);
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (value.hasValue) observeOperandRegister(value.value, maximum, found);
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                observeOperandRegister(value.value, maximum, found);
            }
        },
        terminator);
}

ca::u32 nextRegisterId(const RegFunction& function)
{
    ca::u32 maximum = 0;
    bool    found   = false;
    for (const auto& parameter : function.parameters)
        observeValueRegister(parameter, maximum, found);
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions)
            observeInstructionRegisters(instruction, maximum, found);
        observeTerminatorRegisters(block->terminator, maximum, found);
    }
    for (const auto& region : function.exceptionRegions)
        for (const auto& edge : region.edges) observeCopies(edge.copies, maximum, found);
    return found ? maximum + 1 : 0;
}

ca::u32 nextBlockId(const RegFunction& function)
{
    ca::u32 maximum = 0;
    bool    found   = false;
    for (const auto& block : function.blocks) {
        if (!found || block->id.value > maximum) maximum = block->id.value;
        found = true;
    }
    return found ? maximum + 1 : 0;
}

bool canRewriteMba(const RegInstruction& instruction)
{
    return instruction.kind == RegInstructionKind::Binary && instruction.hasDest &&
           instruction.binaryOp == SSABinaryOp::Add &&
           (instruction.dest.type == SSAType::I32 || instruction.dest.type == SSAType::I64) &&
           instruction.lhs.type == instruction.dest.type &&
           instruction.rhs.type == instruction.dest.type &&
           !hasObservableEffect(instruction.effect);
}

void applyMba(RegFunction& function, ca::u32& nextRegister, RegTransformStats& stats)
{
    for (auto& block : function.blocks) {
        std::vector<RegInstruction> transformed;
        transformed.reserve(block->instructions.size() * 3);
        for (const auto& instruction : block->instructions) {
            if (!canRewriteMba(instruction)) {
                transformed.push_back(instruction);
                continue;
            }

            const RegValue orValue{RegId{nextRegister++}, instruction.dest.type};
            const RegValue andValue{RegId{nextRegister++}, instruction.dest.type};
            transformed.push_back(
                makeBinary(orValue, instruction.lhs, instruction.rhs, SSABinaryOp::Or));
            transformed.push_back(
                makeBinary(andValue, instruction.lhs, instruction.rhs, SSABinaryOp::And));
            RegInstruction rewritten = instruction;
            rewritten.lhs            = RegOperand::fromRegister(orValue);
            rewritten.rhs            = RegOperand::fromRegister(andValue);
            transformed.push_back(std::move(rewritten));
            ++stats.mbaRewrites;
        }
        block->instructions = std::move(transformed);
    }
}

RegOperand opaqueSource(const RegFunction& function, const RegBlock& block)
{
    for (const auto& parameter : function.parameters) {
        if (parameter.type == SSAType::I32 || parameter.type == SSAType::I64)
            return RegOperand::fromRegister(parameter);
    }
    return RegOperand::fromInt(static_cast<int64_t>(block.id.value), SSAType::I32);
}

bool normalizeBranchCondition(RegBlock& block, RegOperand& condition, ca::u32& nextRegister)
{
    if (condition.type == SSAType::BOOL) return true;
    if (condition.type != SSAType::I32 && condition.type != SSAType::I64) return false;
    const RegValue normalized{RegId{nextRegister++}, SSAType::BOOL};
    block.instructions.push_back(makeCompare(
        normalized, condition, RegOperand::fromInt(0, condition.type), SSAComparePredicate::Ne));
    condition = RegOperand::fromRegister(normalized);
    return true;
}

void applyOpaquePredicates(RegFunction& function, ca::u32& nextRegister, RegTransformStats& stats)
{
    for (auto& block : function.blocks) {
        auto* branch = std::get_if<RegBranchTerminator>(&block->terminator);
        if (branch == nullptr) continue;

        RegOperand condition = branch->condition;
        if (!normalizeBranchCondition(*block, condition, nextRegister)) continue;

        const RegOperand source = opaqueSource(function, *block);
        const SSAType    type   = source.type;
        const RegValue   orValue{RegId{nextRegister++}, type};
        const RegValue   andValue{RegId{nextRegister++}, type};
        const RegValue   predicate{RegId{nextRegister++}, SSAType::BOOL};
        const RegValue   combined{RegId{nextRegister++}, SSAType::BOOL};
        const RegOperand one = RegOperand::fromInt(1, type);
        block->instructions.push_back(makeBinary(orValue, source, one, SSABinaryOp::Or));
        block->instructions.push_back(
            makeBinary(andValue, RegOperand::fromRegister(orValue), one, SSABinaryOp::And));
        block->instructions.push_back(makeCompare(
            predicate, RegOperand::fromRegister(andValue), one, SSAComparePredicate::Eq));
        block->instructions.push_back(
            makeBinary(combined, condition, RegOperand::fromRegister(predicate), SSABinaryOp::And));
        branch->condition = RegOperand::fromRegister(combined);
        ++stats.opaquePredicates;
    }
}

bool isCffPureInstruction(const RegInstruction& instruction)
{
    if (hasObservableEffect(instruction.effect)) return false;
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

bool hasValidSuccessor(const RegSuccessor& successor)
{
    return successor.block != nullptr;
}

bool canFlattenControlFlow(const RegFunction& function)
{
    if (function.entry == nullptr || function.blocks.empty() || !function.exceptionRegions.empty())
        return false;

    bool hasControlFlow = false;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions) {
            if (!isCffPureInstruction(instruction)) return false;
        }
        const bool validTerminator = std::visit(
            [&](const auto& terminator) -> bool {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    hasControlFlow = true;
                    return hasValidSuccessor(terminator.target);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    hasControlFlow = true;
                    return hasValidSuccessor(terminator.trueTarget) &&
                           hasValidSuccessor(terminator.falseTarget);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    hasControlFlow = true;
                    if (!hasValidSuccessor(terminator.defaultTarget)) return false;
                    for (const auto& item : terminator.cases) {
                        if (!hasValidSuccessor(item.target)) return false;
                    }
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    return true;
                }
                else {
                    return false;
                }
            },
            block->terminator);
        if (!validTerminator) return false;
    }
    return hasControlFlow;
}

RegSuccessor makeDispatcherRoute(RegSuccessor                              successor,
                                 const std::map<const RegBlock*, ca::u32>& stateCodes,
                                 RegValue state, RegBlock* dispatcher, ca::u32& nextBlock,
                                 std::vector<std::unique_ptr<RegBlock>>& created,
                                 RegTransformStats&                      stats)
{
    const auto stateCode = stateCodes.find(successor.block);
    if (stateCode == stateCodes.end()) return successor;

    auto setter = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
    setter->instructions.push_back(makeCopy(
        state, RegOperand::fromInt(static_cast<int64_t>(stateCode->second), SSAType::I32)));
    setter->terminator = RegJumpTerminator{RegSuccessor{dispatcher, {}}};
    successor.block    = setter.get();
    created.push_back(std::move(setter));
    ++stats.cffEdgeSetters;
    return successor;
}

bool applyControlFlowFlattening(RegFunction& function, ca::u32& nextRegister,
                                RegTransformStats& stats)
{
    if (!canFlattenControlFlow(function)) return false;

    std::vector<RegBlock*> originals;
    originals.reserve(function.blocks.size());
    for (const auto& block : function.blocks) originals.push_back(block.get());

    std::map<const RegBlock*, ca::u32> stateCodes;
    ca::u32                            stateCode = 1;
    for (const auto* block : originals) stateCodes.emplace(block, stateCode++);

    const RegBlock* originalEntry = function.entry;
    const RegValue  state{RegId{nextRegister++}, SSAType::I32};
    ca::u32         nextBlock     = nextBlockId(function);
    auto            dispatcher    = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
    RegBlock*       dispatcherPtr = dispatcher.get();
    auto            bootstrap     = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
    RegBlock*       bootstrapPtr  = bootstrap.get();

    RegSwitchTerminator dispatch;
    dispatch.value = RegOperand::fromRegister(state);
    for (const auto* block : originals) {
        const auto code = stateCodes.find(block);
        dispatch.cases.push_back(RegSwitchCase{static_cast<int64_t>(code->second),
                                               RegSuccessor{const_cast<RegBlock*>(block), {}}});
    }
    dispatch.defaultTarget = RegSuccessor{const_cast<RegBlock*>(originalEntry), {}};
    dispatcher->terminator = std::move(dispatch);

    const auto entryCode = stateCodes.find(originalEntry);
    bootstrap->instructions.push_back(makeCopy(
        state, RegOperand::fromInt(static_cast<int64_t>(entryCode->second), SSAType::I32)));
    bootstrap->terminator = RegJumpTerminator{RegSuccessor{dispatcherPtr, {}}};

    std::vector<std::unique_ptr<RegBlock>> created;
    for (auto* block : originals) {
        std::visit(
            [&](auto& terminator) {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    terminator.target = makeDispatcherRoute(std::move(terminator.target),
                                                            stateCodes,
                                                            state,
                                                            dispatcherPtr,
                                                            nextBlock,
                                                            created,
                                                            stats);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    terminator.trueTarget  = makeDispatcherRoute(std::move(terminator.trueTarget),
                                                                stateCodes,
                                                                state,
                                                                dispatcherPtr,
                                                                nextBlock,
                                                                created,
                                                                stats);
                    terminator.falseTarget = makeDispatcherRoute(std::move(terminator.falseTarget),
                                                                 stateCodes,
                                                                 state,
                                                                 dispatcherPtr,
                                                                 nextBlock,
                                                                 created,
                                                                 stats);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (auto& item : terminator.cases) {
                        item.target = makeDispatcherRoute(std::move(item.target),
                                                          stateCodes,
                                                          state,
                                                          dispatcherPtr,
                                                          nextBlock,
                                                          created,
                                                          stats);
                    }
                    terminator.defaultTarget =
                        makeDispatcherRoute(std::move(terminator.defaultTarget),
                                            stateCodes,
                                            state,
                                            dispatcherPtr,
                                            nextBlock,
                                            created,
                                            stats);
                }
            },
            block->terminator);
    }

    function.entry = bootstrapPtr;
    function.blocks.push_back(std::move(bootstrap));
    function.blocks.push_back(std::move(dispatcher));
    for (auto& block : created) function.blocks.push_back(std::move(block));
    stats.controlFlowFlattened = true;
    return true;
}

}   // namespace

RegTransformStats applyRegTransformPasses(RegFunction& function, const RegTransformOptions& options)
{
    RegTransformStats stats;
    ca::u32           nextRegister = nextRegisterId(function);
    if (options.enableMba) applyMba(function, nextRegister, stats);
    if (options.enableOpaquePredicates) applyOpaquePredicates(function, nextRegister, stats);
    if (options.enableControlFlowFlattening)
        applyControlFlowFlattening(function, nextRegister, stats);
    return stats;
}

}   // namespace mj2x

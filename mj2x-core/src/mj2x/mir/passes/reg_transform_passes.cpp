#include "mj2x/mir/passes/reg_transform_passes.hpp"

#include "mj2x/mir/passes/const_key_derivation.hpp"

#include "libca/random/split_mix64.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mj2x {
namespace {

using jbytecode::core::Opcodes;

// 前置声明（定义在文件后部；applyMbaDeep / applyControlFlowFlattening 先使用）。
uint64_t splitmix64(uint64_t x);
void applyCffDeadBlocks(RegFunction& function, const RegTransformOptions& options,
                        RegValue state, ca::u32& nextRegister, RegTransformStats& stats);

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

RegInstruction makeUnary(RegValue dest, RegOperand value, SSAUnaryOp op)
{
    RegInstruction instruction;
    instruction.kind    = RegInstructionKind::Unary;
    instruction.hasDest = true;
    instruction.dest    = dest;
    instruction.value   = std::move(value);
    instruction.unaryOp = op;
    instruction.effect  = SSAEffect::pure();
    return instruction;
}

RegInstruction makeBarrier(const char* reason)
{
    RegInstruction instruction;
    instruction.kind          = RegInstructionKind::Barrier;
    instruction.barrierReason = reason;
    instruction.effect        = SSAEffect::barrier();
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

/// 深 MBA（#829 P0-2，吸收 Cloakwork De Morgan/进位链 + volatile 防折叠思路）。
///
/// 用 `mj2x_regc_barrier`（RegIR Barrier，发射为运行时调用，副作用语义）夹住
/// 恒等链，C 编译器无法折叠中间步骤。每调用点按 (blockId*7 + 指令序号) 选变体：
/// - 变体 0（De Morgan）：a+b = ~(~a|~b) + ~(~a&~b)（即 (a&b)+(a|b)）
/// - 变体 1（carry 两轮）：a+b = ((a^b) ^ ((a&b)<<1)) + (((a^b) & ((a&b)<<1)) << 1)
/// 均为位运算恒等，溢出安全（模 2³² 语义）。默认关闭（mbaDeep=false 走基础 MBA）。
void applyMbaDeep(RegFunction& function, ca::u32& nextRegister, uint64_t seed,
                  RegTransformStats& stats)
{
    ca::u32 site = 0;
    for (auto& block : function.blocks) {
        std::vector<RegInstruction> transformed;
        transformed.reserve(block->instructions.size() * 10);
        for (const auto& instruction : block->instructions) {
            if (!canRewriteMba(instruction)) {
                transformed.push_back(instruction);
                continue;
            }
            const SSAType type = instruction.dest.type;
            const auto    lhs  = instruction.lhs;
            const auto    rhs  = instruction.rhs;
            const bool    variant = ((splitmix64(seed ^ (site++ * 0x9E3779B97F4A7C15ull)) & 1u) == 0);
            RegValue      r{RegId{nextRegister++}, type};
            if (variant) {
                // De Morgan 展开：t1=~a, t2=~b, t3=t1|t2(=~(a&b)), t4=t1&t2(=~(a|b)),
                // t5=~t3(=a&b), t6=~t4(=a|b), r=t5+t6（恒等式 (a&b)+(a|b)=a+b，
                // 模 2^N 下成立）
                const RegValue t1{RegId{nextRegister++}, type};
                const RegValue t2{RegId{nextRegister++}, type};
                const RegValue t3{RegId{nextRegister++}, type};
                const RegValue t4{RegId{nextRegister++}, type};
                const RegValue t5{RegId{nextRegister++}, type};
                const RegValue t6{RegId{nextRegister++}, type};
                transformed.push_back(makeUnary(t1, lhs, SSAUnaryOp::BitNot));
                transformed.push_back(makeUnary(t2, rhs, SSAUnaryOp::BitNot));
                transformed.push_back(makeBinary(
                    t3, RegOperand::fromRegister(t1), RegOperand::fromRegister(t2),
                    SSABinaryOp::Or));
                transformed.push_back(makeBinary(
                    t4, RegOperand::fromRegister(t1), RegOperand::fromRegister(t2),
                    SSABinaryOp::And));
                transformed.push_back(makeBarrier("mba_deep"));
                transformed.push_back(makeUnary(t5, RegOperand::fromRegister(t3),
                                                SSAUnaryOp::BitNot));
                transformed.push_back(makeUnary(t6, RegOperand::fromRegister(t4),
                                                SSAUnaryOp::BitNot));
                transformed.push_back(makeBarrier("mba_deep"));
                transformed.push_back(makeBinary(r, RegOperand::fromRegister(t5),
                                                 RegOperand::fromRegister(t6), SSABinaryOp::Add));
            }
            else {
                // carry 两轮：t1=a^b, t2=a&b, t3=t2<<1, t4=t1^t3, t5=t1&t3,
                // t6=t5<<1, r=t4+t6
                const RegValue t1{RegId{nextRegister++}, type};
                const RegValue t2{RegId{nextRegister++}, type};
                const RegValue t3{RegId{nextRegister++}, type};
                const RegValue t4{RegId{nextRegister++}, type};
                const RegValue t5{RegId{nextRegister++}, type};
                const RegValue t6{RegId{nextRegister++}, type};
                transformed.push_back(
                    makeBinary(t1, lhs, rhs, SSABinaryOp::Xor));
                transformed.push_back(
                    makeBinary(t2, lhs, rhs, SSABinaryOp::And));
                transformed.push_back(makeBinary(
                    t3, RegOperand::fromRegister(t2), RegOperand::fromInt(1, type),
                    SSABinaryOp::Shl));
                transformed.push_back(makeBarrier("mba_deep"));
                transformed.push_back(makeBinary(
                    t4, RegOperand::fromRegister(t1), RegOperand::fromRegister(t3),
                    SSABinaryOp::Xor));
                transformed.push_back(makeBinary(
                    t5, RegOperand::fromRegister(t1), RegOperand::fromRegister(t3),
                    SSABinaryOp::And));
                transformed.push_back(makeBinary(
                    t6, RegOperand::fromRegister(t5), RegOperand::fromInt(1, type),
                    SSABinaryOp::Shl));
                transformed.push_back(makeBarrier("mba_deep"));
                transformed.push_back(makeBinary(r, RegOperand::fromRegister(t4),
                                                 RegOperand::fromRegister(t6), SSABinaryOp::Add));
            }
            RegInstruction rewritten = instruction;
            rewritten.hasDest        = false;
            transformed.push_back(makeCopy(instruction.dest, RegOperand::fromRegister(r)));
            ++stats.mbaDeepRewrites;
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

void applyOpaquePredicates(RegFunction& function, ca::u32& nextRegister,
                           const RegTransformOptions& options, RegTransformStats& stats)
{
    ca::u32 site = 0;
    for (auto& block : function.blocks) {
        auto* branch = std::get_if<RegBranchTerminator>(&block->terminator);
        if (branch == nullptr) continue;

        RegOperand condition = branch->condition;
        if (!normalizeBranchCondition(*block, condition, nextRegister)) continue;

        const RegOperand source = opaqueSource(function, *block);
        const SSAType    type   = source.type;
        const RegValue   predicate{RegId{nextRegister++}, SSAType::BOOL};
        const RegValue   combined{RegId{nextRegister++}, SSAType::BOOL};
        const RegOperand one = RegOperand::fromInt(1, type);

        if (!options.opaqueZero) {
            // 基础形态：(x | 1) & 1 == 1 —— 恒真。
            const RegValue orValue{RegId{nextRegister++}, type};
            const RegValue andValue{RegId{nextRegister++}, type};
            block->instructions.push_back(makeBinary(orValue, source, one, SSABinaryOp::Or));
            block->instructions.push_back(
                makeBinary(andValue, RegOperand::fromRegister(orValue), one, SSABinaryOp::And));
            block->instructions.push_back(makeCompare(
                predicate, RegOperand::fromRegister(andValue), one, SSAComparePredicate::Eq));
            ++stats.opaquePredicates;
        }
        else {
            // #829 P0-3：opaque-零恒等变体。全部恒真、零 UB（纯位运算/无
            // 溢出乘法），折叠只会降强度、不破坏语义。按 (seed, site) 派生
            // 选变体，同 seed 同产物（可复现构建不变量）。
            const uint64_t sel = splitmix64(options.transformSeed ^ (site++ * 0x85EBCA77u)) & 3u;
            if (sel == 0) {
                // 基础形态保留为变体之一。
                const RegValue orValue{RegId{nextRegister++}, type};
                const RegValue andValue{RegId{nextRegister++}, type};
                block->instructions.push_back(makeBinary(orValue, source, one, SSABinaryOp::Or));
                block->instructions.push_back(
                    makeBinary(andValue, RegOperand::fromRegister(orValue), one, SSABinaryOp::And));
                block->instructions.push_back(makeCompare(
                    predicate, RegOperand::fromRegister(andValue), one,
                    SSAComparePredicate::Eq));
            }
            else if (sel == 1) {
                // demorgan：~(~x) == x（双 BitNot 位恒等，零 UB）。
                const RegValue t1{RegId{nextRegister++}, type};
                const RegValue t2{RegId{nextRegister++}, type};
                block->instructions.push_back(makeUnary(t1, source, SSAUnaryOp::BitNot));
                block->instructions.push_back(
                    makeUnary(t2, RegOperand::fromRegister(t1), SSAUnaryOp::BitNot));
                block->instructions.push_back(makeCompare(
                    predicate, RegOperand::fromRegister(t2), source, SSAComparePredicate::Eq));
            }
            else if (sel == 2) {
                // shift-pair：(x >> 1) << 1 == x & ~1（算术右移再左移 = 清最低
                // 位 = x & ~1；对负数也成立——>>1 后 <<1 只清最低位。零 UB）。
                const RegValue notOne{RegId{nextRegister++}, type};
                const RegValue half{RegId{nextRegister++}, type};
                const RegValue twice{RegId{nextRegister++}, type};
                const RegValue masked{RegId{nextRegister++}, type};
                block->instructions.push_back(makeUnary(notOne, one, SSAUnaryOp::BitNot));
                block->instructions.push_back(makeBinary(
                    half, source, one, SSABinaryOp::Shr));
                block->instructions.push_back(makeBinary(
                    twice, RegOperand::fromRegister(half), one, SSABinaryOp::Shl));
                block->instructions.push_back(makeBinary(
                    masked, source, RegOperand::fromRegister(notOne), SSABinaryOp::And));
                block->instructions.push_back(makeCompare(
                    predicate, RegOperand::fromRegister(twice),
                    RegOperand::fromRegister(masked), SSAComparePredicate::Eq));
            }
            else {
                // or-dec：(x | 1) - 1 == x & ~1（两路径都清最低位；x|1 恒 >= 1，
                // 减法不溢出。零 UB）。
                const RegValue notOne{RegId{nextRegister++}, type};
                const RegValue orOne{RegId{nextRegister++}, type};
                const RegValue dec{RegId{nextRegister++}, type};
                const RegValue masked{RegId{nextRegister++}, type};
                block->instructions.push_back(makeUnary(notOne, one, SSAUnaryOp::BitNot));
                block->instructions.push_back(makeBinary(orOne, source, one, SSABinaryOp::Or));
                block->instructions.push_back(makeBinary(
                    dec, RegOperand::fromRegister(orOne), one, SSABinaryOp::Sub));
                block->instructions.push_back(makeBinary(
                    masked, source, RegOperand::fromRegister(notOne), SSABinaryOp::And));
                block->instructions.push_back(makeCompare(
                    predicate, RegOperand::fromRegister(dec),
                    RegOperand::fromRegister(masked), SSAComparePredicate::Eq));
            }
            ++stats.opaqueZeroPredicates;
        }
        block->instructions.push_back(
            makeBinary(combined, condition, RegOperand::fromRegister(predicate), SSABinaryOp::And));
        branch->condition = RegOperand::fromRegister(combined);
    }
}

/// domain-specific note 见函数体结尾：Barrier 是**唯一**被放行的带 effect 指令。
///
/// 注意判定顺序：Barrier 的 effect.isBarrier 会被 hasObservableEffect() 判为可观
/// 测，因此必须在 effect 检查**之前**特判放行，否则开关形同虚设。
bool isCffPureInstruction(const RegInstruction& instruction)
{
    // Barrier 放行（#X H2）：它虽带 isBarrier effect，但语义上是"不可折叠锚点"——
    // 无数据流产出、不抛异常、不读外部状态，唯一作用是阻止 C 编译器把其相邻的
    // MBA/恒假推导常量化。CFF 只改**块间**路由（原 CFG 边 → dispatcher state
    // 迁移），块内指令序列逐字保留，故 barrier 与其它 effect 的相对顺序不变。
    // 反面后果（实测）：若在此判 false，`--regc-mba-deep` 注入的 barrier 会让
    // 整函数被排除，深 MBA 与 CFF 互为毒药——同一纯算术函数开启两者后 dispatcher
    // 从 12 块退回原始 CFG（保护强度归零且**不报错**，用户无从察觉）。
    if (instruction.kind == RegInstructionKind::Barrier) return true;
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
    case RegInstructionKind::Intrinsic: return false;
    // Barrier 放行（#X H2）：它虽带 isBarrier effect，但语义上是"不可折叠锚点"——
    // 无数据流产出、不抛异常、不读外部状态，唯一作用是阻止 C 编译器把其相邻的
    // MBA/恒假推导常量化。CFF 只改**块间**路由（原 CFG 边 → dispatcher state
    // 迁移），块内指令序列逐字保留，故 barrier 与其它 effect 的相对顺序不变。
    // 反面后果（实测）：若在此判 false，`--regc-mba-deep` 注入的 barrier 会让
    // 整函数被排除，深 MBA 与 CFF 互为毒药——同一纯算术函数开启两者后 dispatcher
    // 从 12 块退回原始 CFG（保护强度归零而不报错）。
    case RegInstructionKind::Barrier: return true;
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

/// splitmix64 派生：确定性、雪崩好，用于随机 state 码与变体选择。
/// 同 seed 同 index → 同值（可复现构建不变量）。单源 = ca::random::SplitMix64
///（libca canonical，#1027 A3 收敛；单步 = 播种后一次 next()，逐位同原本地式）。
uint64_t splitmix64(uint64_t x)
{
    return ca::random::SplitMix64(x).next();
}

/// 派生非零 I32 state 码：splitmix64(seed, index) 低位；0 冲突时退 1。
/// CFF 加密模式下 state 码不再是顺序 1,2,3..，而是伪随机分布。
///
/// 值域收敛到 int32 **正**区间：`RegSwitchCase::key` 以 int64_t 承载，但 RegC
/// 发射为 C `switch` 的 case 常量，u32 最高位置 1 会溢出 int32（clang
/// `-Wswitch: overflow converting case value`，且踩整型转换实现域）。码空间从
/// 2^32 降到 2^31 只抬高碰撞概率，dispatcher 建表处已有查重 + 线性探测兜底。
ca::u32 deriveStateCode(uint64_t seed, ca::u32 index)
{
    const uint64_t mixed = splitmix64(seed ^ (static_cast<uint64_t>(index) * 0x85EBCA77u));
    ca::u32        code  = static_cast<ca::u32>(mixed) & 0x7FFFFFFFu;
    return code == 0 ? 1u : code;
}

/// 状态异或 key：常量（非零），写入值 = code，dispatch 前 state ^= key，
/// case 常量 = code ^ key（二进制中写入常量与比较常量不同）。
/// 同 deriveStateCode 的 int32 正值域约束——key 越界会让 case 常量连带溢出。
ca::u32 deriveStateXorKey(uint64_t seed)
{
    ca::u32 key = static_cast<ca::u32>(splitmix64(seed ^ 0x9E3779B97F4A7C15ull)) & 0x7FFFFFFFu;
    return key == 0 ? 0x9E3779B9u : key;
}

/// 在 dispatcher 块入口插入 `state ^= xorKey`（新增异或寄存器），
/// 使 switch case 常量 = 写入值 ^ xorKey。仅 cffStateEncryption 时调用。
void injectStateXor(RegBlock& dispatcher, RegValue state, ca::u32 xorKey, ca::u32& nextRegister)
{
    const RegValue mixed{RegId{nextRegister++}, SSAType::I32};
    dispatcher.instructions.push_back(
        makeBinary(mixed, RegOperand::fromRegister(state),
                   RegOperand::fromInt(static_cast<int64_t>(xorKey), SSAType::I32),
                   SSABinaryOp::Xor));
    auto* dispatch = std::get_if<RegSwitchTerminator>(&dispatcher.terminator);
    if (dispatch != nullptr) dispatch->value = RegOperand::fromRegister(mixed);
}

bool applyControlFlowFlattening(RegFunction& function, ca::u32& nextRegister,
                                const RegTransformOptions& options, RegTransformStats& stats)
{
    if (!canFlattenControlFlow(function)) return false;

    std::vector<RegBlock*> originals;
    originals.reserve(function.blocks.size());
    for (const auto& block : function.blocks) originals.push_back(block.get());

    std::map<const RegBlock*, ca::u32> stateCodes;
    if (options.cffStateEncryption) {
        // 派生随机 state 码：splitmix64(seed, index)，非顺序。码空间碰撞
        //（生日效应，数千块的函数概率非平凡）会使 dispatcher switch 首个
        // case 命中而静默路由错误——查重，冲突时线性探测下一个值
        //（0 为非法码，绕开）。
        ca::u32           index = 0;
        std::set<ca::u32> usedCodes;
        for (const auto* block : originals) {
            ca::u32 code = deriveStateCode(options.transformSeed, index++);
            while (usedCodes.count(code) != 0) {
                code = code == 0xFFFFFFFFu ? 1u : code + 1u;
            }
            usedCodes.insert(code);
            stateCodes.emplace(block, code);
        }
    }
    else {
        ca::u32 stateCode = 1;
        for (const auto* block : originals) stateCodes.emplace(block, stateCode++);
    }

    const RegBlock* originalEntry = function.entry;
    const RegValue  state{RegId{nextRegister++}, SSAType::I32};
    ca::u32         nextBlock     = nextBlockId(function);
    auto            dispatcher    = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
    RegBlock*       dispatcherPtr = dispatcher.get();
    auto            bootstrap     = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
    RegBlock*       bootstrapPtr  = bootstrap.get();

    RegSwitchTerminator dispatch;
    dispatch.value = RegOperand::fromRegister(state);
    ca::u32         xorKey = 0;
    if (options.cffStateEncryption) {
        xorKey = deriveStateXorKey(options.transformSeed);
        // case 常量 = 写入值 ^ xorKey（dispatcher 入口先异或再比较）。
        for (const auto* block : originals) {
            const auto code = stateCodes.find(block);
            dispatch.cases.push_back(RegSwitchCase{
                static_cast<int64_t>(static_cast<ca::u32>(code->second) ^ xorKey),
                RegSuccessor{const_cast<RegBlock*>(block), {}}});
        }
        dispatch.defaultTarget = RegSuccessor{const_cast<RegBlock*>(originalEntry), {}};
        dispatcher->terminator = std::move(dispatch);
        injectStateXor(*dispatcher, state, xorKey, nextRegister);
    }
    else {
        for (const auto* block : originals) {
            const auto code = stateCodes.find(block);
            dispatch.cases.push_back(RegSwitchCase{static_cast<int64_t>(code->second),
                                                   RegSuccessor{const_cast<RegBlock*>(block), {}}});
        }
        dispatch.defaultTarget = RegSuccessor{const_cast<RegBlock*>(originalEntry), {}};
        dispatcher->terminator = std::move(dispatch);
    }

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
    // 交给发射端：把 state 声明为 volatile，使 dispatcher 不被优化器消解。
    // 语义不受影响（volatile 只约束优化器，不改变单线程语义）；死块内的
    // barrier 已是 volatile 无关的副作用锚点，两者叠加不冲突。
    if (options.cffStateVolatile) function.cffVolatileState = state;
    stats.controlFlowFlattened = true;
    if (options.cffDeadBlockDensity != 0)
        applyCffDeadBlocks(function, options, state, nextRegister, stats);
    return true;
}

/// 在 CFF 平坦化之后注入恒假死块（#829 P0-1，吸收 Cloakwork 死块思路）。
///
/// 结构：每 `density` 个 setter 块，在其跳转 dispatcher 前插入恒假分支
/// `if ((state*state + 1) == 0) goto deadN; else goto dispatcher`。
/// 恒假性：x²+1 ≡ 0 (mod 2³²) 无解（平方模 4 只能为 0/1），而 state 是运行时
/// 值，C 编译器不会做模算术证明 → 分支与死块不可 DCE。死块内容为依赖 state
/// 的多步计算链 + barrier（`mj2x_regc_barrier`，副作用语义），跳回 dispatcher。
/// 死块计算永不执行，不影响语义；指令数 = 1 个死块 ≈ 6-8 条。
///
/// 实现注意：遍历期间不得 push_back（vector realloc 会悬垂 block 指针）；
/// 先收集 setter 索引，reserve 容量后插入死块，最后按索引改 terminator。
void applyCffDeadBlocks(RegFunction& function, const RegTransformOptions& options,
                        RegValue state, ca::u32& nextRegister, RegTransformStats& stats)
{
    if (options.cffDeadBlockDensity == 0 || function.blocks.empty()) return;

    // 第一遍：定位 dispatcher（含 switch terminator 的块）。
    ca::usize dispatcherIndex = static_cast<ca::usize>(-1);
    for (ca::usize i = 0; i < function.blocks.size(); ++i) {
        if (std::get_if<RegSwitchTerminator>(&function.blocks[i]->terminator) != nullptr) {
            dispatcherIndex = i;
            break;
        }
    }
    if (dispatcherIndex == static_cast<ca::usize>(-1)) return;
    RegBlock* dispatcher = function.blocks[dispatcherIndex].get();

    // 第二遍：收集需要注入死块的 setter 块索引（Jump→dispatcher，每 density 个）。
    std::vector<ca::usize> setterIndices;
    ca::u32                counter = 0;
    for (ca::usize i = 0; i < function.blocks.size(); ++i) {
        if (i == dispatcherIndex) continue;
        auto* jump = std::get_if<RegJumpTerminator>(&function.blocks[i]->terminator);
        if (jump == nullptr || jump->target.block != dispatcher) continue;
        if ((counter++ % options.cffDeadBlockDensity) != 0) continue;
        setterIndices.push_back(i);
    }
    if (setterIndices.empty()) return;

    // 第三遍：reserve 容量（防后续 push_back realloc 悬垂）后插入死块。
    function.blocks.reserve(function.blocks.size() + setterIndices.size());
    ca::u32 nextBlock = nextBlockId(function);
    std::vector<RegBlock*> deadBlocks;
    deadBlocks.reserve(setterIndices.size());
    for (ca::usize n = 0; n < setterIndices.size(); ++n) {
        auto dead = std::make_unique<RegBlock>(SSABlockId{nextBlock++});
        const RegValue t1{RegId{nextRegister++}, SSAType::I32};
        const RegValue t2{RegId{nextRegister++}, SSAType::I32};
        const RegValue t3{RegId{nextRegister++}, SSAType::I32};
        const RegValue t4{RegId{nextRegister++}, SSAType::I32};
        dead->instructions.push_back(makeBinary(
            t1, RegOperand::fromRegister(state), RegOperand::fromInt(0x45D9F3B, SSAType::I32),
            SSABinaryOp::Mul));
        dead->instructions.push_back(makeBinary(
            t2, RegOperand::fromRegister(t1), RegOperand::fromInt(0x119DE1F3, SSAType::I32),
            SSABinaryOp::Xor));
        dead->instructions.push_back(makeBarrier("cff_dead"));
        dead->instructions.push_back(makeBinary(
            t3, RegOperand::fromRegister(t2), RegOperand::fromInt(0x85EBCA77, SSAType::I32),
            SSABinaryOp::Add));
        dead->instructions.push_back(makeBinary(
            t4, RegOperand::fromRegister(t3), RegOperand::fromInt(0x27D4EB2D, SSAType::I32),
            SSABinaryOp::Xor));
        dead->instructions.push_back(makeBarrier("cff_dead"));
        dead->terminator = RegJumpTerminator{RegSuccessor{dispatcher, {}}};
        function.blocks.push_back(std::move(dead));
        deadBlocks.push_back(function.blocks.back().get());
        ++stats.cffDeadBlocks;
    }

    // 第四遍：按索引把 setter 改为恒假分支（此时 blocks 稳定，指针有效）。
    for (ca::usize n = 0; n < setterIndices.size(); ++n) {
        RegBlock* setter = function.blocks[setterIndices[n]].get();
        // 恒假条件：(state*state + 1) == 0（模 2³² 无解；state 运行时值防折叠）。
        const RegValue square{RegId{nextRegister++}, SSAType::I32};
        const RegValue plusOne{RegId{nextRegister++}, SSAType::I32};
        const RegValue cond{RegId{nextRegister++}, SSAType::BOOL};
        setter->instructions.push_back(makeBinary(
            square, RegOperand::fromRegister(state), RegOperand::fromRegister(state),
            SSABinaryOp::Mul));
        setter->instructions.push_back(makeBinary(
            plusOne, RegOperand::fromRegister(square), RegOperand::fromInt(1, SSAType::I32),
            SSABinaryOp::Add));
        setter->instructions.push_back(makeCompare(
            cond, RegOperand::fromRegister(plusOne), RegOperand::fromInt(0, SSAType::I32),
            SSAComparePredicate::Eq));
        setter->terminator = RegBranchTerminator{
            RegOperand::fromRegister(cond), RegSuccessor{deadBlocks[n], {}},
            RegSuccessor{dispatcher, {}}};
    }
}

/// 可隐藏操作数判定：仅整型 IntLiteral（I32/I64）。浮点/null/opaque 不动；
/// BOOL/I8/I16/CHAR 窄类型实践中不作为 IntLiteral 操作数出现（lowering 统一
/// I32 计算位宽），保守跳过。
bool canHideImmediate(const RegOperand& operand)
{
    return operand.kind == RegOperandKind::IntLiteral &&
           (operand.type == SSAType::I32 || operand.type == SSAType::I64);
}

/// 隐藏一个整型立即数：`k = const_key(site)`（I64，不透明调用）→
/// `e = Xor(E64, k)`（I64）→ 仅 I32 追加 `Convert` 截断。返回承载还原值的
/// 寄存器操作数；合成指令先于被改写指令追加（同块 SSA 序）。E64 = C64 ^ K
/// 编译期算；K = splitmix64(site ^ kOpaqueConstant) 与 RT 双端逐位同源。
RegOperand hideImmediateOperand(const RegOperand& operand, ca::u32& site, ca::u32& nextRegister,
                                RegTransformStats& stats, std::vector<RegInstruction>& out)
{
    const ca::u32  currentSite = site++;
    const uint64_t key         = deriveConstKey(static_cast<int64_t>(currentSite));
    const uint64_t hidden64    = static_cast<uint64_t>(operand.intValue) ^ key;

    const RegValue keyReg{RegId{nextRegister++}, SSAType::I64};
    RegInstruction  keyInstruction;
    keyInstruction.kind          = RegInstructionKind::Intrinsic;
    keyInstruction.hasDest       = true;
    keyInstruction.dest          = keyReg;
    keyInstruction.intrinsicName = std::string(kConstKeyIntrinsicPrefix) +
                                   std::to_string(currentSite);
    keyInstruction.effect        = SSAEffect::pure();
    out.push_back(std::move(keyInstruction));

    const RegValue xorReg{RegId{nextRegister++}, SSAType::I64};
    out.push_back(makeBinary(xorReg, RegOperand::fromRegister(keyReg),
                             RegOperand::fromInt(static_cast<int64_t>(hidden64), SSAType::I64),
                             SSABinaryOp::Xor));
    ++stats.constantsHidden;

    if (operand.type == SSAType::I64) return RegOperand::fromRegister(xorReg);

    // I32：E64 符号扩展 + 末位 Convert 截断。XOR 低 32 位只依赖操作数低 32 位，
    // (int32)(E32 ^ K32) == (int32)C 恒等。
    const RegValue narrowReg{RegId{nextRegister++}, operand.type};
    RegInstruction  narrow;
    narrow.kind        = RegInstructionKind::Convert;
    narrow.hasDest     = true;
    narrow.dest        = narrowReg;
    narrow.value       = RegOperand::fromRegister(xorReg);
    narrow.convertKind = SSAConvertKind::Numeric;
    narrow.targetType  = operand.type;
    narrow.effect      = SSAEffect::pure();
    out.push_back(std::move(narrow));
    return RegOperand::fromRegister(narrowReg);
}

/// 调用目标间接化的 invoke 形态编码（与 RT 侧 mj2x_regc_call_site 同值）。
int32_t invokeKindOf(const RegInstruction& instruction)
{
    switch (instruction.method.invokeOpcode) {
    case Opcodes::INVOKESTATIC: return 1;
    case Opcodes::INVOKESPECIAL: return 2;
    default: return 0;   // INVOKEVIRTUAL / INVOKEINTERFACE
    }
}

/// 调用目标间接化（对标 jvmtp `obfuscation.calls.*`）。
///
/// 只给**外部**目标（JDK/库方法）打标记：调用点改走 per-signature dispatcher，
/// 身份字符串（owner/name/desc）从调用点移出，静态分析必须先解析
/// (表, 索引) → 条目才能还原调用边——调用图收敛到单一 dispatcher 符号。
///
/// 为什么不改已转换目标：那类调用在发射端有直接 C 调用快路径（0 JNI），
/// 间接化它只会退化成 JNI 调用，纯开销、零混淆收益。
///
/// 真正落到 dispatcher 的是发射端：pass 只置 `RegInstruction::indirectCall`，
/// 发射端在通用 JNI 兜底路径（tryEmitDirectCall / intrinsic / resolved 均未
/// 命中）按标记改发 dispatcher 调用。这样"是否外部"由发射端既有的解析
/// 结果决定，pass 无需跨函数全局信息（保持可复现构建不变量）。
void applyCallIndirections(RegFunction& function, RegTransformStats& stats)
{
    // 同 desc 分组的组内索引（发射端按签名分表，索引空间按组独立）。
    std::map<std::string, int32_t> groupCounters;
    for (auto& block : function.blocks) {
        for (auto& instruction : block->instructions) {
            if (instruction.kind != RegInstructionKind::Call) continue;
            // 自递归静态调用保留原路径（JVM 栈帧语义），其余一律标记。
            if (instruction.method.invokeOpcode == Opcodes::INVOKESTATIC &&
                instruction.method.owner == function.symbol.owner) {
                continue;
            }
            const std::string signature = instruction.method.desc.to_std_string();
            const int32_t     groupIndex = groupCounters[signature]++;
            function.callSites.push_back(RegFunction::CallSite{
                instruction.method.owner.to_std_string(),
                instruction.method.name.to_std_string(),
                signature,
                invokeKindOf(instruction),
                groupIndex});
            instruction.indirectCall = true;
            ++stats.callsIndirected;
        }
    }
}

/// 立即数隐藏（对标 jvmtp `obfuscation.constants` / `.constants.mba`）。
///
/// 纯字面量恒等式必被编译器折叠（`(C^k)^k` 同值异指令也会被重结合折叠），
/// 唯一可行路线是「编译期密钥 + 运行时不可折叠读取」（同 OLLVM constant
/// encryption）：每个整型立即数 C 改写为
///     k = Intrinsic("const.key:<site>")   // 运行时 splitmix64 派生，独立 TU 不透明调用
///     e = Xor(E64, k)                     // E64 = C64 ^ K，编译期算
///     c = Convert(e)                      // 仅 I32：截断回 32 位
/// 原操作数 `IntLiteral(C)` → `Register(c)`。site 为函数内局部计数器（确定性，
/// 可复现构建不变量）；K = splitmix64(site ^ kOpaqueConstant) 与 RT 双端
/// mj2x_regc_const_key / mj2x_llvm_const_key 逐位同源（#1027 A3 + 不透明
/// 常量契约，E2E 真实编译+运行守跨语言一致）。
///
/// 顺序纪律：必须排在 CFF **之后**——`isCffPureInstruction` 的 switch 中
/// `Intrinsic → false`，若先插入合成 Intrinsic 会杀死平坦化（零交互保证）。
/// terminator 的 switch case key 不迭代（dispatcher 表完整性）；CFF 之前
/// 各 pass 生成的字面量（opaque 谓词的 1、state 码）随本 pass 一并隐藏，
/// 语义不变。
///
/// 合成 Intrinsic 留 `SSAEffect::pure()`（无 mayThrow → 不触发 ExceptionCheck；
/// 非 Call → 不进 local ref 统计），与原立即数的效果面一致。
void applyConstantHiding(RegFunction& function, ca::u32& nextRegister, RegTransformStats& stats)
{
    ca::u32 site = 0;
    for (auto& block : function.blocks) {
        std::vector<RegInstruction> transformed;
        transformed.reserve(block->instructions.size() * 4);
        for (const auto& instruction : block->instructions) {
            RegInstruction rewritten = instruction;
            // 按 operand 槽位逐个隐藏（合成指令插在被改写指令之前，同块 SSA 序）。
            // 全槽位统一判定，漏槽位即漏隐藏；dest 是定义不是操作数，不动。
            if (canHideImmediate(rewritten.value))
                rewritten.value = hideImmediateOperand(rewritten.value, site, nextRegister, stats,
                                                       transformed);
            if (canHideImmediate(rewritten.lhs))
                rewritten.lhs =
                    hideImmediateOperand(rewritten.lhs, site, nextRegister, stats, transformed);
            if (canHideImmediate(rewritten.rhs))
                rewritten.rhs =
                    hideImmediateOperand(rewritten.rhs, site, nextRegister, stats, transformed);
            if (canHideImmediate(rewritten.receiver))
                rewritten.receiver = hideImmediateOperand(rewritten.receiver, site, nextRegister,
                                                          stats, transformed);
            if (canHideImmediate(rewritten.index))
                rewritten.index =
                    hideImmediateOperand(rewritten.index, site, nextRegister, stats, transformed);
            for (auto& argument : rewritten.args) {
                if (canHideImmediate(argument))
                    argument =
                        hideImmediateOperand(argument, site, nextRegister, stats, transformed);
            }
            if (rewritten.kind == RegInstructionKind::Copy &&
                canHideImmediate(rewritten.copy.source)) {
                rewritten.copy.source =
                    hideImmediateOperand(rewritten.copy.source, site, nextRegister, stats,
                                         transformed);
            }
            transformed.push_back(std::move(rewritten));
        }
        block->instructions = std::move(transformed);
    }
}

}   // namespace

RegTransformStats applyRegTransformPasses(RegFunction&               function,
                                          const RegTransformOptions& options)
{
    RegTransformStats stats;
    ca::u32           nextRegister = nextRegisterId(function);
    if (options.enableMba) {
        if (options.mbaDeep)
            applyMbaDeep(function, nextRegister, options.transformSeed, stats);
        else
            applyMba(function, nextRegister, stats);
    }
    if (options.enableOpaquePredicates)
        applyOpaquePredicates(function, nextRegister, options, stats);
    if (options.enableControlFlowFlattening)
        applyControlFlowFlattening(function, nextRegister, options, stats);
    // 调用目标间接化：只标记，不改指令形态（发射端按标记换发分派调用）。
    if (options.indirectCallTargets) applyCallIndirections(function, stats);
    // 立即数隐藏必须排 CFF 之后（isCffPureInstruction 对 Intrinsic 返回 false，
    // 先插合成 Intrinsic 会杀死平坦化）。
    if (options.hideImmediateConstants) applyConstantHiding(function, nextRegister, stats);
    return stats;
}

}   // namespace mj2x

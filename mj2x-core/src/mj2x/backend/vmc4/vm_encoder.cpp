#include "mj2x/backend/vmc4/vm_encoder.hpp"

#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/register_ir/reg_instruction.hpp"
#include "mj2x/mir/register_ir/reg_value.hpp"
#include "mj2x/mir/ssa/ssa_instruction.hpp"
#include "mj2x/mir/ssa/ssa_type.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace mj2x {

namespace {

// RegIR / SSA 类型直接在 mj2x 命名空间（无 mj2x::mir 子命名空间）。

// ---------------------------------------------------------------------------
// 字节流写入 helper：小端编码。
// ---------------------------------------------------------------------------
void emitU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void emitU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

/// @brief SSAType → VmType 映射（计算类型子集）。
///
/// @details
/// MVP 只处理 I32/I64/F32/F64。BOOL/I8/I16/CHAR 归一到 I32（计算位宽一致，
/// 见 ssa_type.hpp:38 ssaIsIntLike）。REF/UNKNOWN 在 MVP fail-closed。
bool toVmType(SSAType s, VmType& out)
{
    switch (s) {
    case SSAType::BOOL:
    case SSAType::I8:
    case SSAType::I16:
    case SSAType::CHAR:
    case SSAType::I32: out = VmType::I32; return true;
    case SSAType::I64: out = VmType::I64; return true;
    case SSAType::F32: out = VmType::F32; return true;
    case SSAType::F64: out = VmType::F64; return true;
    default: return false;   // REF/UNKNOWN 等 MVP 不支持
    }
}

/// @brief SSAUnaryOp → VmUnaryOp。
bool toVmUnaryOp(SSAUnaryOp op, VmUnaryOp& out)
{
    switch (op) {
    case SSAUnaryOp::Neg: out = VmUnaryOp::Neg; return true;
    case SSAUnaryOp::BitNot: out = VmUnaryOp::BitNot; return true;
    }
    return false;
}

/// @brief SSABinaryOp → VmBinaryOp。
bool toVmBinaryOp(SSABinaryOp op, VmBinaryOp& out)
{
    switch (op) {
    case SSABinaryOp::Add: out = VmBinaryOp::Add; return true;
    case SSABinaryOp::Sub: out = VmBinaryOp::Sub; return true;
    case SSABinaryOp::Mul: out = VmBinaryOp::Mul; return true;
    case SSABinaryOp::Div: out = VmBinaryOp::Div; return true;
    case SSABinaryOp::Rem: out = VmBinaryOp::Rem; return true;
    case SSABinaryOp::And: out = VmBinaryOp::And; return true;
    case SSABinaryOp::Or: out = VmBinaryOp::Or; return true;
    case SSABinaryOp::Xor: out = VmBinaryOp::Xor; return true;
    case SSABinaryOp::Shl: out = VmBinaryOp::Shl; return true;
    case SSABinaryOp::Shr: out = VmBinaryOp::Shr; return true;
    case SSABinaryOp::UShr: out = VmBinaryOp::UShr; return true;
    }
    return false;
}

/// @brief SSAComparePredicate → VmComparePred。
bool toVmComparePred(SSAComparePredicate p, VmComparePred& out)
{
    switch (p) {
    case SSAComparePredicate::Eq: out = VmComparePred::Eq; return true;
    case SSAComparePredicate::Ne: out = VmComparePred::Ne; return true;
    case SSAComparePredicate::Lt: out = VmComparePred::Lt; return true;
    case SSAComparePredicate::Le: out = VmComparePred::Le; return true;
    case SSAComparePredicate::Gt: out = VmComparePred::Gt; return true;
    case SSAComparePredicate::Ge: out = VmComparePred::Ge; return true;
    }
    return false;
}

/// @brief 编码期上下文：常量池 + block 索引映射 + 寄存器计数。
struct EncoderState
{
    const RegFunction& fn;

    /// block 指针 → 在 fn.blocks 里的索引（也用作 blockTargets/Jump/Branch 的目标索引）。
    std::unordered_map<const RegBlock*, uint16_t> blockIndex;

    /// 每个常量的签名 → 在 constantPool 的索引（去重）。
    /// 签名 = type(1B) + intBits(8B) / floatValue(8B)，用字符串当 key 简化去重。
    std::unordered_map<std::string, uint16_t> constSlotBySignature;

    /// 常量寄存器槽从 maxReg 高水位之后分配（见 internConstant），起始索引
    /// 取决于计算寄存器扫描结果，而非固定 numInputs。
    uint16_t numInputs = 0;

    /// 已分配的最大寄存器索引 + 1（含参数、常量槽、计算寄存器）。
    uint16_t maxReg = 0;

    explicit EncoderState(const RegFunction& f)
        : fn(f)
    {
        numInputs = static_cast<uint16_t>(f.parameters.size());
        // 预扫描全部计算寄存器（指令 dest + Register 操作数），把常量槽隔离到
        // 计算寄存器空间之外——否则常量槽与 SSA value id（密集递增）重叠，
        // 常量复用或槽被后续 dest 覆盖后读参即错值。
        maxReg = numInputs;
        scanComputeRegisters();
    }

    /// @brief 第一遍：扫描所有指令的 dest 与 Register 操作数，抬高 maxReg。
    ///
    /// @details
    /// 只观察 Register 操作数，字面量由 internConstant 在编码期分配槽位；
    /// 由于 maxReg 已含全部计算寄存器，常量槽必然落在计算寄存器之后，互不重叠。
    void scanComputeRegisters()
    {
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block->instructions) {
                observeReg(static_cast<uint16_t>(ins.dest.id.value));
                switch (ins.kind) {
                case RegInstructionKind::Copy:
                case RegInstructionKind::Unary:
                    observeReg(static_cast<uint16_t>(ins.value.reg.value));
                    break;
                case RegInstructionKind::Binary:
                case RegInstructionKind::Compare:
                    observeReg(static_cast<uint16_t>(ins.lhs.reg.value));
                    observeReg(static_cast<uint16_t>(ins.rhs.reg.value));
                    break;
                case RegInstructionKind::Convert:
                    observeReg(static_cast<uint16_t>(ins.value.reg.value));
                    break;
                default: break;
                }
            }
            const auto& term = block->terminator;
            std::visit([&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    observeReg(static_cast<uint16_t>(t.condition.reg.value));
                } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (t.hasValue) observeReg(static_cast<uint16_t>(t.value.reg.value));
                }
            },
                       term);
        }
    }

    /// 更新 maxReg（计算寄存器索引来自 RegId.value）。
    void observeReg(uint16_t idx)
    {
        if (idx >= maxReg) maxReg = idx + 1;
    }

    /// @brief 为字面量分配/复用一个常量寄存器槽。
    ///
    /// @details
    /// 常量槽从 maxReg 高水位（已含全部计算寄存器）之后连续分配，与计算寄存器
    /// 空间隔离。相同签名的字面量复用同一槽（去重）。
    /// 返回的索引写入 bytecodes 作为操作数，prologue 据此从 constantPool 预加载。
    uint16_t internConstant(VmType type, int64_t intBits, double floatValue)
    {
        std::string sig;
        sig.push_back(static_cast<char>(type));
        sig.append(reinterpret_cast<const char*>(&intBits), sizeof(int64_t));
        sig.append(reinterpret_cast<const char*>(&floatValue), sizeof(double));

        auto it = constSlotBySignature.find(sig);
        if (it != constSlotBySignature.end()) {
            observeReg(it->second);
            return it->second;
        }

        const uint16_t slot = maxReg;   // 常量槽紧接在已分配寄存器之后
        observeReg(slot);
        constSlotBySignature[sig] = slot;
        return slot;
    }

    /// @brief 解析一个 RegOperand 为 VM 寄存器索引。
    ///
    /// @details
    /// - Register：直接用 reg.value（计算寄存器）
    /// - IntLiteral/FloatLiteral：intern 到常量池，返回常量槽索引
    /// - NullLiteral/Opaque：MVP 不支持（fail-closed）
    ///
    /// @return 成功返回 true 并填 regIdx；失败返回 false 并填 reason。
    bool resolveOperand(const RegOperand& op, uint16_t& regIdx, std::string& reason)
    {
        switch (op.kind) {
        case RegOperandKind::Register: {
            regIdx = static_cast<uint16_t>(op.reg.value);
            observeReg(regIdx);
            return true;
        }
        case RegOperandKind::IntLiteral: {
            VmType vt;
            if (!toVmType(op.type, vt)) {
                reason = "IntLiteral with unsupported VM type";
                return false;
            }
            regIdx = internConstant(vt, op.intValue, 0.0);
            return true;
        }
        case RegOperandKind::FloatLiteral: {
            VmType vt;
            if (!toVmType(op.type, vt)) {
                reason = "FloatLiteral with unsupported VM type";
                return false;
            }
            regIdx = internConstant(vt, 0, op.floatValue);
            return true;
        }
        case RegOperandKind::NullLiteral:
            reason = "NullLiteral operand (object ref) not supported in MVP";
            return false;
        case RegOperandKind::Opaque:
            reason = "Opaque operand not supported in MVP";
            return false;
        }
        reason = "unknown RegOperandKind";
        return false;
    }
};

// ---------------------------------------------------------------------------
// 前向声明：layoutBlocks → encodeBlock → encodeInstruction/encodeTerminator →
// encodeCopy/Unary/Binary/Compare/Convert/Jump/Branch/Return。
// ---------------------------------------------------------------------------
bool layoutBlocks(EncoderState& ctx, VmProgram& prog, std::string& reason);
bool encodeBlock(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason);
bool encodeInstruction(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog,
                       std::string& reason);
bool encodeTerminator(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason);
bool encodeCopy(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeUnary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeBinary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeCompare(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeConvert(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeJump(EncoderState& ctx, const RegJumpTerminator& term, VmProgram& prog,
                std::string& reason);
bool encodeBranch(EncoderState& ctx, const RegBranchTerminator& term, VmProgram& prog,
                  std::string& reason);
bool encodeReturn(EncoderState& ctx, const RegReturnTerminator& term, VmProgram& prog,
                  std::string& reason);

/// @brief 第一遍：为每个 block 分配索引，记录到 ctx.blockIndex。
///
/// @details
/// block 索引用作 blockTargets 的下标和 Jump/Branch 的目标操作数。
/// fn.blocks 的顺序即索引顺序（entry 是 blocks[0]，见 reg_function.hpp:38）。
bool indexBlocks(EncoderState& ctx, std::string& reason)
{
    if (ctx.fn.blocks.size() > 0xFFFF) {
        reason = "too many blocks (>65535) for u16 block index";
        return false;
    }
    uint16_t idx = 0;
    for (const auto& block : ctx.fn.blocks) {
        ctx.blockIndex[block.get()] = idx++;
    }
    return true;
}

/// @brief 第二遍 A：计算每个 block 的字节码长度，填 blockTargets（起始 offset）。
///
/// @details
/// 一遍完成：按 block 顺序边编码边记录起始 offset（block 的偏移只取决于前面
/// block 的字节数，故一遍即够）。末尾追加 Halt 作为停机哨兵。
bool layoutBlocks(EncoderState& ctx, VmProgram& prog, std::string& reason)
{
    prog.blockTargets.assign(ctx.fn.blocks.size(), 0);
    for (size_t bi = 0; bi < ctx.fn.blocks.size(); ++bi) {
        prog.blockTargets[bi] = static_cast<uint32_t>(prog.bytecodes.size());
        // 编码本 block 到 prog.bytecodes（offset 已记录，后续编码直接追加）。
        if (!encodeBlock(ctx, *ctx.fn.blocks[bi], prog, reason)) return false;
    }
    // 末尾 Halt 作为停机哨兵（解释器主循环遇到 Halt 退出）。
    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Halt));
    return true;
}

/// @brief 编码单个 block 的指令 + terminator。
///
/// @details
/// 在 layoutBlocks 里被调用，直接追加到 prog.bytecodes。block 起始 offset
/// 已由调用方记录到 blockTargets。
bool encodeBlock(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason)
{
    for (const auto& ins : block.instructions) {
        if (!encodeInstruction(ctx, ins, prog, reason)) return false;
    }
    return encodeTerminator(ctx, block, prog, reason);
}

bool encodeInstruction(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog,
                       std::string& reason)
{
    switch (ins.kind) {
    case RegInstructionKind::Copy: return encodeCopy(ctx, ins, prog, reason);
    case RegInstructionKind::Unary: return encodeUnary(ctx, ins, prog, reason);
    case RegInstructionKind::Binary: return encodeBinary(ctx, ins, prog, reason);
    case RegInstructionKind::Compare: return encodeCompare(ctx, ins, prog, reason);
    case RegInstructionKind::Convert: return encodeConvert(ctx, ins, prog, reason);
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier:
        reason = "RegIR family not supported in VMC4 MVP (object/array/call/intrinsic)";
        return false;
    }
    reason = "unknown RegInstructionKind";
    return false;
}

bool encodeCopy(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Copy dest type not supported in MVP";
        return false;
    }
    uint16_t src;
    if (!ctx.resolveOperand(ins.value, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Copy));
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

bool encodeUnary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Unary dest type not supported in MVP";
        return false;
    }
    VmUnaryOp op;
    if (!toVmUnaryOp(ins.unaryOp, op)) {
        reason = "Unary op not supported";
        return false;
    }
    uint16_t src;
    if (!ctx.resolveOperand(ins.value, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Unary));
    emitU8(prog.bytecodes, static_cast<uint8_t>(op));
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

bool encodeBinary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Binary dest type not supported in MVP";
        return false;
    }
    VmBinaryOp op;
    if (!toVmBinaryOp(ins.binaryOp, op)) {
        reason = "Binary op not supported";
        return false;
    }
    uint16_t lhs, rhs;
    if (!ctx.resolveOperand(ins.lhs, lhs, reason)) return false;
    if (!ctx.resolveOperand(ins.rhs, rhs, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Binary));
    emitU8(prog.bytecodes, static_cast<uint8_t>(op));
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, lhs);
    emitU16(prog.bytecodes, rhs);
    return true;
}

bool encodeCompare(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // Compare 结果类型恒为 I32（布尔条件），但 operand 类型来自 lhs/rhs。
    VmType operandType;
    if (!toVmType(ins.lhs.type, operandType)) {
        reason = "Compare operand type not supported in MVP";
        return false;
    }
    VmComparePred pred;
    if (!toVmComparePred(ins.comparePredicate, pred)) {
        reason = "Compare predicate not supported";
        return false;
    }
    uint16_t lhs, rhs;
    if (!ctx.resolveOperand(ins.lhs, lhs, reason)) return false;
    if (!ctx.resolveOperand(ins.rhs, rhs, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Compare));
    emitU8(prog.bytecodes, static_cast<uint8_t>(pred));
    emitU8(prog.bytecodes, static_cast<uint8_t>(operandType));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, lhs);
    emitU16(prog.bytecodes, rhs);
    return true;
}

bool encodeConvert(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // MVP 只支持 Numeric convert（ReferenceCast 即 CHECKCAST 属对象语义，fail-closed）。
    if (ins.convertKind != SSAConvertKind::Numeric) {
        reason = "Convert: ReferenceCast not supported in MVP";
        return false;
    }
    VmType from, to;
    if (!toVmType(ins.value.type, from)) {
        reason = "Convert source type not supported in MVP";
        return false;
    }
    if (!toVmType(ins.targetType, to)) {
        reason = "Convert target type not supported in MVP";
        return false;
    }
    uint16_t src;
    if (!ctx.resolveOperand(ins.value, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Convert));
    emitU8(prog.bytecodes, static_cast<uint8_t>(from));
    emitU8(prog.bytecodes, static_cast<uint8_t>(to));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

bool encodeTerminator(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason)
{
    // std::variant 访问：按 terminator 类型分派。
    return std::visit(
        [&](const auto& term) -> bool {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                return encodeJump(ctx, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                return encodeBranch(ctx, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                return encodeReturn(ctx, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                reason = "Switch terminator not supported in MVP";
                (void)term;
                return false;
            } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                reason = "Throw terminator not supported in MVP";
                (void)term;
                return false;
            } else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                // Unreachable：发射 Halt 作为防御性停机（不应被执行到）。
                emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Halt));
                (void)term;
                return true;
            }
            reason = "unknown terminator kind";
            return false;
        },
        block.terminator);
}

bool encodeJump(EncoderState& ctx, const RegJumpTerminator& term, VmProgram& prog,
                std::string& reason)
{
    const auto it = ctx.blockIndex.find(term.target.block);
    if (it == ctx.blockIndex.end()) {
        reason = "Jump target block not indexed";
        return false;
    }
    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Jump));
    emitU16(prog.bytecodes, it->second);
    return true;
}

bool encodeBranch(EncoderState& ctx, const RegBranchTerminator& term, VmProgram& prog,
                  std::string& reason)
{
    uint16_t cond;
    if (!ctx.resolveOperand(term.condition, cond, reason)) return false;
    const auto trueIt = ctx.blockIndex.find(term.trueTarget.block);
    const auto falseIt = ctx.blockIndex.find(term.falseTarget.block);
    if (trueIt == ctx.blockIndex.end() || falseIt == ctx.blockIndex.end()) {
        reason = "Branch target block not indexed";
        return false;
    }
    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Branch));
    emitU16(prog.bytecodes, cond);
    emitU16(prog.bytecodes, trueIt->second);
    emitU16(prog.bytecodes, falseIt->second);
    return true;
}

bool encodeReturn(EncoderState& ctx, const RegReturnTerminator& term, VmProgram& prog,
                  std::string& reason)
{
    if (!term.hasValue) {
        emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Return));
        emitU8(prog.bytecodes, 0);   // has_value = 0
        emitU16(prog.bytecodes, 0);  // src unused
        return true;
    }
    uint16_t src;
    if (!ctx.resolveOperand(term.value, src, reason)) return false;
    emitU8(prog.bytecodes, static_cast<uint8_t>(VmOpcode::Return));
    emitU8(prog.bytecodes, 1);   // has_value = 1
    emitU16(prog.bytecodes, src);
    return true;
}

}   // namespace

// ===========================================================================
// VmEncoder 公开入口
// ===========================================================================
std::unique_ptr<VmProgram> VmEncoder::encode(const RegFunction& fn, const VmInstance& instance,
                                             std::string& reason)
{
    (void)instance;   // MVP：instance 只影响命名，不影响编码（Phase 2 起影响寄存器映射）

    auto prog = std::make_unique<VmProgram>();
    EncoderState ctx(fn);

    if (!indexBlocks(ctx, reason)) return nullptr;
    if (!layoutBlocks(ctx, *prog, reason)) return nullptr;

    // 填充常量池：constSlotBySignature 存 (签名 → 槽索引)，签名含 type+intBits+floatValue。
    // 常量槽全部落在计算寄存器之后（EncoderState 构造期预扫描保证隔离），
    // 收集 (slotIndex, entry) 对后按 slot 排序，prologue 按 entry.destSlot 写入。
    std::vector<std::pair<uint16_t, std::pair<std::string, VmConstantEntry>>> sortedConsts;
    for (const auto& kv : ctx.constSlotBySignature) {
        const std::string& sig = kv.first;
        const uint16_t     slot = kv.second;
        VmConstantEntry    entry{};
        entry.type     = static_cast<VmType>(static_cast<uint8_t>(sig[0]));
        entry.destSlot = slot;
        std::memcpy(&entry.intBits, sig.data() + 1, sizeof(int64_t));
        std::memcpy(&entry.floatValue, sig.data() + 1 + sizeof(int64_t), sizeof(double));
        sortedConsts.emplace_back(slot, std::make_pair(sig, entry));
    }
    std::sort(sortedConsts.begin(), sortedConsts.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    prog->constantPool.reserve(sortedConsts.size());
    for (auto& sc : sortedConsts) {
        prog->constantPool.push_back(sc.second.second);
    }

    prog->numInputs = ctx.numInputs;
    prog->numRegs   = ctx.maxReg;
    return prog;
}

}   // namespace mj2x

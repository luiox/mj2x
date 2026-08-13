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

/// @brief SSAType → VmType 映射（计算类型 + 对象引用）。
///
/// @details
/// I32/I64/F32/F64 是计算类型；BOOL/I8/I16/CHAR 归一到 I32（计算位宽一致，
/// 见 ssa_type.hpp:38 ssaIsIntLike）。REF 映射到 VmType::REF（对象引用，
/// Phase 2.0 起支持，用于对象参数/返回值/字段/Copy）。UNKNOWN 不支持。
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
    case SSAType::REF: out = VmType::REF; return true;
    default: return false;   // UNKNOWN 等不支持
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

/// @brief SSAType → VmJniKind（per-callsite helper marshalling 用）。
///
/// @details
/// boolean/byte/char/short/int 归 Int32（JNI 侧 jint，slot .i）；long → Int64（slot .i）；
/// float/double → Float/Double（slot .f）；REF → Ref（slot .ref）。UNKNOWN 不应出现在
/// 已通过 toVmType 校验的调用点，兜底归 Void（helper 不读该参数，调用结果错位会显式失败）。
VmJniKind toVmJniKind(SSAType s)
{
    switch (s) {
    case SSAType::BOOL:
    case SSAType::I8:
    case SSAType::I16:
    case SSAType::CHAR:
    case SSAType::I32: return VmJniKind::Int32;
    case SSAType::I64: return VmJniKind::Int64;
    case SSAType::F32: return VmJniKind::Float;
    case SSAType::F64: return VmJniKind::Double;
    case SSAType::REF: return VmJniKind::Ref;
    default: return VmJniKind::Void;
    }
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
    const VmInstance&  instance;

    /// block 指针 → 在 fn.blocks 里的索引（也用作 blockTargets/Jump/Branch 的目标索引）。
    std::unordered_map<const RegBlock*, uint16_t> blockIndex;

    /// 每个常量的签名 → 在 constantPool 的索引（去重）。
    /// 签名 = type(1B) + intBits(8B) / floatValue(8B)，用字符串当 key 简化去重。
    std::unordered_map<std::string, uint16_t> constSlotBySignature;

    /// Call 调用点的方法引用表（Phase 2.0）：每个 Call 指令收集一个 VmMethodRef，
    /// 字节流只存 callSiteIdx（索引）。backend 为每项发射一个 TU-local helper。
    std::vector<VmMethodRef>                                  methodRefs;
    std::unordered_map<std::string, uint16_t>                methodRefByKey;

    /// 常量寄存器槽从 maxReg 高水位之后分配（见 internConstant），起始索引
    /// 取决于计算寄存器扫描结果，而非固定 numInputs。
    uint16_t numInputs = 0;

    /// 已分配的最大寄存器索引 + 1（含参数、常量槽、计算寄存器）。
    uint16_t maxReg = 0;

    /// 指令发射计数器：pickOpcodeByte 的 nonce（每条指令递增，确定性选别名）。
    uint64_t instructionNonce = 0;

    /// 带边拷贝（块参数 RegCopy）的边 → 合成块。合成块内容 = [Copy...] + Jump 真实目标，
    /// 索引排在所有真实块之后（blockTargets 后半段）。每条带拷贝的边一个合成块，
    /// 按 (src 块, dst 块, slot) 区分（slot：Jump=0，Branch true=0/false=1）。
    struct SyntheticEdge
    {
        const RegBlock*           realTarget;
        std::vector<RegCopy>      copies;   ///< 边拷贝快照（值语义，安全持有）
        uint16_t                  index = 0;
    };
    std::vector<SyntheticEdge> syntheticEdges;

    /// (src 块, dst 块, slot) → 合成块索引，供 encodeJump/encodeBranch 解析目标。
    struct EdgeKey
    {
        const RegBlock* src = nullptr;
        const RegBlock* dst = nullptr;
        uint16_t        slot = 0;
    };
    std::vector<std::pair<EdgeKey, uint16_t>> edgeToSynthetic;

    explicit EncoderState(const RegFunction& f, const VmInstance& inst)
        : fn(f)
        , instance(inst)
    {
        numInputs = static_cast<uint16_t>(f.parameters.size());
        // 预扫描全部计算寄存器（指令 dest + Register 操作数），把常量槽隔离到
        // 计算寄存器空间之外——否则常量槽与 SSA value id（密集递增）重叠，
        // 常量复用或槽被后续 dest 覆盖后读参即错值。
        maxReg = numInputs;
        scanComputeRegisters();
    }

    /// @brief 为带边拷贝的后继创建合成块并分配索引（在 indexBlocks 之后调用）。
    ///
    /// @details
    /// SSA 块参数经 RegIR 消除后变成后继边上的 RegCopy（reg_lowering.cpp lowerSuccessor）。
    /// 这些拷贝必须在该边被取用时执行——MVP 的解释器只有 Copy/Jump/Branch 原语，
    /// 因此把每条带拷贝的边重写为「合成块：Copy... + Jump 真实目标」，跳转/分支
    /// 的目标指向合成块。无拷贝的边仍直接指向真实块。
    void planSyntheticEdges()
    {
        const uint16_t base = static_cast<uint16_t>(fn.blocks.size());
        auto plan = [&](const RegBlock* src, const RegSuccessor& succ, uint16_t slot) {
            if (succ.block == nullptr || succ.copies.empty()) return;
            SyntheticEdge edge;
            edge.realTarget = succ.block;
            edge.copies     = succ.copies;
            edge.index      = static_cast<uint16_t>(base + syntheticEdges.size());
            syntheticEdges.push_back(std::move(edge));
            edgeToSynthetic.push_back({{src, succ.block, slot}, syntheticEdges.back().index});
        };
        for (const auto& block : fn.blocks) {
            std::visit(
                [&](const auto& term) {
                    using T = std::decay_t<decltype(term)>;
                    if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                        plan(block.get(), term.target, 0);
                    } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                        plan(block.get(), term.trueTarget, 0);
                        plan(block.get(), term.falseTarget, 1);
                    }
                    // Switch/Throw/Return/Unreachable：MVP 分别 fail-closed/无后继，
                    // 无需合成块。
                },
                block->terminator);
        }
    }

    /// @brief 解析边目标：该边有合成块则用合成块索引，否则用真实块索引。
    uint16_t resolveEdgeTarget(const RegBlock* src, const RegBlock* dst, uint16_t slot,
                               uint16_t realIndex)
    {
        for (const auto& kv : edgeToSynthetic) {
            if (kv.first.src == src && kv.first.dst == dst && kv.first.slot == slot)
                return kv.second;
        }
        return realIndex;
    }

    /// @brief 第一遍：扫描所有指令的 dest 与 Register 操作数，抬高 maxReg。
    ///
    /// @details
    /// 只观察 Register 操作数，字面量由 internConstant 在编码期分配槽位；
    /// 由于 maxReg 已含全部计算寄存器，常量槽必然落在计算寄存器之后，互不重叠。
    /// 边拷贝（后继块的块参数 RegCopy）也会占寄存器，必须一并扫描，否则常量槽
    /// 可能落在边拷贝 dest 寄存器之上被覆盖。
    void scanComputeRegisters()
    {
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block->instructions) {
                observeReg(static_cast<uint16_t>(ins.dest.id.value));
                switch (ins.kind) {
                case RegInstructionKind::Copy:
                    observeReg(static_cast<uint16_t>(ins.copy.source.reg.value));
                    break;
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
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    for (const auto& copy : t.target.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    observeReg(static_cast<uint16_t>(t.condition.reg.value));
                    for (const auto& copy : t.trueTarget.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                    for (const auto& copy : t.falseTarget.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
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

    /// @brief 为一个 Call 调用点分配/复用方法引用索引（Phase 2.0）。
    ///
    /// @details
    /// 相同 (owner, name, desc, invokeKind, hasDest) 的调用点复用同一 callSiteIdx，
    /// 共享一个 backend 发射的 helper。返回值写入字节流作为 Call 指令的操作数。
    uint16_t internMethodRef(VmMethodRef mref)
    {
        std::string key = mref.owner;
        key.push_back('\0');
        key += mref.name;
        key.push_back('\0');
        key += mref.desc;
        key.push_back('\0');
        key.push_back(static_cast<char>(mref.invokeKind));
        key.push_back(mref.hasDest ? '\1' : '\0');
        auto it = methodRefByKey.find(key);
        if (it != methodRefByKey.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(methodRefs.size());
        methodRefByKey[key] = idx;
        methodRefs.push_back(std::move(mref));
        return idx;
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
            if (!op.tag.empty()) reason += " (tag: " + op.tag + ")";
            return false;
        }
        reason = "unknown RegOperandKind";
        return false;
    }
};

/// 写入 VmOpcode 的实例字节。drift 开启时按字节流偏移选变体（语义标签轮转），
/// 与解释器 variantForPc 同一公式天然同步；nonce 负责 canonical/ALT 别名的
/// 确定性选取（同一语义集合内多字节）。
void emitOpcode(EncoderState& ctx, VmOpcode op, std::vector<uint8_t>& out)
{
    const uint64_t pc = out.size();
    uint8_t byte;
    if (ctx.instance.driftEnabled()) {
        const int d = ctx.instance.variantForPc(pc);
        byte = ctx.instance.variantPickOpcodeByte(op, ctx.instructionNonce++, d);
    } else {
        byte = ctx.instance.pickOpcodeByte(op, ctx.instructionNonce++);
    }
    emitU8(out, byte);
}

/// 发射 Halt 哨兵：固定用 canonical 字节（发射器的 Halt case 覆盖全部别名，
/// 别名槽在字节流里不会出现，仅作为防御性停机面）。
void emitHaltOpcode(EncoderState& ctx, std::vector<uint8_t>& out)
{
    emitU8(out, ctx.instance.opcodeFor(VmOpcode::Halt));
}

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
bool encodeCall(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
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
/// block 的字节数，故一遍即够）。真实块之后布局合成块（边拷贝块，见
/// EncoderState::planSyntheticEdges），末尾追加 Halt 作为停机哨兵。
bool layoutBlocks(EncoderState& ctx, VmProgram& prog, std::string& reason)
{
    prog.blockTargets.assign(ctx.fn.blocks.size() + ctx.syntheticEdges.size(), 0);
    for (size_t bi = 0; bi < ctx.fn.blocks.size(); ++bi) {
        prog.blockTargets[bi] = static_cast<uint32_t>(prog.bytecodes.size());
        // 编码本 block 到 prog.bytecodes（offset 已记录，后续编码直接追加）。
        if (!encodeBlock(ctx, *ctx.fn.blocks[bi], prog, reason)) return false;
    }
    // 合成块：边拷贝 + Jump 到真实目标。
    for (const auto& edge : ctx.syntheticEdges) {
        prog.blockTargets[edge.index] = static_cast<uint32_t>(prog.bytecodes.size());
        for (const auto& copy : edge.copies) {
            VmType type;
            if (!toVmType(copy.dest.type, type)) {
                reason = "edge copy dest type not supported in MVP";
                return false;
            }
            uint16_t src;
            if (!ctx.resolveOperand(copy.source, src, reason)) return false;
            emitOpcode(ctx, VmOpcode::Copy, prog.bytecodes);
            emitU8(prog.bytecodes, static_cast<uint8_t>(type));
            emitU16(prog.bytecodes, static_cast<uint16_t>(copy.dest.id.value));
            emitU16(prog.bytecodes, src);
        }
        const auto realIt = ctx.blockIndex.find(edge.realTarget);
        if (realIt == ctx.blockIndex.end()) {
            reason = "edge copy target block not indexed";
            return false;
        }
        emitOpcode(ctx, VmOpcode::Jump, prog.bytecodes);
        emitU16(prog.bytecodes, realIt->second);
    }
    // 末尾 Halt 作为停机哨兵（解释器主循环遇到 Halt 退出）。
    emitHaltOpcode(ctx, prog.bytecodes);
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
    case RegInstructionKind::Call: return encodeCall(ctx, ins, prog, reason);
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier:
        reason = "RegIR family not supported in VMC4 yet (field/array/alloc/intrinsic)";
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
    // Copy 指令的源操作数存在 ins.copy.source（不是 ins.value——那是 Unary/Convert
    // 用的字段，对 Copy 是默认构造的 Opaque）。
    uint16_t src;
    if (!ctx.resolveOperand(ins.copy.source, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Copy, prog.bytecodes);
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

    emitOpcode(ctx, VmOpcode::Unary, prog.bytecodes);
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

    // super-operator 折叠（Phase 3）：Binary(x, 1, Add/Sub) → SuperInc/SuperDec。
    // 语义等价（整数环绕/浮点 ±1 一致），但指令格式更短（无 rhs operand），
    // 且引入独立 opcode 增加指令集膨胀。rhs 必须是 IntLiteral(1) 字面量。
    if (ins.rhs.kind == RegOperandKind::IntLiteral && ins.rhs.intValue == 1 &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub)) {
        const VmOpcode superOp = ins.binaryOp == SSABinaryOp::Add ? VmOpcode::SuperInc
                                                                  : VmOpcode::SuperDec;
        uint16_t src;
        if (!ctx.resolveOperand(ins.lhs, src, reason)) return false;
        const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
        ctx.observeReg(dest);
        emitOpcode(ctx, superOp, prog.bytecodes);
        emitU8(prog.bytecodes, static_cast<uint8_t>(type));
        emitU16(prog.bytecodes, dest);
        emitU16(prog.bytecodes, src);
        return true;
    }

    uint16_t lhs, rhs;
    if (!ctx.resolveOperand(ins.lhs, lhs, reason)) return false;
    if (!ctx.resolveOperand(ins.rhs, rhs, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Binary, prog.bytecodes);
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

    emitOpcode(ctx, VmOpcode::Compare, prog.bytecodes);
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

    emitOpcode(ctx, VmOpcode::Convert, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(from));
    emitU8(prog.bytecodes, static_cast<uint8_t>(to));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

/// @brief 编码 Call 指令（Phase 2.0）。
///
/// @details
/// 把 SSAMethodRef 收集到 VmProgram.methodRefs，字节流只存 callSiteIdx。
/// backend 为每个 methodRef 发射一个 TU-local helper（按 invokeKind + desc 调 JNI），
/// 解释器经函数指针表 mj2x_vm_call_table_<tag>[callSiteIdx] 分派。
///
/// 字节流：[Call][callSiteIdx:u16][recv:u16][dest:u16][argCount:u8][args:argCount×u16]
/// - recv：receiver 寄存器（static 调用时填 0，helper 不读）
/// - dest：结果寄存器（void 方法填 0，helper 不写）
bool encodeCall(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // JVM invoke opcode → VmInvokeKind（182=virtual,183=special,184=static,185=interface）。
    VmInvokeKind ik;
    switch (ins.method.invokeOpcode) {
    case 182: ik = VmInvokeKind::Virtual; break;
    case 183: ik = VmInvokeKind::Special; break;
    case 184: ik = VmInvokeKind::Static; break;
    case 185: ik = VmInvokeKind::Interface; break;
    default:
        reason = "Call: unknown invoke opcode " + std::to_string(ins.method.invokeOpcode);
        return false;
    }
    const bool isStatic = (ik == VmInvokeKind::Static);

    // SSA 约定（ssa_instruction.hpp:223）：instance 调用 args[0]=receiver，其后是描述符
    // 参数；static 调用 args 全是描述符参数。RegInstruction.receiver 字段 Call 不填（留
    // 默认 Opaque），receiver 经 args[0] 取。故 argKinds/argIdxs 只含描述符参数。
    const size_t startArg = isStatic ? 0 : 1;
    if (!isStatic && ins.args.empty()) {
        reason = "Call: instance invoke missing receiver (args[0])";
        return false;
    }

    VmMethodRef mref;
    mref.owner      = ins.method.owner.to_std_string();
    mref.name       = ins.method.name.to_std_string();
    mref.desc       = ins.method.desc.to_std_string();
    mref.invokeKind = ik;
    mref.hasDest    = ins.hasDest;
    mref.retKind    = ins.hasDest ? toVmJniKind(ins.dest.type) : VmJniKind::Void;
    // argKinds 仅含描述符参数（不含 receiver），与 desc 对齐，与下方 argIdxs 同序。
    mref.argKinds.reserve(ins.args.size() - startArg);
    for (size_t a = startArg; a < ins.args.size(); ++a) {
        mref.argKinds.push_back(toVmJniKind(ins.args[a].type));
    }
    const uint16_t callSiteIdx = ctx.internMethodRef(std::move(mref));

    // receiver 寄存器（instance 调用取 args[0]）；static 调用 recv=0（helper 不读）。
    uint16_t recv = 0;
    if (!isStatic) {
        if (!ctx.resolveOperand(ins.args[0], recv, reason)) return false;
    }

    // 返回值寄存器（void 方法无 dest）。
    uint16_t dest = 0;
    if (ins.hasDest) {
        dest = static_cast<uint16_t>(ins.dest.id.value);
        ctx.observeReg(dest);
    }

    // 描述符参数寄存器列表（变长，不含 receiver）。argCount 上限 32：解释器 handler
    // 用固定大小 argBuf[32] 承载参数索引数组（避免动态分配）；JVM 实际参数极少超此数。
    const size_t descArgCount = ins.args.size() - startArg;
    if (descArgCount > 32) {
        reason = "Call: too many arguments (>32)";
        return false;
    }
    std::vector<uint16_t> argIdxs;
    argIdxs.reserve(descArgCount);
    for (size_t a = startArg; a < ins.args.size(); ++a) {
        uint16_t ai;
        if (!ctx.resolveOperand(ins.args[a], ai, reason)) return false;
        argIdxs.push_back(ai);
    }

    emitOpcode(ctx, VmOpcode::Call, prog.bytecodes);
    emitU16(prog.bytecodes, callSiteIdx);
    emitU16(prog.bytecodes, recv);
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, static_cast<uint8_t>(argIdxs.size()));
    for (uint16_t ai : argIdxs) emitU16(prog.bytecodes, ai);
    return true;
}

bool encodeTerminator(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason)
{
    // std::variant 访问：按 terminator 类型分派。
    return std::visit(
        [&](const auto& term) -> bool {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                return encodeJump(ctx, &block, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                return encodeBranch(ctx, &block, term, prog, reason);
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
                emitHaltOpcode(ctx, prog.bytecodes);
                (void)term;
                return true;
            }
            reason = "unknown terminator kind";
            return false;
        },
        block.terminator);
}

bool encodeJump(EncoderState& ctx, const RegBlock* srcBlock, const RegJumpTerminator& term,
                VmProgram& prog, std::string& reason)
{
    const auto it = ctx.blockIndex.find(term.target.block);
    if (it == ctx.blockIndex.end()) {
        reason = "Jump target block not indexed";
        return false;
    }
    // 边拷贝（块参数）走合成块：resolveEdgeTarget 优先返回合成块索引。
    const uint16_t target =
        ctx.resolveEdgeTarget(srcBlock, term.target.block, 0, it->second);
    emitOpcode(ctx, VmOpcode::Jump, prog.bytecodes);
    emitU16(prog.bytecodes, target);
    return true;
}

bool encodeBranch(EncoderState& ctx, const RegBlock* srcBlock, const RegBranchTerminator& term,
                  VmProgram& prog, std::string& reason)
{
    uint16_t cond;
    if (!ctx.resolveOperand(term.condition, cond, reason)) return false;
    const auto trueIt = ctx.blockIndex.find(term.trueTarget.block);
    const auto falseIt = ctx.blockIndex.find(term.falseTarget.block);
    if (trueIt == ctx.blockIndex.end() || falseIt == ctx.blockIndex.end()) {
        reason = "Branch target block not indexed";
        return false;
    }
    const uint16_t trueIdx  = ctx.resolveEdgeTarget(srcBlock, term.trueTarget.block, 0, trueIt->second);
    const uint16_t falseIdx = ctx.resolveEdgeTarget(srcBlock, term.falseTarget.block, 1, falseIt->second);
    emitOpcode(ctx, VmOpcode::Branch, prog.bytecodes);
    emitU16(prog.bytecodes, cond);
    emitU16(prog.bytecodes, trueIdx);
    emitU16(prog.bytecodes, falseIdx);
    return true;
}

bool encodeReturn(EncoderState& ctx, const RegReturnTerminator& term, VmProgram& prog,
                  std::string& reason)
{
    if (!term.hasValue) {
        emitOpcode(ctx, VmOpcode::Return, prog.bytecodes);
        emitU8(prog.bytecodes, 0);   // has_value = 0
        emitU16(prog.bytecodes, 0);  // src unused
        return true;
    }
    uint16_t src;
    if (!ctx.resolveOperand(term.value, src, reason)) return false;
    emitOpcode(ctx, VmOpcode::Return, prog.bytecodes);
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
    auto prog = std::make_unique<VmProgram>();
    EncoderState ctx(fn, instance);

    // 异常区域的隐式边（handler 参数）MVP 不支持：编码器既不编码 exception edges
    // 也不抛 Java 异常，含 try/catch 的方法整体 fail-closed。
    if (!fn.exceptionRegions.empty()) {
        reason = "exception regions not supported in MVP";
        return nullptr;
    }

    if (!indexBlocks(ctx, reason)) return nullptr;
    ctx.planSyntheticEdges();
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

    // Call 调用点的方法引用表（Phase 2.0）：直接搬移 EncoderState 收集的结果。
    prog->methodRefs = std::move(ctx.methodRefs);

    prog->numInputs = ctx.numInputs;
    prog->numRegs   = ctx.maxReg;
    return prog;
}

}   // namespace mj2x

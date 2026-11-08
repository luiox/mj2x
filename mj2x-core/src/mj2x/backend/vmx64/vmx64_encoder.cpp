#include "mj2x/backend/vmx64/vmx64_encoder.hpp"

#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/ssa/ssa_type.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mj2x {

namespace {

/// @brief 规划期站点：操作数已解析（字面量已物化为前置站点），分支目标待
///        块布局确定后解析（Block 目标或 NextSite = 本站点之后的下一站点）。
struct PlannedSite
{
    Vmx64Op op = Vmx64Op::Halt;
    uint16_t a  = 0;   ///< dst / a / src（按 op 布局取义）
    uint16_t b  = 0;   ///< src / b
    uint16_t c  = 0;   ///< b（算术第三操作数）
    uint32_t imm32 = 0;
    uint64_t imm64 = 0;

    /// 分支目标（Goto / IfIcmp）。
    struct Target
    {
        enum class Kind
        {
            None,
            Block,     ///< 指向某块首站点（块起始偏移）
            NextSite,  ///< 指向紧随本站点之后的站点（Branch 的 false 边链）
        };
        Kind            kind  = Kind::None;
        const RegBlock* block = nullptr;
    };
    Target targetA;   ///< IfIcmp true 目标 / Goto 目标
    Target targetB;   ///< IfIcmp false 目标
};

/// @brief i32 计算宽度判定（JVM 语义：boolean/byte/short/char/int 同宽）。
bool is32Type(SSAType t) { return ssaIsIntLike(t); }

/// @brief vslot 宽度判定：i64 = 64 位，int-like = 32 位，其余不支持。
bool slotWidthOf(SSAType t, bool& is64, std::string& reason, const char* what)
{
    if (t == SSAType::I64) {
        is64 = true;
        return true;
    }
    if (is32Type(t)) {
        is64 = false;
        return true;
    }
    reason = std::string(what) + ": unsupported operand type (int32/int64 only in Phase 1)";
    return false;
}

/// @brief 编码状态：站点规划 + 全函数寄存器读计数 + Compare 定义索引。
struct EncoderState
{
    std::vector<PlannedSite> sites;
    /// reg id → 全函数读取次数（不含定义本身）。
    std::unordered_map<uint32_t, uint32_t> reads;
    /// reg id → 定义它的 Compare 指令（Compare+Branch 融合用，SSA 单定义）。
    std::unordered_map<uint32_t, const RegInstruction*> compareDefs;
    uint32_t maxReg = 0;

    void noteReg(uint32_t id)
    {
        if (id > maxReg) maxReg = id;
    }

    void noteRead(const RegOperand& op)
    {
        if (op.kind == RegOperandKind::Register) {
            ++reads[op.reg.value];
            noteReg(op.reg.value);
        }
    }

    void noteReads(const RegInstruction& ins)
    {
        noteRead(ins.value);
        noteRead(ins.lhs);
        noteRead(ins.rhs);
        noteRead(ins.receiver);
        noteRead(ins.index);
        for (const auto& arg : ins.args) noteRead(arg);
    }
};

/// @brief 操作数解析：寄存器直接取 slot，IntLiteral 物化为前置 IConst/LConst
///        站点（写入共享暂存 slot），其余 fail-closed。
bool resolveOperand(const RegOperand& op, bool is64, uint16_t scratchSlot,
                    EncoderState& st, uint16_t& slotOut, std::string& reason)
{
    if (op.kind == RegOperandKind::Register) {
        if (op.reg.value > 0xFFFF) {
            reason = "register id exceeds u16 slot space";
            return false;
        }
        slotOut = static_cast<uint16_t>(op.reg.value);
        return true;
    }
    if (op.kind == RegOperandKind::IntLiteral) {
        PlannedSite mat;
        mat.a = scratchSlot;
        if (is64) {
            mat.op    = Vmx64Op::LConst;
            mat.imm64 = static_cast<uint64_t>(op.intValue);
        } else {
            mat.op    = Vmx64Op::IConst;
            mat.imm32 = static_cast<uint32_t>(static_cast<int32_t>(op.intValue));
        }
        st.sites.push_back(mat);
        slotOut = scratchSlot;
        return true;
    }
    reason = "operand kind unsupported (float/null/opaque literals are fail-closed in Phase 1)";
    return false;
}

/// @brief 边拷贝（phi 消除产物）→ 拷贝站点序列；字面量源直接物化进目标 slot。
bool planEdgeCopies(const std::vector<RegCopy>& copies, EncoderState& st, std::string& reason)
{
    for (const auto& cp : copies) {
        st.noteReg(cp.dest.id.value);
        if (cp.dest.id.value > 0xFFFF) {
            reason = "register id exceeds u16 slot space";
            return false;
        }
        const uint16_t dst = static_cast<uint16_t>(cp.dest.id.value);
        bool       is64 = false;
        if (!slotWidthOf(cp.dest.type, is64, reason, "edge copy")) return false;
        if (cp.source.kind == RegOperandKind::IntLiteral) {
            PlannedSite mat;
            mat.a = dst;
            if (is64) {
                mat.op    = Vmx64Op::LConst;
                mat.imm64 = static_cast<uint64_t>(cp.source.intValue);
            } else {
                mat.op    = Vmx64Op::IConst;
                mat.imm32 = static_cast<uint32_t>(static_cast<int32_t>(cp.source.intValue));
            }
            st.sites.push_back(mat);
            continue;
        }
        // 此处只剩寄存器源（字面量源已走上面的直接物化分支；浮点/null/opaque
        // 源 fail-closed，不做寄存器中转）。
        if (cp.source.kind != RegOperandKind::Register) {
            reason = "edge copy source unsupported (float/null/opaque literals are fail-closed)";
            return false;
        }
        if (cp.source.reg.value > 0xFFFF) {
            reason = "register id exceeds u16 slot space";
            return false;
        }
        const uint16_t src = static_cast<uint16_t>(cp.source.reg.value);
        PlannedSite cp2;
        cp2.a = dst;
        cp2.b = src;
        // 拷贝语义孪生 opcode（load/store 同 handler 语义）：按 (dst+src) 奇偶
        // 确定性二选一，扩大流中 opcode 覆盖面（纯编码期变换，语义零变化）。
        const bool useStore = (((dst + src) & 1u) != 0u);
        if (is64)
            cp2.op = useStore ? Vmx64Op::LStore : Vmx64Op::LLoad;
        else
            cp2.op = useStore ? Vmx64Op::IStore : Vmx64Op::ILoad;
        st.sites.push_back(cp2);
    }
    return true;
}

/// @brief Branch 条件是否可融合：条件寄存器由本块内 Compare 定义且全函数
///        读取次数恰为 1（该分支自身），双方操作数为 i32（寄存器/字面量）。
const RegInstruction* fusableCompare(const RegBlock& block, const RegBranchTerminator& br,
                                     const EncoderState& st)
{
    if (br.condition.kind != RegOperandKind::Register) return nullptr;
    const auto found = st.compareDefs.find(br.condition.reg.value);
    if (found == st.compareDefs.end() || found->second == nullptr) return nullptr;
    const RegInstruction* def = found->second;
    // 定义必须在本块（Compare 结果跨块消费 = 多读取，reads 检查已拦，此处
    // 再按块归属防御）。
    bool inBlock = false;
    for (const auto& ins : block.instructions) {
        if (&ins == def) {
            inBlock = true;
            break;
        }
    }
    if (!inBlock) return nullptr;
    // 唯一消费 = 该分支（reads 含分支条件本身）。
    const auto reads = st.reads.find(br.condition.reg.value);
    if (reads == st.reads.end() || reads->second != 1u) return nullptr;
    // i32 比较语义（if_icmp 族）：结果宽 32、双方操作数 int-like。
    if (!is32Type(def->dest.type)) return nullptr;
    const bool lhsOk = def->lhs.kind == RegOperandKind::Register
                           ? is32Type(def->lhs.type)
                           : def->lhs.kind == RegOperandKind::IntLiteral;
    const bool rhsOk = def->rhs.kind == RegOperandKind::Register
                           ? is32Type(def->rhs.type)
                           : def->rhs.kind == RegOperandKind::IntLiteral;
    if (!lhsOk || !rhsOk) return nullptr;
    return def;
}

Vmx64Op ifIcmpOpFor(SSAComparePredicate pred)
{
    switch (pred) {
    case SSAComparePredicate::Eq: return Vmx64Op::IfIcmpEq;
    case SSAComparePredicate::Ne: return Vmx64Op::IfIcmpNe;
    case SSAComparePredicate::Lt: return Vmx64Op::IfIcmpLt;
    case SSAComparePredicate::Le: return Vmx64Op::IfIcmpLe;
    case SSAComparePredicate::Gt: return Vmx64Op::IfIcmpGt;
    case SSAComparePredicate::Ge: return Vmx64Op::IfIcmpGe;
    }
    return Vmx64Op::IfIcmpEq;
}

const char* familyName(RegInstructionKind kind)
{
    switch (kind) {
    case RegInstructionKind::Copy: return "Copy";
    case RegInstructionKind::Unary: return "Unary";
    case RegInstructionKind::Binary: return "Binary";
    case RegInstructionKind::Compare: return "Compare";
    case RegInstructionKind::Convert: return "Convert";
    case RegInstructionKind::LoadField: return "LoadField";
    case RegInstructionKind::StoreField: return "StoreField";
    case RegInstructionKind::ArrayLoad: return "ArrayLoad";
    case RegInstructionKind::ArrayStore: return "ArrayStore";
    case RegInstructionKind::Call: return "Call";
    case RegInstructionKind::Allocate: return "Allocate";
    case RegInstructionKind::Intrinsic: return "Intrinsic";
    case RegInstructionKind::Barrier: return "Barrier";
    }
    return "?";
}

void appendU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32(std::vector<uint8_t>& out, uint32_t v)
{
    appendU16(out, static_cast<uint16_t>(v & 0xFFFF));
    appendU16(out, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

void appendU64(std::vector<uint8_t>& out, uint64_t v)
{
    appendU32(out, static_cast<uint32_t>(v & 0xFFFFFFFF));
    appendU32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFF));
}

}   // namespace

const char* vmx64OpHandlerName(Vmx64Op op)
{
    switch (op) {
    case Vmx64Op::IConst: return "iconst";
    case Vmx64Op::LConst: return "lconst";
    case Vmx64Op::ILoad: return "iload";
    case Vmx64Op::LLoad: return "lload";
    case Vmx64Op::IStore: return "istore";
    case Vmx64Op::LStore: return "lstore";
    case Vmx64Op::IAdd: return "iadd";
    case Vmx64Op::LAdd: return "ladd";
    case Vmx64Op::ISub: return "isub";
    case Vmx64Op::LSub: return "lsub";
    case Vmx64Op::IMul: return "imul";
    case Vmx64Op::LMul: return "lmul";
    case Vmx64Op::IfIcmpEq: return "icmpeq";
    case Vmx64Op::IfIcmpNe: return "icmpne";
    case Vmx64Op::IfIcmpLt: return "icmplt";
    case Vmx64Op::IfIcmpLe: return "icmple";
    case Vmx64Op::IfIcmpGt: return "icmpgt";
    case Vmx64Op::IfIcmpGe: return "icmpge";
    case Vmx64Op::Goto: return "goto";
    case Vmx64Op::IReturn: return "iret";
    case Vmx64Op::LReturn: return "lret";
    case Vmx64Op::VReturn: return "vret";
    case Vmx64Op::Halt: return "halt";
    case Vmx64Op::Count: break;
    }
    return "?";
}

const char* vmx64OpName(Vmx64Op op) { return vmx64OpHandlerName(op); }

uint32_t vmx64OpOperandSize(Vmx64Op op)
{
    switch (op) {
    case Vmx64Op::IConst: return 2 + 4;                     // dst + imm32
    case Vmx64Op::LConst: return 2 + 8;                     // dst + imm64
    case Vmx64Op::ILoad:
    case Vmx64Op::LLoad:
    case Vmx64Op::IStore:
    case Vmx64Op::LStore: return 2 + 2;                     // dst + src
    case Vmx64Op::IAdd:
    case Vmx64Op::LAdd:
    case Vmx64Op::ISub:
    case Vmx64Op::LSub:
    case Vmx64Op::IMul:
    case Vmx64Op::LMul: return 2 + 2 + 2;                   // dst + a + b
    case Vmx64Op::IfIcmpEq:
    case Vmx64Op::IfIcmpNe:
    case Vmx64Op::IfIcmpLt:
    case Vmx64Op::IfIcmpLe:
    case Vmx64Op::IfIcmpGt:
    case Vmx64Op::IfIcmpGe: return 2 + 2 + 4 + 4;           // a + b + true + false
    case Vmx64Op::Goto: return 4;                           // target
    case Vmx64Op::IReturn:
    case Vmx64Op::LReturn: return 2;                        // src
    case Vmx64Op::VReturn:
    case Vmx64Op::Halt: return 0;
    case Vmx64Op::Count: break;
    }
    return 0;
}

std::unique_ptr<Vmx64Program> Vmx64Encoder::encode(const RegFunction& fn, std::string& reason)
{
    // 异常区域不支持（Phase 1 无异常分派面）。
    if (!fn.exceptionRegions.empty()) {
        reason = "exception regions unsupported (fail-closed in Phase 1)";
        return nullptr;
    }

    EncoderState st;

    // --- 预扫描：参数类型/寄存器水位 + 全函数读取计数 + Compare 定义索引 ---
    for (const auto& param : fn.parameters) st.noteReg(param.id.value);
    for (const auto& blk : fn.blocks) {
        for (const auto& ins : blk->instructions) {
            if (ins.hasDest) {
                st.noteReg(ins.dest.id.value);
                if (ins.kind == RegInstructionKind::Compare)
                    st.compareDefs[ins.dest.id.value] = &ins;
            }
            st.noteReads(ins);
        }
        const auto& term = blk->terminator;
        std::visit(
            [&st](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    st.noteRead(t.condition);
                } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (t.hasValue) st.noteRead(t.value);
                } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    st.noteRead(t.value);
                } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    st.noteRead(t.value);
                }
            },
            term);
        // 边拷贝：读源写目的。
        const auto noteSucc = [&st](const RegSuccessor& succ) {
            for (const auto& cp : succ.copies) {
                st.noteReg(cp.dest.id.value);
                st.noteRead(cp.source);
            }
        };
        std::visit(
            [&noteSucc](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    noteSucc(t.target);
                } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    noteSucc(t.trueTarget);
                    noteSucc(t.falseTarget);
                } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& c : t.cases) noteSucc(c.target);
                    noteSucc(t.defaultTarget);
                }
            },
            term);
    }

    // 参数校验与落槽元数据。
    auto        program = std::make_unique<Vmx64Program>();
    const auto& params  = fn.parameters;
    for (const auto& param : params) {
        bool is64 = false;
        if (!slotWidthOf(param.type, is64, reason, "parameter")) return nullptr;
        if (param.id.value > 0xFFFF) {
            reason = "register id exceeds u16 slot space";
            return nullptr;
        }
        program->args.push_back({static_cast<uint16_t>(param.id.value), is64});
    }
    program->numInputs = static_cast<uint32_t>(params.size());
    program->numRegs   = st.maxReg + 1;
    if (program->numRegs < program->numInputs) program->numRegs = program->numInputs;
    program->numSlots = program->numRegs + 2;   // 2 个字面量暂存 slot
    const uint16_t scratchA = static_cast<uint16_t>(program->numRegs);
    const uint16_t scratchB = static_cast<uint16_t>(program->numRegs + 1);

    // --- 第一遍：规划站点序列（字面量物化 + Compare/Branch 融合） ---
    // 块首站点索引（站点表序号即站点索引 t；块起始流偏移在第二遍由站点大小推导）。
    std::unordered_map<const RegBlock*, size_t> blockFirstSite;
    bool retSeen = false;
    for (const auto& blk : fn.blocks) {
        blockFirstSite[blk.get()] = st.sites.size();
        const RegInstruction* fusedCompare = nullptr;
        const RegBranchTerminator* fusedBranch = nullptr;
        // 融合预判（Branch 条件是否吃掉本块一条 Compare）。
        const auto* asBranch = std::get_if<RegBranchTerminator>(&blk->terminator);
        if (asBranch != nullptr) {
            fusedCompare = fusableCompare(*blk, *asBranch, st);
            if (fusedCompare != nullptr) fusedBranch = asBranch;
        }
        for (const auto& ins : blk->instructions) {
            if (fusedCompare != nullptr && &ins == fusedCompare) continue;   // 融合吞并
            switch (ins.kind) {
            case RegInstructionKind::Copy: {
                if (!ins.hasDest) {
                    reason = "Copy without destination";
                    return nullptr;
                }
                bool is64 = false;
                if (!slotWidthOf(ins.dest.type, is64, reason, "copy")) return nullptr;
                if (ins.dest.id.value > 0xFFFF) {
                    reason = "register id exceeds u16 slot space";
                    return nullptr;
                }
                const uint16_t dst = static_cast<uint16_t>(ins.dest.id.value);
                if (ins.copy.source.kind == RegOperandKind::IntLiteral) {
                    PlannedSite mat;
                    mat.a = dst;
                    if (is64) {
                        mat.op    = Vmx64Op::LConst;
                        mat.imm64 = static_cast<uint64_t>(ins.copy.source.intValue);
                    } else {
                        mat.op    = Vmx64Op::IConst;
                        mat.imm32 = static_cast<uint32_t>(
                            static_cast<int32_t>(ins.copy.source.intValue));
                    }
                    st.sites.push_back(mat);
                    break;
                }
                uint16_t src = 0;
                if (!resolveOperand(ins.copy.source, is64, scratchA, st, src, reason))
                    return nullptr;
                PlannedSite cp;
                cp.a = dst;
                cp.b = src;
                const bool useStore = (((dst + src) & 1u) != 0u);
                if (is64)
                    cp.op = useStore ? Vmx64Op::LStore : Vmx64Op::LLoad;
                else
                    cp.op = useStore ? Vmx64Op::IStore : Vmx64Op::ILoad;
                st.sites.push_back(cp);
                break;
            }
            case RegInstructionKind::Binary: {
                bool is64 = false;
                if (!slotWidthOf(ins.dest.type, is64, reason, "binary")) return nullptr;
                Vmx64Op op = Vmx64Op::Halt;
                if (ins.binaryOp == SSABinaryOp::Add)
                    op = is64 ? Vmx64Op::LAdd : Vmx64Op::IAdd;
                else if (ins.binaryOp == SSABinaryOp::Sub)
                    op = is64 ? Vmx64Op::LSub : Vmx64Op::ISub;
                else if (ins.binaryOp == SSABinaryOp::Mul)
                    op = is64 ? Vmx64Op::LMul : Vmx64Op::IMul;
                else {
                    reason = "binary op unsupported (add/sub/mul only in Phase 1)";
                    return nullptr;
                }
                uint16_t dst = 0, a = 0, b = 0;
                if (ins.dest.id.value > 0xFFFF) {
                    reason = "register id exceeds u16 slot space";
                    return nullptr;
                }
                dst = static_cast<uint16_t>(ins.dest.id.value);
                if (!resolveOperand(ins.lhs, is64, scratchA, st, a, reason)) return nullptr;
                if (!resolveOperand(ins.rhs, is64, scratchB, st, b, reason)) return nullptr;
                PlannedSite site;
                site.op = op;
                site.a  = dst;
                site.b  = a;
                site.c  = b;
                st.sites.push_back(site);
                break;
            }
            case RegInstructionKind::Compare: {
                // 未被融合的独立 Compare：结果寄存器另有消费或类型不符。
                reason = "standalone Compare unsupported (only fused if_icmp branches in Phase 1)";
                return nullptr;
            }
            default:
                reason = std::string("instruction family unsupported: ") + familyName(ins.kind);
                return nullptr;
            }
        }

        // --- terminator ---
        const auto& term = blk->terminator;
        if (const auto* jump = std::get_if<RegJumpTerminator>(&term)) {
            if (!planEdgeCopies(jump->target.copies, st, reason)) return nullptr;
            PlannedSite site;
            site.op             = Vmx64Op::Goto;
            site.targetA.kind   = PlannedSite::Target::Kind::Block;
            site.targetA.block  = jump->target.block;
            st.sites.push_back(site);
        } else if (const auto* br = std::get_if<RegBranchTerminator>(&term)) {
            if (fusedBranch == nullptr || fusedCompare == nullptr) {
                reason = "Branch without fusable i32 Compare condition (fail-closed in Phase 1)";
                return nullptr;
            }
            // 退化分支：双后继同块且任一边有拷贝 → 拷贝互踩（单流无法分边执行）。
            if (br->trueTarget.block == br->falseTarget.block &&
                (!br->trueTarget.copies.empty() || !br->falseTarget.copies.empty())) {
                reason = "degenerate branch (both edges to same block with copies) unsupported";
                return nullptr;
            }
            uint16_t a = 0, b = 0;
            if (!resolveOperand(fusedCompare->lhs, false, scratchA, st, a, reason))
                return nullptr;
            if (!resolveOperand(fusedCompare->rhs, false, scratchB, st, b, reason))
                return nullptr;
            // true 边拷贝在分支前执行（写 true 后继的 phi 槽，false 路径随后
            // 覆写自己的 phi 槽，互不干扰——两后继 phi 寄存器不相交）。
            if (!planEdgeCopies(br->trueTarget.copies, st, reason)) return nullptr;
            PlannedSite site;
            site.op            = ifIcmpOpFor(fusedCompare->comparePredicate);
            site.a             = a;
            site.b             = b;
            site.targetA.kind  = PlannedSite::Target::Kind::Block;
            site.targetA.block = br->trueTarget.block;
            site.targetB.kind  = PlannedSite::Target::Kind::NextSite;   // false 边链入口
            st.sites.push_back(site);
            // false 边拷贝 + Goto falseBlock（分支站点的直落续链）。
            if (!planEdgeCopies(br->falseTarget.copies, st, reason)) return nullptr;
            PlannedSite gsite;
            gsite.op            = Vmx64Op::Goto;
            gsite.targetA.kind  = PlannedSite::Target::Kind::Block;
            gsite.targetA.block = br->falseTarget.block;
            st.sites.push_back(gsite);
        } else if (const auto* ret = std::get_if<RegReturnTerminator>(&term)) {
            if (!ret->hasValue) {
                if (retSeen && program->returnsValue) {
                    reason = "inconsistent return forms (void vs value)";
                    return nullptr;
                }
                program->returnsValue = false;
                program->retIs64      = false;
                retSeen               = true;
                PlannedSite site;
                site.op = Vmx64Op::VReturn;
                st.sites.push_back(site);
            } else {
                bool is64 = false;
                if (!slotWidthOf(ret->value.type, is64, reason, "return")) return nullptr;
                if (retSeen && (program->returnsValue != true || program->retIs64 != is64)) {
                    reason = "inconsistent return forms (mixed value widths)";
                    return nullptr;
                }
                program->returnsValue = true;
                program->retIs64      = is64;
                retSeen               = true;
                uint16_t src = 0;
                if (!resolveOperand(ret->value, is64, scratchA, st, src, reason)) return nullptr;
                PlannedSite site;
                site.op = is64 ? Vmx64Op::LReturn : Vmx64Op::IReturn;
                site.a  = src;
                st.sites.push_back(site);
            }
        } else {
            reason = "terminator unsupported (switch/throw/unreachable are fail-closed in Phase 1)";
            return nullptr;
        }
    }

    // 尾部防御性 Halt 哨兵（全路径 return 时不达；保证流非空）。
    {
        PlannedSite halt;
        halt.op = Vmx64Op::Halt;
        st.sites.push_back(halt);
    }

    if (st.sites.size() > 0xFFFF) {
        reason = "site count exceeds u16 handler-number space";
        return nullptr;
    }

    // --- 第二遍：物化字节流（站点索引 t 明文 + 解析后的块偏移目标） ---
    // 先按站点大小累计偏移，再解析 Block 目标为块首站点偏移。
    std::vector<uint32_t> siteOffset(st.sites.size(), 0);
    {
        uint32_t off = 0;
        for (size_t i = 0; i < st.sites.size(); ++i) {
            siteOffset[i] = off;
            off += 2u + vmx64OpOperandSize(st.sites[i].op);
        }
        if (off > 0xFFFFFFFFu) {
            reason = "stream size exceeds u32 offset space";
            return nullptr;
        }
    }
    auto blockStartOffset = [&](const RegBlock* blk, uint32_t& out) -> bool {
        if (blk == nullptr) {
            reason = "branch/jump target block is null";
            return false;
        }
        const auto found = blockFirstSite.find(blk);
        if (found == blockFirstSite.end()) {
            reason = "branch/jump target block is not part of the function";
            return false;
        }
        out = siteOffset[found->second];
        return true;
    };

    std::vector<uint8_t>& stream = program->stream;
    stream.clear();
    for (size_t i = 0; i < st.sites.size(); ++i) {
        const PlannedSite& site = st.sites[i];
        appendU16(stream, static_cast<uint16_t>(i));   // 明文 t（发射器加密改写）
        switch (site.op) {
        case Vmx64Op::IConst:
            appendU16(stream, site.a);
            appendU32(stream, site.imm32);
            break;
        case Vmx64Op::LConst:
            appendU16(stream, site.a);
            appendU64(stream, site.imm64);
            break;
        case Vmx64Op::ILoad:
        case Vmx64Op::LLoad:
        case Vmx64Op::IStore:
        case Vmx64Op::LStore:
            appendU16(stream, site.a);
            appendU16(stream, site.b);
            break;
        case Vmx64Op::IAdd:
        case Vmx64Op::LAdd:
        case Vmx64Op::ISub:
        case Vmx64Op::LSub:
        case Vmx64Op::IMul:
        case Vmx64Op::LMul:
            appendU16(stream, site.a);
            appendU16(stream, site.b);
            appendU16(stream, site.c);
            break;
        case Vmx64Op::IfIcmpEq:
        case Vmx64Op::IfIcmpNe:
        case Vmx64Op::IfIcmpLt:
        case Vmx64Op::IfIcmpLe:
        case Vmx64Op::IfIcmpGt:
        case Vmx64Op::IfIcmpGe: {
            appendU16(stream, site.a);
            appendU16(stream, site.b);
            uint32_t trueOff = 0;
            if (!blockStartOffset(site.targetA.block, trueOff)) return nullptr;
            appendU32(stream, trueOff);
            // NextSite：分支站点自身的下一站点（false 边拷贝链入口）。
            const uint32_t selfLen = 2u + vmx64OpOperandSize(site.op);
            appendU32(stream, siteOffset[i] + selfLen);
            break;
        }
        case Vmx64Op::Goto: {
            uint32_t targetOff = 0;
            if (!blockStartOffset(site.targetA.block, targetOff)) return nullptr;
            appendU32(stream, targetOff);
            break;
        }
        case Vmx64Op::IReturn:
        case Vmx64Op::LReturn:
            appendU16(stream, site.a);
            break;
        case Vmx64Op::VReturn:
        case Vmx64Op::Halt:
            break;
        case Vmx64Op::Count:
            reason = "internal: sentinel opcode leaked into stream";
            return nullptr;
        }
        program->sites.push_back({siteOffset[i], site.op});
    }

    return program;
}

}   // namespace mj2x

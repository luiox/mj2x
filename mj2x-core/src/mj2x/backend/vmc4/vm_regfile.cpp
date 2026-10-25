/// @file vm_regfile.cpp
/// @brief VMC4 寄存器文件定长化分配器实现（活性分析 + 线性扫描，见 vm_regfile.hpp）。

#include "mj2x/backend/vmc4/vm_regfile.hpp"

#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/register_ir/reg_instruction.hpp"
#include "mj2x/mir/register_ir/reg_value.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <vector>

namespace mj2x {

namespace {

// ---------------------------------------------------------------------------
// 位点扫描：把 RegFunction 摊平成线性位点流（定义/使用），供活性分析与区间构造。
//
// 位点编号 = fn.blocks 顺序（与块布局/乱序无关——分配只依赖 CFG 真活性，任何
// 线性化下「非重叠区间 ⇒ 不干扰」的包络论证都成立）。每块：指令位点连续编号，
// terminator 占一个位点；融合对（superOpPlan[i] == i+1）压缩为单条复合指令
// ——复合指令占被吸收指令的位点，源 = 两条指令全部源操作数，dest = 承载指令
// 的 dest，被吸收 dest 不再存在。
// ---------------------------------------------------------------------------

struct PosEvent
{
    std::vector<uint32_t> reads;   ///< 本位点读取的计算寄存器（参数已滤除）
    uint32_t              def = 0xFFFFFFFF;   ///< 本位点定义的寄存器（无定义哨兵）
};

struct BlockScan
{
    std::vector<PosEvent> instrs;   ///< 指令位点（融合对已压缩，与物理位点对齐）
    PosEvent              term;     ///< terminator 位点读取（含边拷贝源）
    uint32_t              startPos = 0;   ///< 首指令位点
    uint32_t              endPos   = 0;   ///< terminator 位点
};

void collectOperandRead(const RegOperand& op, uint32_t numInputs, std::vector<uint32_t>& out)
{
    if (op.kind == RegOperandKind::Register && op.reg.value >= numInputs)
        out.push_back(op.reg.value);
}

/// 一条指令读取的全部计算寄存器（不含 dest；镜像 countInstructionUses 的字段面）。
void collectInstructionReads(const RegInstruction& ins, uint32_t numInputs,
                             std::vector<uint32_t>& out)
{
    switch (ins.kind) {
    case RegInstructionKind::Copy: collectOperandRead(ins.copy.source, numInputs, out); break;
    case RegInstructionKind::Unary:
    case RegInstructionKind::Convert: collectOperandRead(ins.value, numInputs, out); break;
    case RegInstructionKind::Binary:
    case RegInstructionKind::Compare:
        collectOperandRead(ins.lhs, numInputs, out);
        collectOperandRead(ins.rhs, numInputs, out);
        break;
    case RegInstructionKind::Call:
        collectOperandRead(ins.receiver, numInputs, out);
        for (const auto& a : ins.args) collectOperandRead(a, numInputs, out);
        break;
    case RegInstructionKind::LoadField: collectOperandRead(ins.receiver, numInputs, out); break;
    case RegInstructionKind::StoreField:
        collectOperandRead(ins.receiver, numInputs, out);
        collectOperandRead(ins.value, numInputs, out);
        break;
    case RegInstructionKind::ArrayLoad:
        collectOperandRead(ins.receiver, numInputs, out);
        collectOperandRead(ins.index, numInputs, out);
        break;
    case RegInstructionKind::ArrayStore:
        collectOperandRead(ins.receiver, numInputs, out);
        collectOperandRead(ins.index, numInputs, out);
        collectOperandRead(ins.value, numInputs, out);
        break;
    case RegInstructionKind::Intrinsic:
        for (const auto& a : ins.args) collectOperandRead(a, numInputs, out);
        break;
    case RegInstructionKind::Barrier:
    case RegInstructionKind::Allocate: break;
    }
}

/// terminator 读取：分支条件 / 返回值 / switch 值 / throw 值 + 全部后继边拷贝源
/// （边拷贝在源块 terminator 位点取源）。
void collectTerminatorReads(const RegTerminator& term, uint32_t numInputs,
                            std::vector<uint32_t>& out)
{
    auto collectCopies = [&](const RegSuccessor& succ) {
        for (const auto& copy : succ.copies) collectOperandRead(copy.source, numInputs, out);
    };
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                collectCopies(t.target);
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                collectOperandRead(t.condition, numInputs, out);
                collectCopies(t.trueTarget);
                collectCopies(t.falseTarget);
            } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                collectOperandRead(t.value, numInputs, out);
                for (const auto& c : t.cases) collectCopies(c.target);
                collectCopies(t.defaultTarget);
            } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (t.hasValue) collectOperandRead(t.value, numInputs, out);
            } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                collectOperandRead(t.value, numInputs, out);
            } else {
                (void)t;   // Unreachable：无读取
            }
        },
        term);
}

/// 活跃区间：[birth, death] 是真活性位点的连续包络（安全过近似——liveIn/liveOut
/// 块跨度扩展保证跨块/乱序线性化下的活位点全部落入包络；非重叠包络 ⇒ 不干扰）。
struct Interval
{
    uint32_t id    = 0;
    uint32_t birth = 0;
    uint32_t death = 0;
};

}   // namespace

bool planVmRegfile(const RegFunction& fn, uint32_t regfileLimit, bool fragmentReserve,
                   const std::map<const RegBlock*, std::vector<int>>& superOpPlan,
                   VmRegfilePlan& out, std::string& reason)
{
    if (regfileLimit == 0 || regfileLimit > 0xFFFF) {
        reason = "regfile limit out of range (1..65535): " + std::to_string(regfileLimit);
        return false;
    }
    out          = VmRegfilePlan{};
    out.numInputs = static_cast<uint16_t>(fn.parameters.size());
    // 溢出槽区从 limit 起：limit < numInputs 时溢出槽会与参数区 [0, numInputs)
    // 重叠（参数是入口现场，被溢出槽覆写即错值）——fail-closed，不做静默搬移。
    if (regfileLimit < out.numInputs) {
        reason = "regfile limit below parameter count: limit=" + std::to_string(regfileLimit)
                 + " numInputs=" + std::to_string(out.numInputs)
                 + " (spill region would overlap the parameter area)";
        return false;
    }
    out.poolTop  = static_cast<uint16_t>(regfileLimit);
    if (fragmentReserve) {
        // fragment 临时槽占池顶 3 槽：不得落入参数区（参数恒等映射不可覆写）。
        if (regfileLimit < 3 ||
            static_cast<uint32_t>(regfileLimit) - 3 < out.numInputs) {
            reason = "regfile limit too small for fragment temp reservation: limit="
                     + std::to_string(regfileLimit) + " numInputs=" + std::to_string(out.numInputs)
                     + " (needs limit >= numInputs + 3)";
            return false;
        }
        out.poolTop = static_cast<uint16_t>(regfileLimit - 3);
    }
    out.spillBase = static_cast<uint16_t>(regfileLimit);

    // --- 位点扫描 ---
    std::unordered_map<const RegBlock*, size_t> blockIndexOf;
    blockIndexOf.reserve(fn.blocks.size());
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi)
        blockIndexOf[fn.blocks[bi].get()] = bi;

    std::vector<BlockScan> scans(fn.blocks.size());
    std::set<uint32_t>     foldedIds;   // 融合吸收的 dest：不参与分配（不再存在）
    uint32_t               pos = 0;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const RegBlock& block = *fn.blocks[bi];
        auto&           scan  = scans[bi];
        scan.startPos         = pos;
        const auto planIt     = superOpPlan.find(&block);
        const std::vector<int>* plan = (planIt == superOpPlan.end()) ? nullptr : &planIt->second;
        const size_t n       = block.instructions.size();
        scan.instrs.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const RegInstruction& ins = block.instructions[i];
            PosEvent              ev;
            if (plan != nullptr && (*plan)[i] != -1) {
                // 融合对首条：复合指令位点。源 = 首条双操作数 + 承载指令第三源；
                // dest = 承载指令 dest；被吸收 dest 记入 foldedIds。
                const RegInstruction& second = block.instructions[i + 1];
                collectInstructionReads(ins, out.numInputs, ev.reads);
                collectOperandRead(second.rhs, out.numInputs, ev.reads);
                if (second.hasDest) ev.def = second.dest.id.value;
                foldedIds.insert(ins.dest.id.value);
                ++i;   // 承载指令不占独立位点（融合产物已在本位点发射）
            } else {
                collectInstructionReads(ins, out.numInputs, ev.reads);
                if (ins.hasDest) ev.def = ins.dest.id.value;
            }
            scan.instrs.push_back(std::move(ev));
            ++pos;
        }
        scan.endPos = pos;
        collectTerminatorReads(block.terminator, out.numInputs, scan.term.reads);
        ++pos;
    }

    // 入口定义：后继边拷贝 dest（块参数）在**源块 terminator 位点**诞生（边拷贝
    // 在源块结尾、目标块开头之间执行），而非目标块入口——birth 取源块 endPos，
    // 否则目标块复用槽位的值会被边拷贝覆盖（java_corpus RfMin 首曝的第二半）。
    // 异常区域拷贝 dest 与 exceptionValue 同为 handler 块入口诞生（分派点在
    // 保护块内，保守取 handler 起点即可——无跨块覆盖面）。无使用的入口定义
    // 同样入表（编码器仍会发射这些写入位点，寄存器映射必须全）。
    struct EntryDef
    {
        size_t   dst = 0;     ///< 目标块下标
        uint32_t id = 0;
        size_t   srcIdx = 0;  ///< 源块下标（birth = 源块 endPos）
    };
    std::vector<EntryDef> entryDefList;
    std::vector<std::set<uint32_t>> entryDefs(fn.blocks.size());
    auto noteEntryDef = [&](size_t srcIdx, const RegBlock* dst, uint32_t id) {
        if (id < out.numInputs) return;
        auto it = blockIndexOf.find(dst);
        if (it == blockIndexOf.end()) return;
        entryDefs[it->second].insert(id);
        entryDefList.push_back({it->second, id, static_cast<uint32_t>(srcIdx)});
    };
    for (size_t srcIdx = 0; srcIdx < fn.blocks.size(); ++srcIdx) {
        const RegBlock& srcBlock = *fn.blocks[srcIdx];
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    for (const auto& copy : t.target.copies)
                        noteEntryDef(srcIdx, t.target.block, copy.dest.id.value);
                } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    for (const auto& copy : t.trueTarget.copies)
                        noteEntryDef(srcIdx, t.trueTarget.block, copy.dest.id.value);
                    for (const auto& copy : t.falseTarget.copies)
                        noteEntryDef(srcIdx, t.falseTarget.block, copy.dest.id.value);
                } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& c : t.cases)
                        for (const auto& copy : c.target.copies)
                            noteEntryDef(srcIdx, c.target.block, copy.dest.id.value);
                    for (const auto& copy : t.defaultTarget.copies)
                        noteEntryDef(srcIdx, t.defaultTarget.block, copy.dest.id.value);
                } else {
                    (void)t;   // Return/Throw/Unreachable：无后继拷贝
                }
            },
            srcBlock.terminator);
    }
    for (const auto& region : fn.exceptionRegions) {
        if (region.handler != nullptr && region.exceptionValue.id.value >= out.numInputs)
            noteEntryDef(blockIndexOf.at(region.handler), region.handler,
                         region.exceptionValue.id.value);
        for (const auto& edge : region.edges)
            for (const auto& copy : edge.copies)
                noteEntryDef(blockIndexOf.at(region.handler), region.handler,
                             copy.dest.id.value);
    }

    // --- 块级活性分析（backward 不动点）---
    const size_t blockCount = fn.blocks.size();
    std::vector<std::set<uint32_t>> useGen(blockCount), defAll(blockCount);
    std::vector<std::set<uint32_t>> liveIn(blockCount), liveOut(blockCount);
    std::vector<std::vector<size_t>> succs(blockCount);
    for (size_t bi = 0; bi < blockCount; ++bi) {
        const RegBlock& block = *fn.blocks[bi];
        auto&           scan  = scans[bi];
        // useGen（向上暴露使用）：块内倒序走，def 杀、read 生。
        std::set<uint32_t> live;
        for (const uint32_t r : scan.term.reads) live.insert(r);
        for (auto it = scan.instrs.rbegin(); it != scan.instrs.rend(); ++it) {
            if (it->def != 0xFFFFFFFF) live.erase(it->def);
            for (const uint32_t r : it->reads) live.insert(r);
        }
        useGen[bi] = std::move(live);
        // defAll：指令定义集（入口定义不算块内定义——块参数 born at entry）。
        for (const auto& ev : scan.instrs)
            if (ev.def != 0xFFFFFFFF) defAll[bi].insert(ev.def);
        // 后继（真实块；合成边块只是拷贝的载体，CFG 目标仍是真实块）。
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    succs[bi].push_back(blockIndexOf.at(t.target.block));
                } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    succs[bi].push_back(blockIndexOf.at(t.trueTarget.block));
                    succs[bi].push_back(blockIndexOf.at(t.falseTarget.block));
                } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& c : t.cases) succs[bi].push_back(blockIndexOf.at(c.target.block));
                    succs[bi].push_back(blockIndexOf.at(t.defaultTarget.block));
                } else {
                    (void)t;
                }
            },
            block.terminator);
    }
    const bool dbgBlocks = std::getenv("MJ2X_REGFILE_DEBUG") != nullptr;
    if (dbgBlocks) {
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const RegBlock& blk = *fn.blocks[bi];
            std::fprintf(stderr, "[regfile-ir] b=%zu term-reads={", bi);
            for (uint32_t id : scans[bi].term.reads) std::fprintf(stderr, "%u,", id);
            std::fprintf(stderr, "} succ={");
            for (size_t s2 : succs[bi]) std::fprintf(stderr, "%zu,", s2);
            std::fprintf(stderr, "}\n");
            for (size_t ii = 0; ii < blk.instructions.size(); ++ii) {
                const RegInstruction& ins = blk.instructions[ii];
                std::fprintf(stderr, "[regfile-ir]   kind=%d dest=%d%s reads={", (int)ins.kind,
                             ins.hasDest ? (int)ins.dest.id.value : -1,
                             ins.hasDest ? "" : " (none)");
                std::vector<uint32_t> rd;
                collectInstructionReads(ins, out.numInputs, rd);
                for (uint32_t id : rd) std::fprintf(stderr, "%u,", id);
                std::fprintf(stderr, "}\n");
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t bi = 0; bi < blockCount; ++bi) {
            std::set<uint32_t> outSet;
            for (const size_t s : succs[bi]) {
                // 后继的块参数（边拷贝 dest）在**边上**诞生（边拷贝写入），
                // 不应经 liveOut 回流进前驱——否则参数的幻影活性回灌全方法
                // （birth=0 / 死参数区间 [0,0] 却在方法末尾的边拷贝处写槽，
                // 覆盖已分配给其他活值的槽——java_corpus RfMin 首曝）。
                for (const uint32_t id : liveIn[s])
                    if (!entryDefs[s].count(id)) outSet.insert(id);
            }
            std::set<uint32_t> inSet = useGen[bi];
            for (const uint32_t id : outSet)
                if (!defAll[bi].count(id)) inSet.insert(id);
            if (outSet != liveOut[bi]) {
                liveOut[bi] = std::move(outSet);
                changed     = true;
            }
            if (inSet != liveIn[bi]) {
                liveIn[bi] = std::move(inSet);
                changed    = true;
            }
        }
    }

    if (dbgBlocks) {
        for (size_t bi = 0; bi < blockCount; ++bi) {
            std::fprintf(stderr, "[regfile-succ] b=%zu succ={", bi);
            for (size_t s2 : succs[bi]) std::fprintf(stderr, "%zu,", s2);
            std::fprintf(stderr, "}\n");
        }
        for (size_t bi = 0; bi < blockCount; ++bi) {
            std::fprintf(stderr, "[regfile-block] b=%zu pos=[%u..%u] useGen={", bi,
                         scans[bi].startPos, scans[bi].endPos);
            for (uint32_t id : useGen[bi]) std::fprintf(stderr, "%u,", id);
            std::fprintf(stderr, "} defAll={");
            for (uint32_t id : defAll[bi]) std::fprintf(stderr, "%u,", id);
            std::fprintf(stderr, "} liveIn={");
            for (uint32_t id : liveIn[bi]) std::fprintf(stderr, "%u,", id);
            std::fprintf(stderr, "} liveOut={");
            for (uint32_t id : liveOut[bi]) std::fprintf(stderr, "%u,", id);
            std::fprintf(stderr, "}\n");
        }
    }

    // --- 活跃区间（连续包络）---
    std::map<uint32_t, uint32_t> defPos, lastUse;
    for (size_t bi = 0; bi < blockCount; ++bi) {
        const auto& scan = scans[bi];
        for (size_t pi = 0; pi < scan.instrs.size(); ++pi) {
            const auto& ev = scan.instrs[pi];
            const uint32_t p = scan.startPos + static_cast<uint32_t>(pi);
            if (ev.def != 0xFFFFFFFF) defPos[ev.def] = p;
            for (const uint32_t r : ev.reads) lastUse[r] = std::max(lastUse[r], p);
        }
        for (const uint32_t r : scan.term.reads) lastUse[r] = std::max(lastUse[r], scan.endPos);
    }
    std::map<uint32_t, uint32_t> birth, death;
    auto noteBirth = [&](uint32_t id, uint32_t p) {
        auto it = birth.find(id);
        if (it == birth.end() || p < it->second) birth[id] = p;
    };
    auto noteDeath = [&](uint32_t id, uint32_t p) {
        auto it = death.find(id);
        if (it == death.end() || p > it->second) death[id] = p;
    };
    for (const auto& kv : defPos) {
        noteBirth(kv.first, kv.second);
        noteDeath(kv.first, kv.second);   // 零使用 dest（副作用指令）也要有退化区间
    }
    for (const auto& kv : lastUse) noteDeath(kv.first, kv.second);
    // 入口定义（块参数/exceptionValue）：即使无使用也要有退化区间（编码器发射写入）。
    // birth = 源块 terminator 位点（EntryDef.srcIdx 的 endPos）——边拷贝在源块
    // 结尾执行，参数槽位的占用从写入时刻起算；若取目标块起点，目标块复用该槽
    // 位的值会被边拷贝覆盖（java_corpus RfMin 首曝的第二半）。多入口取 min。
    // death = 穿透整个目标块：同一入口点的全部参数在同一程序点被（合成边拷贝
    // 序列/异常分派）逐个写入——死参数（如 catch 中未使用的 i）与后续才读的
    // 参数若按各自真实使用区间分配会共享槽位，第二个写覆盖第一个的值
    // （java_corpus Mini 首曝：regs[1]=regs[2]; regs[1]=regs[9]）。参数区间
    // live-through 目标块保证同块参数两两重叠 → 槽位互斥。
    for (const auto& ed : entryDefList) {
        noteBirth(ed.id, scans[ed.srcIdx].endPos);
        noteDeath(ed.id, scans[ed.dst].endPos);
    }
    // liveIn/liveOut 块跨度扩展：包络覆盖块内全部活位点（含乱序线性化与跨块携带）。
    for (size_t bi = 0; bi < blockCount; ++bi) {
        for (const uint32_t id : liveIn[bi]) noteBirth(id, scans[bi].startPos);
        for (const uint32_t id : liveOut[bi]) noteDeath(id, scans[bi].endPos);
    }

    std::vector<Interval> intervals;
    intervals.reserve(birth.size());
    const bool dbgIntervals = std::getenv("MJ2X_REGFILE_DEBUG") != nullptr;
    if (dbgIntervals) {
        for (const auto& kv : birth)
            std::fprintf(stderr, "[regfile-interval] id=%u birth=%u death=%u\n", kv.first,
                         kv.second,
                         death.count(kv.first) ? death.at(kv.first) : kv.second);
    }
    for (const auto& kv : birth) {
        if (foldedIds.count(kv.first)) continue;   // 融合吸收的 dest 不分配
        Interval iv;
        iv.id    = kv.first;
        iv.birth = kv.second;
        auto dit = death.find(kv.first);
        iv.death = (dit == death.end()) ? kv.second : std::max(kv.second, dit->second);
        intervals.push_back(iv);
    }
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) {
        if (a.birth != b.birth) return a.birth < b.birth;
        if (a.death != b.death) return a.death < b.death;
        return a.id < b.id;
    });

    // --- Poletto 线性扫描 ---
    struct Active
    {
        uint32_t id    = 0;
        uint32_t death = 0;
        uint32_t phys  = 0;
    };
    std::vector<Active> active;
    std::set<uint32_t>  freePool;
    for (uint32_t r = out.numInputs; r < out.poolTop; ++r) freePool.insert(r);
    uint32_t spillNext = out.spillBase;
    for (const Interval& iv : intervals) {
        for (auto it = active.begin(); it != active.end();) {
            if (it->death < iv.birth) {
                freePool.insert(it->phys);
                it = active.erase(it);
            } else {
                ++it;
            }
        }
        if (!freePool.empty()) {
            Active a;
            a.id    = iv.id;
            a.death = iv.death;
            a.phys  = *freePool.begin();
            freePool.erase(freePool.begin());
            out.vmRegOf[iv.id] = a.phys;
            active.push_back(a);
            continue;
        }
        // 池耗尽：溢出「结束点最远」的区间（active ∪ 当前）。被溢出者让出池寄存器，
        // 换取稳定溢出槽；当前区间最远时直接溢出当前。
        Active*  victim      = nullptr;
        uint32_t victimDeath = iv.death;
        for (auto& a : active) {
            if (a.death > victimDeath) {
                victimDeath = a.death;
                victim      = &a;
            }
        }
        if (victim == nullptr) {
            out.vmRegOf[iv.id] = spillNext++;
        } else {
            out.vmRegOf[victim->id] = spillNext++;
            out.vmRegOf[iv.id]      = victim->phys;
            victim->id              = iv.id;
            victim->death           = iv.death;
        }
    }
    out.spillCount = spillNext - out.spillBase;
    out.highWater  = spillNext;
    if (std::getenv("MJ2X_REGFILE_DEBUG") != nullptr) {
        std::fprintf(stderr, "[regfile] numInputs=%u poolTop=%u spillBase=%u spillCount=%u\n",
                     out.numInputs, out.poolTop, out.spillBase, out.spillCount);
        for (const auto& kv : out.vmRegOf)
            std::fprintf(stderr, "[regfile] id=%u -> slot=%u\n", kv.first, kv.second);
    }
    return true;
}

}   // namespace mj2x

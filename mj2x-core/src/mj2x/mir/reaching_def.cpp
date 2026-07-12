#include "reaching_def.hpp"

#include <queue>
#include <unordered_set>

namespace mj2x {

namespace {

// 计算块 B 的 gen[B] / kill[B]。
// gen[B]：每个 slot 在 B 内"最后一次"写的 def。
// kill[B]：B 内所有被覆盖的 slot（即 B 内某 slot 被写 → 杀死 IN 来的同 slot def）。
// 注意 reaching-def 的标准定义里 kill 是"其它所有对同 slot 的 def"，但因为我们用
// per-slot 集合 + 集合差，实际只需知道"B 是否写了 slot N"（写了就清空 IN 的该 slot，再放 gen）。
struct GenKill
{
    std::unordered_map<int, DefId> gen;   // slot -> 块内最后写它的 def
    std::unordered_set<int> killedSlots;   // B 内被写过的 slot（这些 slot 的 IN 全被杀）
};

GenKill computeGenKill(const ReachingDefBlock& b)
{
    GenKill gk;
    for (const auto& d : b.localDefs) {
        gk.gen[d.slot] = DefId{b.labelId, d.nodeIndex, d.slot};
        gk.killedSlots.insert(d.slot);
        // 多次写同 slot：后面覆盖前面，gen 自动更新到最后一次。
    }
    return gk;
}

}   // namespace

ReachingDefResult computeReachingDef(const std::vector<ReachingDefBlock>& blocks)
{
    ReachingDefResult result;

    if (blocks.empty()) return result;

    // label -> GenKill
    std::unordered_map<int, GenKill>                 gk;
    std::unordered_map<int, const ReachingDefBlock*> byLabel;
    for (const auto& b : blocks) {
        gk[b.labelId]      = computeGenKill(b);
        byLabel[b.labelId] = &b;
    }

    // 初始化 OUT[B] = gen[B]（标准 reaching-def 初值：OUT = gen）。
    for (const auto& b : blocks) {
        const auto& g = gk[b.labelId].gen;
        for (const auto& [slot, defId] : g) {
            result.out[b.labelId].defsFor(slot).insert(defId);
        }
    }

    // 不动点迭代：worklist。
    // 变化检测：用 block label 集合，每次 OUT 变化就把后继入队。
    std::queue<int>         worklist;
    std::unordered_set<int> inQueue;
    auto                    enqueue = [&](int label) {
        if (inQueue.insert(label).second) worklist.push(label);
    };
    for (const auto& b : blocks) enqueue(b.labelId);

    int       guard       = 0;   // 防御性上限（理论必收敛，设个保险）
    const int GUARD_LIMIT = 100000;

    while (!worklist.empty() && guard++ < GUARD_LIMIT) {
        int label = worklist.front();
        worklist.pop();
        inQueue.erase(label);

        const auto& b    = *byLabel.at(label);
        const auto& gk_b = gk.at(label);

        // IN[B] = ∪ OUT[P]
        SlotDefMap newIn;
        for (int pred : b.predecessorLabels) {
            auto it = result.out.find(pred);
            if (it == result.out.end()) continue;
            for (const auto& [slot, defs] : it->second.bySlot) {
                DefSet& target = newIn.defsFor(slot);
                for (const auto& d : defs) target.insert(d);
            }
        }

        // OUT[B] = gen[B] ∪ (IN[B] − kill[B])
        SlotDefMap newOut;
        // 1) 保留 IN 里没被 kill 的 slot
        for (const auto& [slot, defs] : newIn.bySlot) {
            if (gk_b.killedSlots.count(slot)) continue;   // 杀掉
            DefSet& target = newOut.defsFor(slot);
            for (const auto& d : defs) target.insert(d);
        }
        // 2) 加上 gen
        for (const auto& [slot, defId] : gk_b.gen) {
            newOut.defsFor(slot).insert(defId);
        }

        // 变化检测：比较 newOut 与旧 out[label]。集合单调增长，size 增即变化。
        auto& oldOut  = result.out[label];
        bool  changed = false;
        for (const auto& [slot, defs] : newOut.bySlot) {
            auto& oldSlot = oldOut.defsFor(slot);
            for (const auto& d : defs) {
                if (oldSlot.insert(d).second) changed = true;   // 新插入 = 变化
            }
        }

        result.in[label] = std::move(newIn);

        if (changed) {
            // OUT 变了，后继重新算。successors 我们没存，但 reaching-def 的传播只需：
            // 任何把 B 当作前驱的块都要重算。这里用反向：遍历所有块，前驱含 label 的入队。
            for (const auto& other : blocks) {
                for (int pred : other.predecessorLabels) {
                    if (pred == label) {
                        enqueue(other.labelId);
                        break;
                    }
                }
            }
        }
    }

    return result;
}

}   // namespace mj2x

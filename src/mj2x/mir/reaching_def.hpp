// mj2x reaching-def 数据流（自写，零 libjbytecode analysis 依赖）。
//
// 为什么要自写：R0.5 实测 libjbytecode Analyzer<SourceValue> 有 bug（maxLocuns 算错 +
// 不追踪 local 的 reaching-def，见 dev_plan R1-B、analyzer_probe_test.cpp）。本 pass 在
// L1 CodeConverter 的 CFG（frontend/block.hpp，已 12 测试验证）上跑经典 reaching-def，
// 完全自有代码、零外部依赖风险。
//
// 算法详见 doc/SSA_ALGORITHM.md §3。
//
// 简化：R1 只跟踪 local slot（跨块 phi 都在 local 上；stack 跨块合并 R1 fail-closed）。
// 这覆盖 sumTo/gcd 这类循环 phi 场景（= R1 验收样本）。
#pragma once

#include "mj2x/frontend/block.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

// 一个 def 的全局唯一标识 = (所在 L1 Block 的 label id, 块内节点序号)。
// 用节点序号而非 MIRNode*，便于跨块稳定引用 + dump。
struct DefId {
    int blockLabel = 0;     // L1 Block 的 label id
    int nodeIndex = 0;      // 块内 nodes[] 下标
    int slot = 0;           // 被写的 local slot（MIRLocalStore/Iinc 的 varIndex）

    bool operator==(const DefId& o) const {
        return blockLabel == o.blockLabel && nodeIndex == o.nodeIndex && slot == o.slot;
    }
};

struct DefIdHash {
    size_t operator()(const DefId& d) const noexcept {
        // 简单组合哈希。
        return std::hash<int>()(d.blockLabel) ^ (std::hash<int>()(d.nodeIndex) << 1) ^
               (std::hash<int>()(d.slot) << 2);
    }
};

using DefSet = std::unordered_set<DefId, DefIdHash>;

// 一个块入口/出口处，每个 local slot 的 reaching def 集合。
// slot -> {def...}。集合大小 ≥2 表示该 slot 在此处需要 φ（block arg）。
struct SlotDefMap {
    std::unordered_map<int, DefSet> bySlot;

    const DefSet* defsFor(int slot) const {
        auto it = bySlot.find(slot);
        return it != bySlot.end() ? &it->second : nullptr;
    }
    DefSet& defsFor(int slot) { return bySlot[slot]; }
};

// L1 Block 的 CFG 视图（builder 提供，避免 reaching_def 直接依赖 Block 内部细节）。
// reaching_def 只需要：块 id、前驱、块内"哪些节点写了哪个 slot"。
struct ReachingDefBlock {
    int labelId = 0;
    std::vector<int> predecessorLabels;          // 前驱块的 label id
    // 块内 def：每个 (nodeIndex, slot)。按出现顺序。最后一个 slot N 的写 = gen。
    struct LocalDef { int nodeIndex; int slot; };
    std::vector<LocalDef> localDefs;
};

struct ReachingDefResult {
    // IN[B] / OUT[B]，按 block label id 索引。
    std::unordered_map<int, SlotDefMap> in;
    std::unordered_map<int, SlotDefMap> out;

    // B 入口处 slot 的 def 数（便利）。≥2 = 需要 block arg（φ）。
    int inDefCount(int blockLabel, int slot) const {
        auto it = in.find(blockLabel);
        if (it == in.end()) return 0;
        const DefSet* ds = it->second.defsFor(slot);
        return ds ? static_cast<int>(ds->size()) : 0;
    }
};

// 跑 reaching-def 数据流。blocks 是 L1 CFG 视图（builder 从 CodeConverter 的 Block[] 构造）。
// 算法：IN[B]=∪OUT[P]，OUT[B]=gen[B]∪(IN[B]−kill[B])，不动点迭代。
ReachingDefResult computeReachingDef(const std::vector<ReachingDefBlock>& blocks);

} // namespace mj2x

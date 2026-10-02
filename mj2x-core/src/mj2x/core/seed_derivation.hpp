// seed_derivation.hpp - per-client 唯一派生层（竞品借鉴低成本项，#759/#760）。
//
// 竞品（JavaShroud per-method profile salt / JShield per-client 产物）都落地了
// 「同一份源码/seed 下，不同客户端拿到不同产物」。本模块提供统一派生入口：
//   deriveClientSeed(baseSeed, clientId)
// - clientId == 0：恒等返回 baseSeed —— 默认产物零变化（不变量：不带 --client-id
//   的构建与历史产物逐字节一致，seed 可复现契约不变）；
// - clientId != 0：splitmix64(baseSeed ^ clientId) 雪崩混合 —— 每个客户端得到
//   唯一 seed 流，进而得到唯一 ISA/变体/state 码产物；同 clientId + 同 baseSeed
//   依然确定（可复现构建）。
//
// 派生结果喂给所有 seed 驱动点（RegC transformSeed / VMC4 seed 等），
// 单点改造成本最低，产物唯一性由下游 seed 机制自然继承。

#pragma once

#include <cstdint>

#include "libca/random/split_mix64.hpp"

namespace mj2x {

/// per-client 派生：clientId == 0 恒等；否则 splitmix64(baseSeed ^ clientId)。
/// splitmix64 单源 = ca::random::SplitMix64（libca 0.0.10 canonical，#1027 A3
/// 收敛；一次 next() = 黄金比例增量后终混合，与原本地实现逐位一致）。
inline std::uint64_t deriveClientSeed(std::uint64_t baseSeed, std::uint64_t clientId)
{
    if (clientId == 0) return baseSeed;
    return ca::random::SplitMix64(baseSeed ^ clientId).next();
}

}   // namespace mj2x

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

namespace mj2x {

namespace detail {

/// splitmix64：确定性雪崩混合（与 reg_transform_passes 内部实现一致的算法族）。
inline std::uint64_t splitmix64Step(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}   // namespace detail

/// per-client 派生：clientId == 0 恒等；否则 splitmix64(baseSeed ^ clientId)。
inline std::uint64_t deriveClientSeed(std::uint64_t baseSeed, std::uint64_t clientId)
{
    if (clientId == 0) return baseSeed;
    return detail::splitmix64Step(baseSeed ^ clientId);
}

}   // namespace mj2x

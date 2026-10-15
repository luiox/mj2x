/// @file const_key_derivation.hpp
/// @brief 立即数隐藏的 per-site 密钥派生（C++ 侧单源；RT 双端 C 实现为其镜像）。
///
/// @details
/// `applyConstantHiding` 把整型立即数 C 改写为
///
///     k = Intrinsic("const.key:<site>")   // 运行时派生，独立 TU 不透明调用
///     e = Xor(E64, k)                     // E64 = C64 ^ K，编译期算
///     c = Convert(e)                      // 仅 I32：截断回 32 位
///
/// 编译期算 E 的 K 与运行时派生 K 必须逐位相同，否则 `E ^ K != C` 静默改语义。
/// 因此常量（不透明常量 + splitmix64 四步混合）在两侧各存一份：
///
/// - **C++ 侧**：本文件是唯一权威，pass 与单测共用（同 TU 集，改动必然同步）。
/// - **C 侧（可分发运行时）**：`mj2x-rt/src/regc/mj2x_regc_crt.c` 与
///   `mj2x-rt/src/llvmir/mj2x_llvmir_runtime.c` 各存一份。运行时随生成工程
///   分发、必须自洽（不能依赖 mj2x-core 头文件），故无法与 C++ 侧共用。
///
/// 跨语言一致性由 E2E（真实编译 + 运行生成工程）守；本文件保证 C++ 侧内部一致。
///
/// ## 为何掺不透明常量
///
/// 密钥若写成可内联的纯函数，开启 LTO 后 `E ^ const_key(site)` 会被 IPSCCP /
/// 常量传播折回 C，整个立即数隐藏失效。C 侧因此用 `volatile` 读承载该常量
/// （编译器必须每次真实加载）。C++ 侧只需同一个数值。
#pragma once

#include <cstdint>

#include "libca/random/split_mix64.hpp"

namespace mj2x {

/// @brief per-site 不透明常量（RT 双端 `g_const_key_opaque` 的镜像值）。
///
/// 任意非零常量即可：作用只是让 C 侧经 volatile 读参与运算（不可折叠），
/// 并不提供密码学强度（见头文件顶部说明）。改动必须与 C 侧同步。
constexpr uint64_t kConstKeyOpaque = 0x517CC1B727220A95ull;

/// @brief 立即数隐藏 intrinsic 名前缀（`const.key:<site>`）。
///
/// 发射端按前缀分发、后缀十进制 site 作为参数传给运行时 helper。
/// 与 `indy:` 的站点命名纪律一致（按名过滤，fail-closed）。
constexpr const char* kConstKeyIntrinsicPrefix = "const.key:";

/// @brief 派生 site 对应的密钥：splitmix64(site ^ kConstKeyOpaque)。
///
/// 与 `mj2x_regc_const_key` / `mj2x_llvm_const_key` 逐位同源。
inline uint64_t deriveConstKey(int64_t site)
{
    return ca::random::SplitMix64(static_cast<uint64_t>(site) ^ kConstKeyOpaque).next();
}

}   // namespace mj2x

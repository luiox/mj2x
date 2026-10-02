/// splitmix64_libca_cross_test.cpp —— #1027 A3 单源切换对拍。
///
/// mj2x/mjt-obf 原有 6 份本地 splitmix64 副本（vmc4_instance 内联类 /
/// reg_transform_passes 单步函数 / vm_interpreter_emit lambda /
/// seed_derivation::splitmix64Step / mjt-obf seed_chain::mix64 /
/// name_provider::nextRand）收敛到 libca ca::random::SplitMix64。本测试内嵌
/// 原公式作参考副本，在 ≥8 组含边界值种子（0 / 1 / 全 F / 黄金比例常数 /
/// 单位置位）× 连续 4 步序列上断言 canonical 与参考副本逐位一致；并钉死
/// deriveClientSeed（本地唯一公开派生入口）与原公式全等。

#include <gtest/gtest.h>

#include <cstdint>
#include <iomanip>

#include "libca/random/split_mix64.hpp"

#include "mj2x/core/seed_derivation.hpp"

namespace {

/// 切换前的本地 splitmix64 公式（六份副本的公共算法体，逐 token 一致）。
/// 两种本地用法：
///   - 有状态流（vmc4 内联类 / emit lambda / name_provider nextRand）：
///     state += G 后混合，输出链 = mix(seed + kG)；
///   - 一步式混合函数（reg_transform_passes / seed_derivation / seed_chain）：
///     每次以派生输入现播种，单次调用 = mix(x + G)。
/// 两者 canonical 对应：有状态流 = ca::random::SplitMix64 连续 next()；
/// 一步式 = SplitMix64(x).next() 单发。
std::uint64_t formulaMix(std::uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

TEST(SplitMix64LibcaCross, MatchesFormulaBoundarySeedsFourSteps)
{
    // 有状态流对拍（vmc4/name_provider 用法）：连续 next() = 逐步 +G 后混合。
    const std::uint64_t kSeeds[] = {
        0x0000000000000000ull,   // 边界：全 0
        0x0000000000000001ull,   // 边界：最小非 0
        0xFFFFFFFFFFFFFFFFull,   // 边界：全 F
        0x9E3779B97F4A7C15ull,   // 黄金比例常数自身
        0x8000000000000000ull,   // 单位置位（最高位）
        0x0000000000000080ull,   // 单位置位（低位）
        0x0123456789ABCDEFull,   // 经典向量
        0xDEADBEEFCAFEBABEull,   // 标记模式
    };
    constexpr std::uint64_t kGamma = 0x9E3779B97F4A7C15ull;
    for (const std::uint64_t seed : kSeeds) {
        ca::random::SplitMix64 rng(seed);
        std::uint64_t         state = seed;
        for (int step = 1; step <= 4; ++step) {
            state += kGamma;
            EXPECT_EQ(rng.next(), formulaMix(state))
                << "seed=" << std::hex << seed << " step=" << std::dec << step;
        }
    }
}

TEST(SplitMix64LibcaCross, OneShotMatchesFormulaBoundarySeeds)
{
    // 一步式混合函数对拍（reg_transform_passes/seed_derivation/seed_chain 用法）：
    // SplitMix64(x).next() 单发 = mix(x + G)，同原本地 splitmix64(x)。
    const std::uint64_t kInputs[] = {
        0x0000000000000000ull,
        0x0000000000000001ull,
        0xFFFFFFFFFFFFFFFFull,
        0x9E3779B97F4A7C15ull,
        0x8000000000000000ull,
        0x0000000000000080ull,
        0x0123456789ABCDEFull,
        0xDEADBEEFCAFEBABEull,
    };
    for (const std::uint64_t x : kInputs) {
        EXPECT_EQ(ca::random::SplitMix64(x).next(), formulaMix(x + 0x9E3779B97F4A7C15ull))
            << "x=" << std::hex << x;
    }
}

TEST(SplitMix64LibcaCross, NextBoundedMatchesModulo)
{
    // next_bounded(bound) == next() % bound（与原本地 nextBounded 同式）。
    // bound==0：libca 在 assert 构建下断言、NDEBUG 下抛 invalid_argument——
    // 原本地实现为 UB 取模；生产调用点无零 bound（shuffle 环 ≥2 / size==1
    // 短路后 ≥2），分歧面不可达，不在此构造。
    ca::random::SplitMix64 rng(42);
    ca::random::SplitMix64 ref(42);
    for (const std::uint64_t bound : {1ull, 2ull, 3ull, 255ull, 256ull, 65536ull}) {
        EXPECT_EQ(rng.next_bounded(bound), ref.next() % bound) << "bound=" << bound;
    }
}

TEST(SplitMix64LibcaCross, DeriveClientSeedMatchesFormula)
{
    // deriveClientSeed 的混合步 = SplitMix64(base ^ client).next() 单发（原
    // detail::splitmix64Step 同式）；clientId==0 恒等分支不受影响。
    const std::uint64_t kBases[]   = {0ull, 1ull, 42ull, 0x9E3779B97F4A7C15ull,
                                      0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                                      0x0123456789ABCDEFull, 0xDEADBEEFCAFEBABEull};
    const std::uint64_t kClients[] = {1ull, 7ull, 255ull, 0xFFFFFFFFFFFFFFFFull};
    for (const std::uint64_t base : kBases) {
        EXPECT_EQ(mj2x::deriveClientSeed(base, 0), base) << "identity base=" << base;
        for (const std::uint64_t client : kClients) {
            EXPECT_EQ(mj2x::deriveClientSeed(base, client),
                      formulaMix((base ^ client) + 0x9E3779B97F4A7C15ull))
                << "base=" << base << " client=" << client;
        }
    }
}

}   // namespace

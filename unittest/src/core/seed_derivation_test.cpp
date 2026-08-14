// seed_derivation_test.cpp - per-client 唯一派生层（#759/#760 借鉴低成本项）。

#include <gtest/gtest.h>

#include "mj2x/core/seed_derivation.hpp"

namespace {

TEST(SeedDerivationTest, ClientIdZeroIsIdentity)
{
    // 不变量：clientId == 0 时派生恒等 —— 默认构建产物与历史逐字节一致。
    EXPECT_EQ(mj2x::deriveClientSeed(0, 0), 0u);
    EXPECT_EQ(mj2x::deriveClientSeed(42, 0), 42u);
    EXPECT_EQ(mj2x::deriveClientSeed(0x9E3779B97F4A7C15ull, 0), 0x9E3779B97F4A7C15ull);
}

TEST(SeedDerivationTest, SameClientSameSeedDeterministic)
{
    // 可复现构建：同 clientId + 同 baseSeed → 同派生。
    EXPECT_EQ(mj2x::deriveClientSeed(42, 7), mj2x::deriveClientSeed(42, 7));
    EXPECT_EQ(mj2x::deriveClientSeed(0x1234ull, 0xABCDull),
              mj2x::deriveClientSeed(0x1234ull, 0xABCDull));
}

TEST(SeedDerivationTest, DifferentClientsDiverge)
{
    // 每个客户端唯一：不同 clientId → 不同派生（同 baseSeed）。
    const uint64_t a = mj2x::deriveClientSeed(42, 7);
    const uint64_t b = mj2x::deriveClientSeed(42, 8);
    EXPECT_NE(a, b);
    // 雪崩：低 1 位差异的 clientId 也应显著发散（不止低位翻转）。
    const uint64_t c = mj2x::deriveClientSeed(42, 7);
    const uint64_t d = mj2x::deriveClientSeed(42, 8);
    EXPECT_NE(c >> 32, d >> 32);
}

TEST(SeedDerivationTest, DifferentBaseSeedsDiverge)
{
    // 同一 clientId 下不同 baseSeed 依然发散（与 transform seed 正交）。
    EXPECT_NE(mj2x::deriveClientSeed(1, 7), mj2x::deriveClientSeed(2, 7));
}

TEST(SeedDerivationTest, FullWidthClientId)
{
    // 64 位 clientId 全宽参与（不截断）。
    const uint64_t hi = 0xFFFFFFFFFFFFFFFFull;
    EXPECT_NE(mj2x::deriveClientSeed(42, hi), mj2x::deriveClientSeed(42, hi - 1));
    // 且与低位 clientId 不同。
    EXPECT_NE(mj2x::deriveClientSeed(42, hi), mj2x::deriveClientSeed(42, 0xFF));
}

} // namespace

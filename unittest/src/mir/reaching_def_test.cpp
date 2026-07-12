// reaching-def 数据流自测。
// 直接构造手工 L1 CFG（不依赖 CodeConverter/字节码），覆盖 R0.5 发现的 libjbytecode
// Analyzer 缺陷场景（循环归纳变量的多 def 源 = phi），证明自写 pass 正确。
// 算法见 doc/SSA_ALGORITHM.md §3。
#include <gmock/gmock.h>

#include "mj2x/mir/reaching_def.hpp"

using namespace mj2x;
using testing::Ge;

// 手工构造 sumTo 风格的循环 CFG（3 块）：
//   B0(entry): 定义 s=0 (slot1), i=0 (slot2); goto B1
//   B1(header): 循环头；前驱 B0 + B2(回边)
//   B2(body):  定义 s+=i (slot1), i++ (slot2); goto B1
// 预期：B1 入口处 slot1(s) 有 2 个 def（B0 的 s=0 + B2 的 s+=i）= phi；slot2(i) 同理。
static std::vector<ReachingDefBlock> buildLoopCFG()
{
    std::vector<ReachingDefBlock> blocks(3);
    // B0
    blocks[0].labelId           = 0;
    blocks[0].predecessorLabels = {};
    blocks[0].localDefs         = {{0, 1}, {1, 2}};   // node0 写 slot1(s), node1 写 slot2(i)
    // B1 (header)
    blocks[1].labelId           = 1;
    blocks[1].predecessorLabels = {0, 2};   // B0 进入 + B2 回边
    blocks[1].localDefs         = {};       // 不写
    // B2 (body)
    blocks[2].labelId           = 2;
    blocks[2].predecessorLabels = {1};
    blocks[2].localDefs         = {{0, 1}, {1, 2}};   // 回到 slot1, slot2 各写一次
    return blocks;
}

TEST(ReachingDefTest, LoopInductionVarHasTwoDefsAtHeader)
{
    auto blocks = buildLoopCFG();
    auto r      = computeReachingDef(blocks);

    // B1 入口 slot1(s)：应有 2 个 def（phi）。这是 R0.5 libjbytecode Analyzer 算不出的场景。
    EXPECT_THAT(r.inDefCount(1, /*slot*/ 1), Ge(2))
        << "loop header slot1 (s) must have >=2 reaching defs (= phi)";
    EXPECT_THAT(r.inDefCount(1, /*slot*/ 2), Ge(2))
        << "loop header slot2 (i) must have >=2 reaching defs (= phi)";
}

TEST(ReachingDefTest, SingleDefWhereNoMerge)
{
    // 无回边、无双前驱：每个 slot 只有 1 个 def 到达。
    std::vector<ReachingDefBlock> blocks(2);
    blocks[0].labelId           = 0;
    blocks[0].predecessorLabels = {};
    blocks[0].localDefs         = {{0, 1}};   // 写 slot1
    blocks[1].labelId           = 1;
    blocks[1].predecessorLabels = {0};
    blocks[1].localDefs         = {};   // 不写

    auto r = computeReachingDef(blocks);
    EXPECT_EQ(r.inDefCount(1, 1), 1) << "no-merge slot must have exactly 1 reaching def";
}

TEST(ReachingDefTest, KilledDefDoesNotReach)
{
    // 同块内多次写同 slot：只有最后一次 gen 到达出口。
    std::vector<ReachingDefBlock> blocks(2);
    blocks[0].labelId           = 0;
    blocks[0].predecessorLabels = {};
    blocks[0].localDefs         = {{0, 1}, {1, 1}};   // node0 + node1 都写 slot1
    blocks[1].labelId           = 1;
    blocks[1].predecessorLabels = {0};
    blocks[1].localDefs         = {};

    auto r = computeReachingDef(blocks);
    // B0 出口 slot1：gen 只保留最后一次（node1），所以到达 B1 的 def 数 = 1。
    EXPECT_EQ(r.inDefCount(1, 1), 1)
        << "same-block redefinition kills earlier defs; only gen reaches";
}

TEST(ReachingDefTest, EmptyCFG)
{
    auto r = computeReachingDef({});
    SUCCEED();   // 不崩即可
}

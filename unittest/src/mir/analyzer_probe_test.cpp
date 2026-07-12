// R1 analyzer-verification spike.
//
// 用户明确提示：libjbytecode 的 Analyzer<SourceValue> / Analyzer<BasicValue> 没有
// 深度测试过，"可能会有问题"。在基于它们构建 block-argument SSA（R1a）之前，必须先
// 证明它们在真实字节码上、在会产生 phi 的控制流（循环）下行为正确。
//
// **验证结果（R0.5）：全部判定性测试失败，确认 analyzer 有 bug**：
//   1. maxLocals 算错（analyzer.hpp:59-68 从 LocalVariableTable 推，无 -g 时退化为参数大小）
//   2. SourceInterpreter 只追踪栈上值的来源，不追踪 local 的 reaching-def（getLocal().insns_ 恒空）
// 故 R1-B 决定走兜底（自写 reaching-def，从 CodeConverter 的 CFG），详见 dev_plan.md。
//
// 这些测试体保留作**负证据留档**，全部 DISABLED_ —— 不跑、不阻塞 CI，但代码与诊断注释保留，
// 未来 libjbytecode analysis 修好后去掉 DISABLED_ 前缀即可重新校验。
//
// 测试样本：test_resource/analyzer/analyzer/AnalyzerProbe.class（javac -g 编译）

#include <gmock/gmock.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "test_helper.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace ca::str;

namespace {

std::string toStr(Utf8StringRef ref)
{
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

// 在 test_resource/analyzer/analyzer/AnalyzerProbe.class 里找名为 name+desc 的方法。
// 返回 nullptr 表示没找到（测试会 FAIL，便于发现资源缺失）。
MethodNode* findMethod(ClassNode& cn, const std::string& name, const std::string& desc)
{
    for (auto* m : cn.methods) {
        if (toStr(m->name) == name && toStr(m->desc) == desc) return m;
    }
    return nullptr;
}

// 用 libjbytecode 读 AnalyzerProbe.class（走 CompilerIndex 同款 arena 路径）。
std::unique_ptr<ClassNode> loadProbeClass()
{
    auto            bytes = test::getResource("analyzer/analyzer/AnalyzerProbe.class");
    auto            cn    = std::make_unique<ClassNode>();
    Utf8StringArena arena;
    ClassReader     reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

}   // namespace

// ============================================================================
// Part 1: BasicInterpreter — 循环头/合并点的 frame shape 必须正确
// ============================================================================

TEST(AnalyzerProbe, DISABLED_BasicInterpreterSumToLoopHeaderMerges)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(*cn, "sumTo", "(I)I");
    ASSERT_NE(m, nullptr) << "sumTo(I)I not found in AnalyzerProbe";

    BasicInterpreter     interp;
    Analyzer<BasicValue> analyzer(&interp);
    auto                 frames = analyzer.analyze(cn->name, m);
    auto                 insns  = m->instructions.toArray();

    ASSERT_FALSE(frames.empty()) << "analyzer returned no frames";
    ASSERT_EQ(frames.size(), insns.size()) << "frame count must match instruction count";

    // pc 4 (iload_2) 是循环头：前驱 = 顺序进入(pc3 istore_2 之后) + 回边(pc16 goto)。
    // 断言：该处 frame 存在（可达且已 merge 成功），locals 数 == 3 (n,s,i)，stack 空。
    size_t loopHeader = 4;
    ASSERT_GT(frames.size(), loopHeader);
    auto* fh = frames[loopHeader];
    ASSERT_NE(fh, nullptr) << "loop header frame at pc4 is null (unreachable?)";
    EXPECT_EQ(fh->getLocals(), 3) << "sumTo needs 3 locals: n(0), s(1), i(2)";
    EXPECT_EQ(fh->getStackSize(), 0) << "entering iload_2, stack must be empty";

    // s (local1) 和 i (local2) 在循环头都必须是 INT（ICONST_0 与 IADD/IINC 的 merge）。
    // 这是 BasicInterpreter 最容易出错的地方：merge 必须收敛到 INT 而非退化成 TOP/未初始化。
    EXPECT_EQ(fh->getLocal(1), BasicValue::INT_VALUE)
        << "local1 (s) must merge to INT at loop header";
    EXPECT_EQ(fh->getLocal(2), BasicValue::INT_VALUE)
        << "local2 (i) must merge to INT at loop header";
}

TEST(AnalyzerProbe, DISABLED_BasicInterpreterGcdWhileLoopMerge)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(*cn, "sumTo", "(I)I");   // 先确认 class 加载正常
    (void)m;
    auto* g = findMethod(*cn, "gcd", "(II)I");
    ASSERT_NE(g, nullptr) << "gcd(II)I not found";

    BasicInterpreter     interp;
    Analyzer<BasicValue> analyzer(&interp);
    auto                 frames = analyzer.analyze(cn->name, g);

    // gcd 的回边目标是 pc0（方法入口本身，goto 0）。入口 frame 本身就是 merge 结果。
    ASSERT_FALSE(frames.empty());
    auto* f0 = frames[0];
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(f0->getLocals(), 3) << "gcd needs 3 locals: a(0), b(1), t(2)";
    // a (local0) 在 pc0 有两个 def 源：方法参数 + pc11 istore_0；必须 merge 成 INT。
    EXPECT_EQ(f0->getLocal(0), BasicValue::INT_VALUE)
        << "local0 (a) must merge to INT across while-loop back-edge";
    EXPECT_EQ(f0->getLocal(1), BasicValue::INT_VALUE)
        << "local1 (b) must merge to INT across while-loop back-edge";
}

// ============================================================================
// Part 2: SourceInterpreter — reaching-def 集合必须包含真正的多源 phi
// ============================================================================
//
// 这是 R1a block-argument SSA 的根基：phi 正是"该 value 的 SourceValue.insns_
// 含 ≥2 条指令"。如果 SourceInterpreter 在循环回边上算不出多源集合，整个路线就站不住。

TEST(AnalyzerProbe, DISABLED_SourceInterpreterSumToInductionVarHasPhi)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(*cn, "sumTo", "(I)I");
    ASSERT_NE(m, nullptr);

    SourceInterpreter     interp;
    Analyzer<SourceValue> analyzer(&interp);
    auto                  frames = analyzer.analyze(cn->name, m);
    auto                  insns  = m->instructions.toArray();
    ASSERT_EQ(frames.size(), insns.size());

    // pc4 (iload_2) 处 local1 (s) 的 SourceValue.insns_ 必须含 ≥2 条指令：
    //   - ICONST_0 at pc0（s 的初始化）
    //   - IADD at pc11（循环体内 s += i）
    // 这两个 def 都能到达 pc4 的 local1，正是 phi 的语义。
    auto* fh = frames[4];
    ASSERT_NE(fh, nullptr);
    const auto& sSources = fh->getLocal(1).insns_;
    EXPECT_GE(sSources.size(), 2u)
        << "local1 (s) must have >=2 reaching defs at loop header (= phi); got " << sSources.size();

    // 同理 local2 (i)：ICONST_0 at pc2 + IINC at pc13。
    const auto& iSources = fh->getLocal(2).insns_;
    EXPECT_GE(iSources.size(), 2u)
        << "local2 (i) must have >=2 reaching defs at loop header (= phi); got " << iSources.size();
}

TEST(AnalyzerProbe, DISABLED_SourceInterpreterGcdBackEdgePhi)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* g = findMethod(*cn, "gcd", "(II)I");
    ASSERT_NE(g, nullptr);

    SourceInterpreter     interp;
    Analyzer<SourceValue> analyzer(&interp);
    auto                  frames = analyzer.analyze(cn->name, g);
    ASSERT_FALSE(frames.empty());

    // pc0 处 local0 (a) 的 def 源：方法参数（newParameterValue）+ pc11 istore_0。
    // 至少 2 个。如果分析器漏了回边的 copyOperation，这里会失败。
    auto* f0 = frames[0];
    ASSERT_NE(f0, nullptr);
    EXPECT_GE(f0->getLocal(0).insns_.size(), 2u)
        << "local0 (a) must have >=2 reaching defs at gcd entry (= while phi)";
    EXPECT_GE(f0->getLocal(1).insns_.size(), 2u)
        << "local1 (b) must have >=2 reaching defs at gcd entry (= while phi)";
}

// ============================================================================
// Part 3: 单源（无 phi）的正确性 —— 确保分析器不会"过度合并"
// ============================================================================
//
// 如果 SourceInterpreter 把所有东西都并成一个超大集合（常见 bug：merge 退化成
// "全联通"），Part2 也会通过但语义错误。这一节反向校验：没有控制流汇合的地方，
// insns_ 必须只有 1 个源。

TEST(AnalyzerProbe, DISABLED_SourceInterpreterSingleDefWhereNoMerge)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* a = findMethod(*cn, "abs", "(I)I");
    ASSERT_NE(a, nullptr);

    SourceInterpreter     interp;
    Analyzer<SourceValue> analyzer(&interp);
    auto                  frames = analyzer.analyze(cn->name, a);

    // abs 的 pc0 (iload_0) 处，local0 只有方法参数一个 def 源（还没进入任何 merge）。
    auto* f0 = frames[0];
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(f0->getLocal(0).insns_.size(), 1u)
        << "local0 at method entry must have exactly 1 source (the parameter)";
}

// ============================================================================
// DIAGNOSTIC dump（调试用，非判定性）—— 观察分析器实际产出的 frame 内容
// ============================================================================
TEST(AnalyzerProbe, DISABLED_DiagnosticDumpSumTo)
{
    auto cn = loadProbeClass();
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(*cn, "sumTo", "(I)I");
    ASSERT_NE(m, nullptr);
    auto insns = m->instructions.toArray();

    printf("\n=== sumTo BasicInterpreter ===\n");
    {
        BasicInterpreter     interp;
        Analyzer<BasicValue> analyzer(&interp);
        auto                 frames = analyzer.analyze(cn->name, m);
        printf("  frames.size()=%zu insns.size()=%zu maxLocals=%d\n",
               frames.size(),
               insns.size(),
               m->maxLocals);
        for (size_t i = 0; i < frames.size() && i < insns.size(); i++) {
            auto* fr = frames[i];
            printf("  [%zu] locals=%d", i, fr ? fr->getLocals() : -1);
            if (fr) {
                printf(" [");
                for (int j = 0; j < fr->getLocals(); j++)
                    printf("%s", fr->getLocal(j).toString().c_str());
                printf("] stack=%d[", fr->getStackSize());
                for (int j = 0; j < fr->getStackSize(); j++)
                    printf("%s", fr->getStack(j).toString().c_str());
                printf("]");
            }
            else {
                printf(" (null)");
            }
            printf("\n");
        }
    }

    printf("\n=== sumTo SourceInterpreter ===\n");
    {
        SourceInterpreter     interp;
        Analyzer<SourceValue> analyzer(&interp);
        auto                  frames = analyzer.analyze(cn->name, m);
        for (size_t i = 0; i < frames.size() && i < insns.size(); i++) {
            auto* fr = frames[i];
            printf("  [%zu] locals=%d", i, fr ? fr->getLocals() : -1);
            if (fr) {
                printf(" [");
                for (int j = 0; j < fr->getLocals(); j++)
                    printf("L%d:%zu ", j, fr->getLocal(j).insns_.size());
                printf("] stack=%d[", fr->getStackSize());
                for (int j = 0; j < fr->getStackSize(); j++)
                    printf("S%d:%zu ", j, fr->getStack(j).insns_.size());
                printf("]");
            }
            else {
                printf(" (null)");
            }
            printf("\n");
        }
    }
    SUCCEED();
}

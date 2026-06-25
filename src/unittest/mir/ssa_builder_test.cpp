// R1a 端到端验收：Java 方法 → CodeConverter → SSABuilder → SSAFunction。
//
// 验收标准（doc/SSA_ALGORITHM.md §7）：
//   1. add: 线性方法，SSA 无栈槽引用，dump 不含 cstack/sp。
//   2. loopSum/loopCount: 循环方法，循环头块有 block arg（phi 语义）。
//   3. dump 文本过 c2j CSTACK_RE（cstack(\d+)\.(\w)）→ 零匹配（消除锚点 2）。
//
// 样本：test_resource/ssa/ssa/SsaSamples.class（javac 编译）
#include <gmock/gmock.h>

#include <filesystem>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

#include "test_helper.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_dump.hpp"
#include "mj2x/mj2x.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace ca::str;

namespace {

std::string toStr(Utf8StringRef ref) {
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

// 读 SsaSamples.class，返回 classNode（arena 由调用方持有以保活 Utf8StringRef）。
ClassNode* loadSamples(Utf8StringArena& arena, std::vector<ca::u8>& bytesOut) {
    bytesOut = test::getResource("ssa/ssa/SsaSamples.class");
    auto cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytesOut.data(), static_cast<ca::i32>(bytesOut.size()));
    reader.accept(cn.get(), 0);
    // 返回裸指针；调用方持 arena + cn 容器。
    // 为简化，这里用一个全局持有 cn（测试用）。
    static std::unique_ptr<ClassNode> g_cn;
    g_cn = std::move(cn);
    return g_cn.get();
}

MethodNode* findMethod(ClassNode* cn, const std::string& name, const std::string& desc) {
    for (auto* m : cn->methods) {
        if (toStr(m->name) == name && toStr(m->desc) == desc) return m;
    }
    return nullptr;
}

// 跑 CodeConverter + SSABuilder，返回 SSAFunction（或 nullptr + reason）。
std::unique_ptr<SSAFunction> buildSSAFor(ClassNode* cn, MethodNode* m, std::string& reason) {
    CodeConverter converter;
    auto blocks = converter.convert(*m);
    SSABuilder builder;
    bool isStatic = (m->access & Opcodes::ACC_STATIC) != 0;
    return builder.build(blocks, toStr(cn->name), toStr(m->name), toStr(m->desc),
                         isStatic, m->maxLocals, reason);
}

} // namespace

// ============================================================================
// add(I)I —— 线性方法，基本 IR 生成 + 无栈槽
// ============================================================================
TEST(SSABuilderE2E, AddProducesStacklessSSA) {
    Utf8StringArena arena;
    std::vector<ca::u8> bytes;
    ClassNode* cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "add", "(II)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto fn = buildSSAFor(cn, m, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);

    // 验收 1：dump 不含 c2j 栈锚点子串。
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << "dump must not contain cstack";
    EXPECT_EQ(dump.find("sp"), std::string::npos) << "dump must not contain sp";

    // 验收 2：c2j CSTACK_RE 正则（cstack(\d+)\.(\w)）零匹配。
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe))
        << "CSTACK_RE must not match SSA dump (锚点 2 已打破)";

    // 验收 3：至少有一条 add 指令（param0 + param1）。
    EXPECT_NE(dump.find("+"), std::string::npos)
        << "add method must contain an add operation:\n"
        << dump;

    // 打印 dump 供人工检查（gtest 在失败或 --gtest_also_run_disabled_tests 时可见）
    if (::testing::Test::HasFailure()) {
        printf("=== dump ===\n%s\n", dump.c_str());
    }
}

// ============================================================================
// loopSum(I)I —— 循环，验证 block-arg phi
// ============================================================================
TEST(SSABuilderE2E, LoopSumHasPhiBlockArg) {
    Utf8StringArena arena;
    std::vector<ca::u8> bytes;
    ClassNode* cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto fn = buildSSAFor(cn, m, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);

    // 验收 1：dump 无栈锚点 + CSTACK_RE 零匹配。
    EXPECT_EQ(dump.find("cstack"), std::string::npos);
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe));

    // 验收 2：存在带 args 的块（= block-arg phi）。循环头块应有 args。
    // loopSum 的循环头对 s(local1) 和 i(local2) 各有 ≥2 def 源 → ≥2 args。
    bool foundBlockWithArgs = false;
    int maxArgsOnAnyBlock = 0;
    for (auto& b : fn->blocks) {
        int argCount = static_cast<int>(b->args.size());
        if (argCount > maxArgsOnAnyBlock) maxArgsOnAnyBlock = argCount;
        if (argCount >= 2) foundBlockWithArgs = true;
    }
    EXPECT_TRUE(foundBlockWithArgs)
        << "loopSum must have a block with >=2 args (phi for s and i):\n"
        << dump;

    if (::testing::Test::HasFailure()) {
        printf("=== loopSum dump ===\n%s\n", dump.c_str());
    }
}

// ============================================================================
// loopCount(I)I —— while 循环，验证回边 phi
// ============================================================================
TEST(SSABuilderE2E, LoopCountHasPhiBlockArg) {
    Utf8StringArena arena;
    std::vector<ca::u8> bytes;
    ClassNode* cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "loopCount", "(I)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto fn = buildSSAFor(cn, m, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_EQ(dump.find("cstack"), std::string::npos);
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe));

    // loopCount 的 c(local1) 在回边目标有 2 个 def（iconst_0 + iinc）→ ≥1 arg。
    bool foundBlockWithArgs = false;
    for (auto& b : fn->blocks) {
        if (!b->args.empty()) { foundBlockWithArgs = true; break; }
    }
    EXPECT_TRUE(foundBlockWithArgs)
        << "loopCount must have a block with args (phi for c):\n"
        << dump;

    if (::testing::Test::HasFailure()) {
        printf("=== loopCount dump ===\n%s\n", dump.c_str());
    }
}

// 临时 dump 查看（人工检查 loopSum 的 phi 形态）。跑一次后可 DISABLED。
TEST(SSABuilderE2E, DISABLED_DumpLoopSumSsa) {
    Utf8StringArena arena;
    std::vector<ca::u8> bytes;
    ClassNode* cn = loadSamples(arena, bytes);
    auto* m = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);
    std::string reason;
    auto fn = buildSSAFor(cn, m, reason);
    ASSERT_NE(fn, nullptr);
    printf("\n=== loopSum SSA dump ===\n%s\n", dumpSSA(*fn).c_str());
}

TEST(SSABuilderE2E, DISABLED_DumpLoopSumBlocks) {
    Utf8StringArena arena;
    std::vector<ca::u8> bytes;
    ClassNode* cn = loadSamples(arena, bytes);
    auto* m = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);
    CodeConverter converter;
    auto blocks = converter.convert(*m);
    printf("\n=== loopSum L1 blocks (%zu) ===\n", blocks.size());
    printf("%s\n", CodeConverter::dumpMIR(blocks).c_str());
}


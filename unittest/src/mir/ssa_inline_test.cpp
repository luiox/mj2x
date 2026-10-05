// SSA 方法内联 pass 单测（性能主线：消除 JNI 往返）。
//
// 场景：静态调用内联（参数绑定 + 返回合并 + result 重定向）、多返回点、
// void、递归拒绝、异常区拒绝、大小拒绝、非静态拒绝、解析失败。
// 每个内联成功用例额外跑 RegLowering + validateRegIRNoStackSlots 证明
// 内联后的 SSA 仍然可降级（CFG/引用完整）。
#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/passes/ssa_inline_passes.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"

#include "mj2x/mir/ssa/ssa_builder.hpp"

// B8b：测试侧 Label 统一归宿（进程寿命 scratch arena）——LabelNode() 默认构造
// 已随旧池制度退役，Label 一律 arena 分配。
static garena::Arena& labelScratchArena()
{
    static garena::Arena arena;
    return arena;
}


using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringArena;

namespace {

constexpr int kStaticPublic = Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC;

std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access,
                                           ca::str::Utf8StringRef::from_cstr(name),
                                           ca::str::Utf8StringRef::from_cstr(desc),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    for (auto* insn : insns) mn->instructions.add(insn);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    return mn;
}

/// INVOKESTATIC 指令（MethodInsnNode 需要 Utf8StringRef）。
MethodInsnNode* invokeStatic(const char* owner, const char* name, const char* desc)
{
    return new MethodInsnNode(Opcodes::INVOKESTATIC,
                              ca::str::Utf8StringRef::from_cstr(owner),
                              ca::str::Utf8StringRef::from_cstr(name),
                              ca::str::Utf8StringRef::from_cstr(desc), false);
}

/// CodeConverter + SSABuilder 构建 SSA；owner 统一 "InlineTest"。
/// 注意：converter 必须活到 build 完成（Block 归 converter 所有，提前析构会悬垂）。
std::unique_ptr<SSAFunction> buildSsa(MethodNode& mn, Utf8StringArena& arena, std::string& reason)
{
    CodeConverter converter(arena);
    auto          blocks = converter.convert(mn);
    SSABuilder    builder;
    const bool    isStatic = (mn.access & Opcodes::ACC_STATIC) != 0;
    return builder.build(blocks, "InlineTest", mn.name.to_std_string(), mn.desc.to_std_string(),
                         isStatic, mn.maxLocals, arena, reason);
}

/// 统计函数内 Call 指令数。
ca::usize countCalls(const SSAFunction& fn)
{
    ca::usize count = 0;
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins->kind == SSAInstructionKind::Call) ++count;
        }
    }
    return count;
}

/// 内联后的 SSA 必须能降级为 RegIR 且无栈槽。
bool lowerOk(const SSAFunction& fn, std::string& reason)
{
    RegLowering lowering;
    auto        reg = lowering.lower(fn, reason);
    if (reg == nullptr) return false;
    return validateRegIRNoStackSlots(*reg, reason);
}

}   // namespace

// ---------------------------------------------------------------------------
// 内联成功场景
// ---------------------------------------------------------------------------

TEST(SsaInlineTest, CalleeBuildBlockOrderIsDeterministic)
{
    Utf8StringArena arena;
    auto callee = makeMethod(kStaticPublic, "fe", "(II)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new VarInsnNode(Opcodes::ILOAD, 1),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             3, 2);
    std::string r1, r2;
    auto s1 = buildSsa(*callee, arena, r1);
    auto s2 = buildSsa(*callee, arena, r2);
    ASSERT_NE(s1, nullptr) << r1;
    ASSERT_NE(s2, nullptr) << r2;
    ASSERT_EQ(s1->blocks.size(), s2->blocks.size());
    bool sameOrder = true;
    for (ca::usize i = 0; i < s1->blocks.size(); ++i) {
        if (s1->blocks[i]->id.value != s2->blocks[i]->id.value) sameOrder = false;
    }
    EXPECT_TRUE(sameOrder) << "callee block order must be deterministic across builds";
}

TEST(SsaInlineTest, InlinesStaticAddWithParamBindingAndReturnMerge)
{
    Utf8StringArena arena;

    // callee: static int helper(int a, int b) { return a + b; }
    auto callee = makeMethod(kStaticPublic, "helper", "(II)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new VarInsnNode(Opcodes::ILOAD, 1),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 2);
    // caller: static int f() { return helper(9, 4); }
    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new IntInsnNode(Opcodes::BIPUSH, 9),
                              new IntInsnNode(Opcodes::BIPUSH, 4),
                              invokeStatic("InlineTest", "helper", "(II)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 0);

    std::string reason;
    auto        callerSsa = buildSsa(*caller, arena, reason);
    ASSERT_NE(callerSsa, nullptr) << reason;
    ASSERT_EQ(countCalls(*callerSsa), 1U);

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "helper" || ref.desc != "(II)I") return nullptr;
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineOptions options;
    SsaInlineStats   stats = applySsaInlining(*callerSsa, options, resolver, arena);


    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 0U) << "call site must be replaced by inlined body";
    // 返回汇合块：1 个 block arg（返回类型 I32）+ 原 return 引用已重定向。
    bool sawMerge = false;
    for (const auto& block : callerSsa->blocks) {
        if (block->args.size() == 1U && block->args.front().type == SSAType::I32) sawMerge = true;
        for (const auto& ins : block->instructions) {
            if (ins->kind == SSAInstructionKind::Binary &&
                static_cast<const SSABinaryInstruction*>(ins.get())->op == SSABinaryOp::Add) {
                // 克隆体里应有 Add（参数已绑定常量 9/4）。
            }
        }
    }
    EXPECT_TRUE(sawMerge) << "return merge block with one I32 arg expected";

    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

TEST(SsaInlineTest, MergesMultipleReturnPoints)
{
    Utf8StringArena arena;

    // callee: static int pick(int x) { if (x > 0) return 1; else return -1; }
    auto* posLabel = new LabelNode(labelScratchArena().create<Label>());
    auto* endLabel = new LabelNode(labelScratchArena().create<Label>());
    auto callee    = makeMethod(kStaticPublic, "pick", "(I)I",
                                {new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::ICONST_0),
                                 new JumpInsnNode(Opcodes::IF_ICMPLE, posLabel->label),
                                 new InsnNode(Opcodes::ICONST_M1),
                                 new InsnNode(Opcodes::IRETURN),
                                 posLabel,
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IRETURN)},
                                1, 1);
    // caller: static int f() { return pick(7); }
    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new IntInsnNode(Opcodes::BIPUSH, 7),
                              invokeStatic("InlineTest", "pick", "(I)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;
    const ca::usize blocksBefore = callerSsa->blocks.size();

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "pick" || ref.desc != "(I)I") return nullptr;
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 0U);
    EXPECT_GT(callerSsa->blocks.size(), blocksBefore) << "inlined body blocks appended";
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

TEST(SsaInlineTest, InlinesStaticCallWithRefParamAndIndySite)
{
    Utf8StringArena arena;

    // callee: static String wrap(String s) { return "[" + s + "]"; }
    // 其中 "[" + s + "]" 用 invokedynamic makeConcatWithConstants → Intrinsic("indy:0")。
    auto callee = makeMethod(kStaticPublic, "wrap", "(Ljava/lang/String;)Ljava/lang/String;",
                             {new VarInsnNode(Opcodes::ALOAD, 0),
                              new InvokeDynamicInsnNode(
                                  ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                                  ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/lang/String;"),
                                  jbytecode::core::Handle(0,
                                                          ca::str::Utf8StringRef::from_cstr("java/lang/invoke/StringConcatFactory"),
                                                          ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                                                          ca::str::Utf8StringRef::from_cstr("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;")),
                                  std::vector<ca::core::Any>{}),
                              new InsnNode(Opcodes::ARETURN)},
                             2, 1);
    // caller: static String f(String x) { return wrap(x); }
    auto caller = makeMethod(kStaticPublic, "f", "(Ljava/lang/String;)Ljava/lang/String;",
                             {new VarInsnNode(Opcodes::ALOAD, 0),
                              invokeStatic("InlineTest", "wrap",
                                           "(Ljava/lang/String;)Ljava/lang/String;"),
                              new InsnNode(Opcodes::ARETURN)},
                             1, 1);

    std::string reason;
    auto        callerSsa = buildSsa(*caller, arena, reason);
    ASSERT_NE(callerSsa, nullptr) << reason;
    const ca::usize indyBefore = callerSsa->indySites.size();
    ASSERT_EQ(countCalls(*callerSsa), 1U);

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "wrap" || ref.desc != "(Ljava/lang/String;)Ljava/lang/String;") {
            return nullptr;
        }
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 0U);
    // indy 站点必须合并进 caller，且克隆体引用重映射后的索引。
    EXPECT_EQ(callerSsa->indySites.size(), indyBefore + 1U);
    bool sawIndy = false;
    for (const auto& block : callerSsa->blocks) {
        for (const auto& ins : block->instructions) {
            if (ins->kind == SSAInstructionKind::Intrinsic &&
                static_cast<const SSAIntrinsicInstruction*>(ins.get())->name ==
                    "indy:" + std::to_string(indyBefore)) {
                sawIndy = true;
            }
        }
    }
    EXPECT_TRUE(sawIndy) << "cloned indy reference must be remapped to caller site index";
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

TEST(SsaInlineTest, InlinesCalleeWithExternalCallAndFieldStore)
{
    Utf8StringArena arena;

    // callee: static String fe(long n) { return String.format(Locale, "%s", n); }
    // 内部调用全是 JDK（resolver 解析不到 → 保留调用），fe 本身是小方法。
    auto callee = makeMethod(kStaticPublic, "fe", "(J)Ljava/lang/String;",
                             {new FieldInsnNode(Opcodes::GETSTATIC,
                                                ca::str::Utf8StringRef::from_cstr("java/util/Locale"),
                                                ca::str::Utf8StringRef::from_cstr("US"),
                                                ca::str::Utf8StringRef::from_cstr("Ljava/util/Locale;")),
                              new LdcInsnNode(ca::str::Utf8StringRef::from_cstr("%s")),
                              new InsnNode(Opcodes::ICONST_1),
                              new IntInsnNode(Opcodes::ANEWARRAY, 1),
                              new InsnNode(Opcodes::DUP),
                              new InsnNode(Opcodes::ICONST_0),
                              new VarInsnNode(Opcodes::LLOAD, 0),
                              new InsnNode(Opcodes::L2D),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("java/lang/Double"),
                                                 ca::str::Utf8StringRef::from_cstr("valueOf"),
                                                 ca::str::Utf8StringRef::from_cstr("(D)Ljava/lang/Double;"),
                                                 false),
                              new InsnNode(Opcodes::AASTORE),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                                 ca::str::Utf8StringRef::from_cstr("format"),
                                                 ca::str::Utf8StringRef::from_cstr("(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;"),
                                                 false),
                              new InsnNode(Opcodes::ARETURN)},
                             6, 2);
    // caller: static void p(Row r, long n) { r.time = fe(n); }
    auto caller = makeMethod(kStaticPublic, "p", "(LRow;J)V",
                             {new VarInsnNode(Opcodes::ALOAD, 0),
                              new VarInsnNode(Opcodes::LLOAD, 1),
                              invokeStatic("InlineTest", "fe", "(J)Ljava/lang/String;"),
                              new FieldInsnNode(Opcodes::PUTFIELD,
                                                ca::str::Utf8StringRef::from_cstr("Row"),
                                                ca::str::Utf8StringRef::from_cstr("time"),
                                                ca::str::Utf8StringRef::from_cstr("Ljava/lang/String;")),
                              new InsnNode(Opcodes::RETURN)},
                             3, 3);

    std::string reason;
    auto        callerSsa = buildSsa(*caller, arena, reason);
    ASSERT_NE(callerSsa, nullptr) << reason;
    ASSERT_EQ(countCalls(*callerSsa), 1U);
    const ca::usize blocksBefore = callerSsa->blocks.size();

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "fe" || ref.desc != "(J)Ljava/lang/String;") return nullptr;
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 2U) << "JDK String.format + Double.valueOf calls remain";
    EXPECT_GT(callerSsa->blocks.size(), blocksBefore) << "inlined body blocks appended";
    // 存在 jump 到「无 block arg 的新块」（克隆 entry）的块 → 克隆体可达。
    // （store 等调用点后续指令已迁移到 merge 块，调用块只剩 call 前指令 + jump。）
    bool callBlockJumpsToClone = false;
    for (const auto& block : callerSsa->blocks) {
        const auto* j = std::get_if<SSAJumpTerminator>(&block->terminator);
        if (j == nullptr || j->target.block == nullptr || !j->target.block->args.empty()) continue;
        if (j->target.block != callerSsa->entry) callBlockJumpsToClone = true;
    }
    EXPECT_TRUE(callBlockJumpsToClone)
        << "call block must jump to cloned entry, otherwise inlined body is unreachable";
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

TEST(SsaInlineTest, InlinesVoidStaticCallWithoutReturnValue)
{
    Utf8StringArena arena;

    // callee: static void bump() { int x = 1; }
    auto callee = makeMethod(kStaticPublic, "bump", "()V",
                             {new InsnNode(Opcodes::ICONST_1),
                              new VarInsnNode(Opcodes::ISTORE, 0),
                              new InsnNode(Opcodes::RETURN)},
                             1, 1);
    // caller: static void f() { bump(); }
    auto caller = makeMethod(kStaticPublic, "f", "()V",
                             {invokeStatic("InlineTest", "bump", "()V"),
                              new InsnNode(Opcodes::RETURN)},
                             0, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "bump" || ref.desc != "()V") return nullptr;
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 0U);
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

// ---------------------------------------------------------------------------
// fail-closed 拒绝场景
// ---------------------------------------------------------------------------

TEST(SsaInlineTest, SkipsRecursiveCall)
{
    Utf8StringArena arena;

    // caller == callee：static int f(int n) { return f(n - 1); }（直接递归）
    auto method = makeMethod(kStaticPublic, "f", "(I)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new InsnNode(Opcodes::ICONST_1),
                              new InsnNode(Opcodes::ISUB),
                              invokeStatic("InlineTest", "f", "(I)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 1);

    std::string reason;
    auto        ssa = buildSsa(*method, arena, reason);
    ASSERT_NE(ssa, nullptr) << reason;

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        return buildSsa(*method, arena, reason);   // 解析出自身
    };

    SsaInlineStats stats = applySsaInlining(*ssa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.skippedByRecursion, 1U);
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*ssa), 1U) << "recursive call site must remain";
}

TEST(SsaInlineTest, SkipsExceptionalCallee)
{
    Utf8StringArena arena;

    // callee: static int helper(int a) { return a; }
    auto callee = makeMethod(kStaticPublic, "helper", "(I)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 1);
    // caller: static int f() { return helper(1); }
    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new InsnNode(Opcodes::ICONST_1),
                              invokeStatic("InlineTest", "helper", "(I)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        auto calleeSsa = buildSsa(*callee, arena, reason);
        if (calleeSsa != nullptr) calleeSsa->exceptionRegions.push_back(SSAExceptionRegion{});
        return calleeSsa;
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.skippedByException, 1U);
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*callerSsa), 1U);
}

TEST(SsaInlineTest, SkipsOversizedCallee)
{
    Utf8StringArena arena;

    auto callee = makeMethod(kStaticPublic, "helper", "(II)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new VarInsnNode(Opcodes::ILOAD, 1),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 2);
    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new InsnNode(Opcodes::ICONST_0),
                              new InsnNode(Opcodes::ICONST_1),
                              invokeStatic("InlineTest", "helper", "(II)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineOptions options;
    options.maxCalleeInstructions = 0;   // callee 至少 1 条 SSA 指令 → 拒绝
    SsaInlineStats   stats = applySsaInlining(*callerSsa, options, resolver, arena);

    EXPECT_EQ(stats.skippedBySize, 1U);
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*callerSsa), 1U);
}

TEST(SsaInlineTest, SkipsNonStaticCall)
{
    Utf8StringArena arena;

    // caller: static int f(InlineTest o) { return o.helper(); }
    auto caller = makeMethod(kStaticPublic, "f", "(LInlineTest;)I",
                             {new VarInsnNode(Opcodes::ALOAD, 0),
                              new MethodInsnNode(Opcodes::INVOKEVIRTUAL, ca::str::Utf8StringRef::from_cstr("InlineTest"), ca::str::Utf8StringRef::from_cstr("helper"), ca::str::Utf8StringRef::from_cstr("()I"), false),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 1);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    // resolver 故意返回一个可解析结果——pass 必须因非静态调用而拒绝。
    auto callee = makeMethod(kStaticPublic, "helper", "()I",
                             {new InsnNode(Opcodes::ICONST_0),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 0);
    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        return buildSsa(*callee, arena, reason);
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);

    EXPECT_EQ(stats.skippedNotInlinable, 1U);
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*callerSsa), 1U);
}

TEST(SsaInlineTest, SkipsUnresolvableCall)
{
    Utf8StringArena arena;

    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new IntInsnNode(Opcodes::BIPUSH, 1),
                              invokeStatic("Other", "missing", "(I)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    SsaInlineStats stats =
        applySsaInlining(*callerSsa, SsaInlineOptions{},
                         [](const SSAMethodRef&) -> std::unique_ptr<SSAFunction> { return nullptr; },
                         arena);

    EXPECT_EQ(stats.skippedUnresolvable, 1U);
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*callerSsa), 1U);
}

// #1193 可见化：unresolvable 调用点样例收集——多调用点去重、计数不丢。
TEST(SsaInlineTest, CollectsDedupedUnresolvableSamples)
{
    Utf8StringArena arena;

    // 同一不可解析目标两个调用点：POP 首个返回值后再调一次。
    auto caller = makeMethod(kStaticPublic, "f", "()I",
                             {new IntInsnNode(Opcodes::BIPUSH, 1),
                              invokeStatic("Other", "missing", "(I)I"),
                              new InsnNode(Opcodes::POP),
                              new IntInsnNode(Opcodes::BIPUSH, 2),
                              invokeStatic("Other", "missing", "(I)I"),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 0);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    SsaInlineStats stats =
        applySsaInlining(*callerSsa, SsaInlineOptions{},
                         [](const SSAMethodRef&) -> std::unique_ptr<SSAFunction> { return nullptr; },
                         arena);

    EXPECT_EQ(stats.skippedUnresolvable, 2U);
    ASSERT_EQ(stats.unresolvableSamples.size(), 1U);   // 去重后 1 条
    EXPECT_EQ(stats.unresolvableSamples[0], "Other.missing:(I)I");
}

// #1193 可见化：样例封顶——超过 kUnresolvableSampleCap 只计数不增样例。
TEST(SsaInlineTest, CapsUnresolvableSamples)
{
    Utf8StringArena arena;

    // 20 个不同目标名（样例去重键为 "owner.name:desc"，同名会合并成 1 条）。
    // names 存活到测试结束：from_cstr 按字面量约定引用外部存储，不复制。
    std::vector<std::string> names;
    for (int i = 0; i < 20; ++i) names.push_back("missing" + std::to_string(i));
    std::vector<AbstractInsnNode*> insns;
    for (int i = 0; i < 19; ++i) {
        insns.push_back(new IntInsnNode(Opcodes::BIPUSH, i));
        insns.push_back(invokeStatic("Other", names[i].c_str(), "(I)I"));
        insns.push_back(new InsnNode(Opcodes::POP));
    }
    // 末次调用结果直接返回（IRETURN 需要栈顶 int）。
    insns.push_back(new IntInsnNode(Opcodes::BIPUSH, 19));
    insns.push_back(invokeStatic("Other", names[19].c_str(), "(I)I"));
    insns.push_back(new InsnNode(Opcodes::IRETURN));
    auto caller = makeMethod(kStaticPublic, "f", "()I", {}, 2, 0);
    for (auto* insn : insns) caller->instructions.add(insn);

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;

    SsaInlineStats stats =
        applySsaInlining(*callerSsa, SsaInlineOptions{},
                         [](const SSAMethodRef&) -> std::unique_ptr<SSAFunction> { return nullptr; },
                         arena);

    EXPECT_EQ(stats.skippedUnresolvable, 20U);
    EXPECT_EQ(stats.unresolvableSamples.size(), size_t(kUnresolvableSampleCap));
}

// ---------------------------------------------------------------------------
// #1188：受保护块内内联必须补异常受保护边
// ---------------------------------------------------------------------------

TEST(SsaInlineTest, InlinedBlocksInheritCallerExceptionProtection)
{
    Utf8StringArena arena;

    // callee: static int helper(int a) { return a + 1; }
    auto callee = makeMethod(kStaticPublic, "helper", "(I)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new InsnNode(Opcodes::ICONST_1),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 1);
    // caller: static int f(int x) {
    //   try { return helper(x); } catch (RuntimeException e) { return x + 1; }
    // }
    // handler 读 slot 0（参数）→ handler 块带 exception + slot0 两个 arg，
    // 每条受保护边携带非空 handlerArgs（可验证传输拷贝）。
    auto* tryStart  = new LabelNode(labelScratchArena().create<Label>());
    auto* tryEnd    = new LabelNode(labelScratchArena().create<Label>());
    auto* handlerBb = new LabelNode(labelScratchArena().create<Label>());
    auto  caller    = makeMethod(kStaticPublic, "f", "(I)I",
                                 {tryStart,
                                  new VarInsnNode(Opcodes::ILOAD, 0),
                                  invokeStatic("InlineTest", "helper", "(I)I"),
                                  new InsnNode(Opcodes::IRETURN),
                                  tryEnd,
                                  handlerBb,
                                  new VarInsnNode(Opcodes::ASTORE, 1),
                                  new VarInsnNode(Opcodes::ILOAD, 0),
                                  new InsnNode(Opcodes::ICONST_1),
                                  new InsnNode(Opcodes::IADD),
                                  new InsnNode(Opcodes::IRETURN)},
                                 2, 2);
    caller->tryCatchBlocks.push_back(new TryCatchBlockNode(
        tryStart->label, tryEnd->label, handlerBb->label,
        ca::str::Utf8StringRef::from_cstr("java/lang/RuntimeException")));

    std::string reason;
    std::unique_ptr<SSAFunction> callerSsa;
    try {
        callerSsa = buildSsa(*caller, arena, reason);
    } catch (const std::exception& e) {
        FAIL() << "buildSsa caller threw: " << e.what();
    }
    ASSERT_NE(callerSsa, nullptr) << reason;
    ASSERT_EQ(callerSsa->exceptionRegions.size(), 1U)
        << "caller must carry one exception region";
    ASSERT_FALSE(callerSsa->exceptionRegions.front().edges.empty());
    ASSERT_EQ(countCalls(*callerSsa), 1U);

    // 内联前记录：块 id 集、调用块、调用块边的 handlerArgs。
    std::set<ca::u32> idsBefore;
    for (const auto& block : callerSsa->blocks) idsBefore.insert(block->id.value);
    SSABlock*            callBlock = nullptr;
    std::vector<SSAValue*> hostArgs;
    for (const auto& block : callerSsa->blocks) {
        bool hasCall = false;
        for (const auto& ins : block->instructions) {
            if (ins->kind == SSAInstructionKind::Call) hasCall = true;
        }
        if (!hasCall) continue;
        callBlock = block.get();
        for (const auto& edge : callerSsa->exceptionRegions.front().edges) {
            if (edge.protectedBlock == callBlock) hostArgs = edge.handlerArgs;
        }
        break;
    }
    ASSERT_NE(callBlock, nullptr);

    const auto resolver = [&](const SSAMethodRef& ref) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "helper" || ref.desc != "(I)I") return nullptr;
        return buildSsa(*callee, arena, reason);
    };
    SsaInlineStats stats = applySsaInlining(*callerSsa, SsaInlineOptions{}, resolver, arena);
    EXPECT_EQ(stats.inlinedCalls, 1U);
    EXPECT_EQ(countCalls(*callerSsa), 0U);

    // 不变量：每个新增块（克隆体 + merge）必须登记为受保护块，且有边、
    // 边的 handlerArgs 与调用块边一致（调用点前帧状态）。
    const auto& region = callerSsa->exceptionRegions.front();
    std::set<ca::u32> idsAfter;
    for (const auto& block : callerSsa->blocks) idsAfter.insert(block->id.value);
    ca::usize newBlocks = 0;
    for (ca::u32 id : idsAfter) {
        if (idsBefore.count(id) != 0) continue;
        ++newBlocks;
        SSABlock* newBlock = nullptr;
        for (const auto& block : callerSsa->blocks) {
            if (block->id.value == id) newBlock = block.get();
        }
        ASSERT_NE(newBlock, nullptr);
        bool protectedSeen = false;
        for (const auto* pb : region.protectedBlocks) {
            if (pb == newBlock) protectedSeen = true;
        }
        EXPECT_TRUE(protectedSeen)
            << "new block b" << id << " must be registered as protected (#1188)";
        bool edgeSeen      = false;
        bool argsMatch     = false;
        for (const auto& edge : region.edges) {
            if (edge.protectedBlock != newBlock) continue;
            edgeSeen  = true;
            argsMatch = edge.handlerArgs == hostArgs;
        }
        EXPECT_TRUE(edgeSeen) << "new block b" << id << " must carry an exception edge";
        EXPECT_TRUE(argsMatch) << "new block edge handlerArgs must equal call-block edge";
    }
    EXPECT_GT(newBlocks, 0U) << "inlining must have appended blocks";
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

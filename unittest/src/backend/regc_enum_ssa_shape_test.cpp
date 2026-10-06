// Enums 承接卡（#887）SSA 形状谓词直测 + 内联排除门计数（评审 nit N4 收尾；
// #1192 评审 N4 承接：valueOf SSA 谓词死路径删除，方案 B）。
//
// 覆盖面（当时仅产物级验证：转换日志行 + 生成 C，本卡补符号级直测）：
//   1. isJavacEnumValuesSsa 形状判定——正例钉接受形状，反例钉拒绝边界
//      （同形状手工方法 / 缺一指令 / 非 clone owner / 签名门）。
//   2. javac valueOf 无 SSA 谓词的 lowering 事实钉——body 块恰为
//      [Call, Convert] 2 条（ldc/aload 不落 SSA 指令），valueOf 合成验证
//      唯一来源是字节码侧 isJavacEnumValueOfBody 预扫描/按类补扫。
//   3. SsaInlineOptions.excludeMethod + SsaInlineStats.skippedExcluded——pass 层
//      可观测面（返回结构 SsaInlineStats，仓内 ssa_inline_test 同惯例）。
//
// 红相论证（每 TEST 注释单列）：删谓词 / 改判定条件时哪个断言转红。
// 正例经真实管线（CodeConverter → SSABuilder）构造；valueOf 的 lowering 事实
// 见 ValueOfJavacShapeLowersToCallConvertPair（converter 会丢 ldc/aload）。
// 注意：MethodNode 的 name/desc 是非持有 Utf8StringRef，全部 fixture 只传静态
// 字面量，不造临时 std::string。
#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter_internal.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/passes/ssa_inline_passes.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_function.hpp"

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

/// CodeConverter + SSABuilder 构建 SSA（owner 可指定：谓词校验 field/call owner
/// 与 fn.symbol.owner 绑定，不能像 ssa_inline_test 那样统一 "InlineTest"）。
/// converter 必须活到 build 完成（Block 归 converter 所有）。
std::unique_ptr<SSAFunction> buildSsa(MethodNode& mn, const char* owner, Utf8StringArena& arena,
                                      std::string& reason)
{
    CodeConverter converter(arena);
    auto          blocks = converter.convert(mn);
    SSABuilder    builder;
    const bool    isStatic = (mn.access & Opcodes::ACC_STATIC) != 0;
    return builder.build(blocks, owner, mn.name.to_std_string(), mn.desc.to_std_string(),
                         isStatic, mn.maxLocals, arena, reason);
}

/// javac values() 形状方法体：getstatic $VALUES → clone → checkcast → areturn。
/// desc 与 arrayDesc 分开传（全静态字面量，无临时串）。
std::unique_ptr<MethodNode> makeValuesMethod(const char* fieldOwner, const char* desc,
                                             const char* arrayDesc, const char* cloneOwner)
{
    return makeMethod(kStaticPublic, "values", desc,
                      {new FieldInsnNode(Opcodes::GETSTATIC,
                                         ca::str::Utf8StringRef::from_cstr(fieldOwner),
                                         ca::str::Utf8StringRef::from_cstr("$VALUES"),
                                         ca::str::Utf8StringRef::from_cstr(arrayDesc)),
                       new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                          ca::str::Utf8StringRef::from_cstr(cloneOwner),
                                          ca::str::Utf8StringRef::from_cstr("clone"),
                                          ca::str::Utf8StringRef::from_cstr("()Ljava/lang/Object;"),
                                          false),
                       new TypeInsnNode(Opcodes::CHECKCAST,
                                        ca::str::Utf8StringRef::from_cstr(arrayDesc)),
                       new InsnNode(Opcodes::ARETURN)},
                      1, 0);
}

/// INVOKESTATIC Color.values ()[LColor; 调用点。
MethodInsnNode* invokeStaticValues()
{
    return new MethodInsnNode(Opcodes::INVOKESTATIC,
                              ca::str::Utf8StringRef::from_cstr("Color"),
                              ca::str::Utf8StringRef::from_cstr("values"),
                              ca::str::Utf8StringRef::from_cstr("()[LColor;"), false);
}

/// 统计函数内 Call 指令数（ssa_inline_test 同款）。
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

/// 内联后的 SSA 必须能降级为 RegIR 且无栈槽（ssa_inline_test 同款）。
bool lowerOk(const SSAFunction& fn, std::string& reason)
{
    RegLowering lowering;
    auto        reg = lowering.lower(fn, reason);
    if (reg == nullptr) return false;
    return validateRegIRNoStackSlots(*reg, reason);
}

// ---- 手工 SSAFunction（不经 converter）：钉谓词对块内指令编排的边界 ----

struct HandFn
{
    std::unique_ptr<SSAFunction> fn;
    SSABlock*                    body = nullptr;
};

HandFn makeHandFn(const char* owner, const char* name, const char* desc)
{
    HandFn       h;
    h.fn         = std::make_unique<SSAFunction>();
    h.fn->symbol = SSAFunctionSymbol{owner, name, desc, {}, SSAType::UNKNOWN};
    h.fn->blocks.push_back(std::make_unique<SSABlock>(h.fn->ids.newBlockId()));
    h.body       = h.fn->blocks.back().get();
    return h;
}

SSALoadFieldInstruction* appendLoadField(HandFn& h, const char* owner, const char* name,
                                         const char* desc)
{
    auto ins            = std::make_unique<SSALoadFieldInstruction>();
    ins->field.owner    = ca::str::Utf8StringRef::from_cstr(owner);
    ins->field.name     = ca::str::Utf8StringRef::from_cstr(name);
    ins->field.desc     = ca::str::Utf8StringRef::from_cstr(desc);
    ins->field.isStatic = true;
    auto* raw           = ins.get();
    h.body->instructions.push_back(std::move(ins));
    return raw;
}

SSACallInstruction* appendCall(HandFn& h, const char* owner, const char* name, const char* desc,
                               int invokeOpcode)
{
    auto ins       = std::make_unique<SSACallInstruction>();
    ins->method    = SSAMethodRef{ca::str::Utf8StringRef::from_cstr(owner),
                                  ca::str::Utf8StringRef::from_cstr(name),
                                  ca::str::Utf8StringRef::from_cstr(desc), invokeOpcode};
    ins->result    = SSAInstructionResult(h.fn->ids.newValueId(), SSAType::REF);
    ins->hasResult = true;
    auto* raw      = ins.get();
    h.body->instructions.push_back(std::move(ins));
    return raw;
}

SSAConvertInstruction* appendCheckCast(HandFn& h, const char* desc, SSAValue* value)
{
    auto ins               = std::make_unique<SSAConvertInstruction>();
    ins->convertKind       = SSAConvertKind::ReferenceCast;
    ins->value             = value;
    ins->targetType        = SSAType::REF;
    ins->referenceTypeDesc = ca::str::Utf8StringRef::from_cstr(desc);
    ins->result            = SSAInstructionResult(h.fn->ids.newValueId(), SSAType::REF);
    ins->hasResult         = true;
    auto* raw              = ins.get();
    h.body->instructions.push_back(std::move(ins));
    return raw;
}

SSAIntrinsicInstruction* appendIntrinsic(HandFn& h, const char* tag)
{
    auto  ins  = std::make_unique<SSAIntrinsicInstruction>();
    ins->name  = tag;
    auto* raw  = ins.get();
    h.body->instructions.push_back(std::move(ins));
    return raw;
}

void finishReturn(HandFn& h, SSAValue* value)
{
    h.body->terminator = SSAReturnTerminator{value};
}

/// 追加一个空块（模拟 SSABuilder 可能生成的空 entry 块）。
void appendEmptyBlock(HandFn& h)
{
    h.fn->blocks.push_back(std::make_unique<SSABlock>(h.fn->ids.newBlockId()));
}

/// 手工 values() 形状三指令：LoadField($VALUES) → Call(clone) → Convert(checkcast)，
/// 返回 ret 引用的值（cast 结果）。
SSAValue* appendValuesShape(HandFn& h, const char* fieldOwner, const char* arrayDesc,
                            const char* cloneOwner)
{
    appendLoadField(h, fieldOwner, "$VALUES", arrayDesc);
    SSACallInstruction* clone =
        appendCall(h, cloneOwner, "clone", "()Ljava/lang/Object;", Opcodes::INVOKEVIRTUAL);
    SSAConvertInstruction* cast = appendCheckCast(h, arrayDesc, &clone->result);
    return &cast->result;
}

/// 谓词接受块诊断：逐块指令数（失败消息用）。
std::string blockShape(const SSAFunction& fn)
{
    std::string s = "[";
    for (const auto& b : fn.blocks) {
        s += std::to_string(b->instructions.size());
        s += " ";
    }
    s += "]";
    return s;
}

}   // namespace

// ---------------------------------------------------------------------------
// isJavacEnumValuesSsa：正例
// ---------------------------------------------------------------------------

// 红相：删谓词或改 name/desc 门（values / ()[L 前缀 / 尾分号）任一 → 转红。
TEST(RegCEnumSsaShapeTest, ValuesJavacShapeAccepted)
{
    Utf8StringArena arena;
    std::string     reason;
    // javac 形态：clone owner = 数组描述符。
    auto mn  = makeValuesMethod("Color", "()[LColor;", "[LColor;", "[LColor;");
    auto ssa = buildSsa(*mn, "Color", arena, reason);
    ASSERT_NE(ssa, nullptr) << reason;
    EXPECT_TRUE(isJavacEnumValuesSsa(*ssa))
        << "javac values() shape must be accepted; blocks=" << blockShape(*ssa);
}

// 红相：删 clone owner 白名单的 java/lang/Object 分支 → 转红（钉「双收」口径）。
TEST(RegCEnumSsaShapeTest, ValuesCloneOwnerObjectAccepted)
{
    Utf8StringArena arena;
    std::string     reason;
    // clone owner = java/lang/Object（手工/旧编译器形态，字节码侧同样双收）。
    auto mn  = makeValuesMethod("Color", "()[LColor;", "[LColor;", "java/lang/Object");
    auto ssa = buildSsa(*mn, "Color", arena, reason);
    ASSERT_NE(ssa, nullptr) << reason;
    EXPECT_TRUE(isJavacEnumValuesSsa(*ssa))
        << "Object.clone owner must be accepted; blocks=" << blockShape(*ssa);
}

// 红相：删「按块搜索」（改为只看首块）→ 空 entry 块场景转红（钉空 entry 容忍）。
TEST(RegCEnumSsaShapeTest, ValuesToleratesEmptyEntryBlock)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    appendEmptyBlock(h);   // 空块在前（SSABuilder 可能生成的空 entry）
    SSAValue* ret = appendValuesShape(h, "Color", "[LColor;", "[LColor;");
    finishReturn(h, ret);
    EXPECT_TRUE(isJavacEnumValuesSsa(*h.fn));
}

// ---------------------------------------------------------------------------
// isJavacEnumValuesSsa：反例
// ---------------------------------------------------------------------------

// 红相：把「clone owner == 数组描述符或 java/lang/Object」放宽为任意 owner → 转红。
TEST(RegCEnumSsaShapeTest, ValuesForeignCloneOwnerRejected)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    // clone owner = 枚举类自身（非数组描述符、非 java/lang/Object）。
    SSAValue* ret = appendValuesShape(h, "Color", "[LColor;", "Color");
    finishReturn(h, ret);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：删 field.owner == fn.symbol.owner 绑定 → 同形状手工方法（委托他类
// $VALUES）转红——该绑定是「合成方法」与手工同名方法的分界。
TEST(RegCEnumSsaShapeTest, ValuesHandRolledFieldOwnerMismatchRejected)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    // 同形状手工方法：其余三指令全对，唯 getstatic 的 field owner 是他类。
    SSAValue* ret = appendValuesShape(h, "Other", "[LColor;", "[LColor;");
    finishReturn(h, ret);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：2 指令块（缺 checkcast）先被 size 门（同块 ≠ 3 指令）拒绝——单个 kind
// 检查的删除不改变本例结果（2 指令撞不到 kind 门），故本例钉的是 size 门完整性；
// 各 kind 检查由其余同形状例分钉（评审 P3-nit① 归属勘正）。缺 checkcast 的
// 展开体语义已破，保留作形状文档。
TEST(RegCEnumSsaShapeTest, ValuesMissingCheckCastRejected)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    // 缺 checkcast：getstatic + clone + areturn（块仅 2 指令）。
    appendLoadField(h, "Color", "$VALUES", "[LColor;");
    SSACallInstruction* clone = appendCall(h, "[LColor;", "clone", "()Ljava/lang/Object;",
                                           Opcodes::INVOKEVIRTUAL);
    finishReturn(h, &clone->result);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：放宽「同块 3 指令」size 门为 ≥3 或忽略多余指令 → 转红。
TEST(RegCEnumSsaShapeTest, ValuesExtraInstructionRejected)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    // 4 指令块：首部多一条任意指令（此处 Intrinsic）→ 形状不再唯一。
    appendIntrinsic(h, "noise");
    SSAValue* ret = appendValuesShape(h, "Color", "[LColor;", "[LColor;");
    finishReturn(h, ret);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：删 $VALUES 字段名比对 → 本测试转红。
TEST(RegCEnumSsaShapeTest, ValuesWrongFieldNameRejected)
{
    HandFn h = makeHandFn("Color", "values", "()[LColor;");
    // field name 不是 $VALUES（同形状、同 owner，仅字段名不同）。
    appendLoadField(h, "Color", "BACKING", "[LColor;");
    SSACallInstruction* clone = appendCall(h, "[LColor;", "clone", "()Ljava/lang/Object;",
                                           Opcodes::INVOKEVIRTUAL);
    SSAConvertInstruction* cast = appendCheckCast(h, "[LColor;", &clone->result);
    finishReturn(h, &cast->result);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：删 name 签名门（方法名前置校验）→ 本测试转红。
TEST(RegCEnumSsaShapeTest, ValuesWrongNameRejected)
{
    HandFn h = makeHandFn("Color", "valuesX", "()[LColor;");
    // 方法名不是 values，形状指令全对。
    SSAValue* ret = appendValuesShape(h, "Color", "[LColor;", "[LColor;");
    finishReturn(h, ret);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// 红相：删 desc 门（()[L 前缀 + 尾分号）→ 本测试转红。desc 取 "X)[LColor;" 使
// substr(2) 恰为 "[LColor;"（与 field desc 一致、只漏前缀门）——若取 "(IL..."
// 类非数组 desc，删门后仍被 field desc 失配拒绝，钉不住前缀门（评审 P3-nit② 勘正）。
TEST(RegCEnumSsaShapeTest, ValuesWrongDescRejected)
{
    HandFn h = makeHandFn("Color", "values", "X)[LColor;");  // 过数组形态、仅前缀门可拒
    SSAValue* ret = appendValuesShape(h, "Color", "[LColor;", "[LColor;");
    finishReturn(h, ret);
    EXPECT_FALSE(isJavacEnumValuesSsa(*h.fn));
}

// ---------------------------------------------------------------------------
// javac valueOf(String) 的 SSA lowering 事实钉（#1192 评审 N4 承接，方案 B）
// ---------------------------------------------------------------------------

// valueOf 无 SSA 形状谓词、无 SSA 自登记（谓词已删）：真实 javac valueOf 经
// 真实管线（CodeConverter → SSABuilder）后 ldc 类字面量与 aload#0 都不落 SSA
// 指令（分别降为 opaque 值与局部值，ssa_builder.cpp lowerConstant/
// lowerLocalLoad），body 块恰为 [Call(Enum.valueOf), Convert(checkcast owner)]
// 两条——任何 SSA 侧形状门都查不到 ldc desc == owner 这一 owner 绑定证据，
// 只能造出弱于字节码门的合同（2 指令口径会放行 Enum.valueOf(他类字面量, s) +
// checkcast 本类的病态形状并注入站点 intrinsic 信任集合，fail-open）。
// valueOf 合成验证唯一来源 = 字节码侧 isJavacEnumValueOfBody 预扫描/按类补扫
// （与 lowering 无关、时序严格先于本方法 SSA 构建）。
// 红相：lowering 演进使 ldc/aload 落为 SSA 指令（body 块 ≠ 2 条）→ 本测试
// 转红，即重估 SSA 侧 valueOf 验证可行性的时点；重新引入 valueOf SSA 形状门
// （无论 2/3 指令口径）前必须先以本钉校准 lowering 事实。
TEST(RegCEnumSsaShapeTest, ValueOfJavacShapeLowersToCallConvertPair)
{
    Utf8StringArena arena;
    std::string     reason;
    auto mn = makeMethod(kStaticPublic, "valueOf", "(Ljava/lang/String;)LColor;",
                         {new LdcInsnNode(Type::getObjectType(arena.intern("Color"))),
                          new VarInsnNode(Opcodes::ALOAD, 0),
                          new MethodInsnNode(
                              Opcodes::INVOKESTATIC,
                              ca::str::Utf8StringRef::from_cstr("java/lang/Enum"),
                              ca::str::Utf8StringRef::from_cstr("valueOf"),
                              ca::str::Utf8StringRef::from_cstr(
                                  "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;"),
                              false),
                          new TypeInsnNode(Opcodes::CHECKCAST,
                                           ca::str::Utf8StringRef::from_cstr("Color")),
                          new InsnNode(Opcodes::ARETURN)},
                         2, 1);
    auto ssa = buildSsa(*mn, "Color", arena, reason);
    ASSERT_NE(ssa, nullptr) << reason;

    // 定位非空 body 块（SSABuilder 可能另生成空 entry 块），钉其恰为 2 指令。
    const SSABlock* body = nullptr;
    for (const auto& block : ssa->blocks) {
        if (!block->instructions.empty()) {
            ASSERT_EQ(body, nullptr) << "multiple non-empty blocks; blocks="
                                     << blockShape(*ssa);
            body = block.get();
        }
    }
    ASSERT_NE(body, nullptr) << "no non-empty block; blocks=" << blockShape(*ssa);
    ASSERT_EQ(body->instructions.size(), 2U)
        << "lowering must keep dropping ldc/aload for javac valueOf; blocks="
        << blockShape(*ssa);
    ASSERT_EQ(body->instructions[0]->kind, SSAInstructionKind::Call);
    const auto* invoke = static_cast<const SSACallInstruction*>(body->instructions[0].get());
    EXPECT_EQ(invoke->method.owner, "java/lang/Enum");
    EXPECT_EQ(invoke->method.name, "valueOf");
    EXPECT_EQ(invoke->method.desc,
              "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;");
    ASSERT_EQ(body->instructions[1]->kind, SSAInstructionKind::Convert);
    const auto* cast = static_cast<const SSAConvertInstruction*>(body->instructions[1].get());
    EXPECT_EQ(cast->convertKind, SSAConvertKind::ReferenceCast);
    EXPECT_EQ(cast->referenceTypeDesc, "Color");
    const auto* ret = std::get_if<SSAReturnTerminator>(&body->terminator);
    ASSERT_NE(ret, nullptr) << "body block must return";
    EXPECT_EQ(ret->value, cast->hasResult ? &cast->result : nullptr);
}

// ---------------------------------------------------------------------------
// excludeMethod / skippedExcluded（pass 层可观测面：SsaInlineStats 返回结构，
// ssa_inline_test 同惯例）
// ---------------------------------------------------------------------------

// 红相：删 pass 侧排除检查（安全条件 0.5，ssa_inline_passes.cpp）或
// skippedExcluded 递增 → 本测试转红；后端 excludeMethod 接线
// （regc_backend.cpp inlineOptions）由后端自身职责，此处钉 pass 契约。
TEST(RegCEnumSsaShapeTest, ExcludedSyntheticValuesNotInlined)
{
    Utf8StringArena arena;
    auto callee = makeValuesMethod("Color", "()[LColor;", "[LColor;", "[LColor;");
    // caller: static Color[] f() { return Color.values(); }
    auto caller = makeMethod(kStaticPublic, "f", "()[LColor;",
                             {invokeStaticValues(), new InsnNode(Opcodes::ARETURN)}, 1, 0);

    std::string reason;
    auto        callerSsa = buildSsa(*caller, "Caller", arena, reason);
    ASSERT_NE(callerSsa, nullptr) << reason;
    ASSERT_EQ(countCalls(*callerSsa), 1U);

    const auto resolver = [&](const SSAMethodRef& ref,
                              std::string& /*failReason*/) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "values" || ref.desc != "()[LColor;") return nullptr;
        return buildSsa(*callee, "Color", arena, reason);
    };
    SsaInlineOptions options;
    options.excludeMethod = [](const SSAMethodRef& ref) -> bool {
        return ref.name == "values" && ref.desc == "()[LColor;";
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, options, resolver, arena);

    EXPECT_EQ(stats.skippedExcluded, 1U) << "excluded call site must be counted";
    EXPECT_EQ(stats.inlinedCalls, 0U);
    EXPECT_EQ(countCalls(*callerSsa), 1U) << "excluded call must remain un-inlined";
}

// 对照：谓词不命中 → 行为与排除门加入前一致（正常内联、计数 0）。
// 红相：排除门误伤未命中调用点（谓词后误置 skip）→ 转红。
TEST(RegCEnumSsaShapeTest, PredicateMissStillInlines)
{
    Utf8StringArena arena;
    auto callee = makeValuesMethod("Color", "()[LColor;", "[LColor;", "[LColor;");
    auto caller = makeMethod(kStaticPublic, "f", "()[LColor;",
                             {invokeStaticValues(), new InsnNode(Opcodes::ARETURN)}, 1, 0);

    std::string reason;
    auto        callerSsa = buildSsa(*caller, "Caller", arena, reason);
    ASSERT_NE(callerSsa, nullptr) << reason;
    ASSERT_EQ(countCalls(*callerSsa), 1U);

    const auto resolver = [&](const SSAMethodRef& ref,
                              std::string& /*failReason*/) -> std::unique_ptr<SSAFunction> {
        if (ref.name != "values" || ref.desc != "()[LColor;") return nullptr;
        return buildSsa(*callee, "Color", arena, reason);
    };
    SsaInlineOptions options;
    options.excludeMethod = [](const SSAMethodRef& ref) -> bool {
        (void)ref;
        return false;   // 不命中：不排除
    };

    SsaInlineStats stats = applySsaInlining(*callerSsa, options, resolver, arena);

    EXPECT_EQ(stats.skippedExcluded, 0U);
    EXPECT_EQ(stats.inlinedCalls, 1U);
    // 展开体自带 clone 调用（Call 指令），总数为 1；原 values 调用点必须已消失。
    EXPECT_EQ(countCalls(*callerSsa), 1U);
    bool sawValuesCall = false;
    for (const auto& block : callerSsa->blocks) {
        for (const auto& ins : block->instructions) {
            if (ins->kind == SSAInstructionKind::Call &&
                static_cast<const SSACallInstruction*>(ins.get())->method.name ==
                    "values") {
                sawValuesCall = true;
            }
        }
    }
    EXPECT_FALSE(sawValuesCall) << "values call site must be replaced by inlined body";
    EXPECT_TRUE(lowerOk(*callerSsa, reason)) << reason;
}

#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <memory>
#include <limits>
#include <string>
#include <unordered_map>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
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

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const std::string& name, const std::string& desc,
                                       std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                       int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto                                mn = std::make_unique<MethodNode>(access,
                                           ca::str::Utf8StringRef::from_cstr(name.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(desc.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack                           = maxStack;
    mn->maxLocals                          = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

// 注意：arena 必须活到 emitFunction 返回——MIR/SSA/RegIR 节点字段是 arena-backed
// Utf8StringRef（spec §6.1），arena 提前析构会让 emit 读到悬垂指针（参见 #698）。
// 生产侧 pipeline 把 arena 放在 CompilerIndex（job 级寿命）；测试侧把 arena 与 emit
// 放同一作用域，对齐该寿命约定。
RegCEmitResult emitRegC(const std::string& name, const std::string& desc,
                        std::initializer_list<AbstractInsnNode*> insns, int maxStack, int maxLocals,
                        std::string& reason,
                        codegen::DirectCallTier tier = codegen::DirectCallTier::Off)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineRegC", name, desc, true, maxLocals, arena, reason);
    if (!ssa) {
        RegCEmitResult result;
        result.reason = reason;
        return result;
    }

    RegLowering   lowering;
    auto          reg = lowering.lower(*ssa, reason);
    if (!reg) {
        RegCEmitResult result;
        result.reason = reason;
        return result;
    }

    codegen::CodeOptions options;
    options.directCallTier = tier;
    RegCEmitter emitter(nullptr, options);
    return emitter.emitFunction(*reg);
}

void expectNoStackAnchors(const std::string& source)
{
    std::string reason;
    EXPECT_TRUE(validateRegCNoStackAnchors(source, reason)) << reason << "\n" << source;
    EXPECT_EQ(source.find("cstack["), std::string::npos) << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
    EXPECT_EQ(source.find("Cannot invoke"), std::string::npos) << source;
}

size_t countSubstring(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

}   // namespace

TEST(RegCTest, EmitsPureArithmeticWithoutStackMachineAnchors)
{
    std::string reason;
    auto        result = emitRegC("addMul",
                           "(II)I",
                                  {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::IADD),
                               new InsnNode(Opcodes::ICONST_3),
                               new InsnNode(Opcodes::IMUL),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           3,
                           2,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("static int32_t regc_InlineRegC_addMul__II_I"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("int32_t r0"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("uint32_t"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_result = r"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("goto _mj2x_regc_cleanup;"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("return _mj2x_regc_result;"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsBitExactFloatingLiterals)
{
    std::string reason;
    auto        result = emitRegC("floatingLiterals",
                           "()D",
                           {
                               new LdcInsnNode(-0.0f),
                               new InsnNode(Opcodes::F2D),
                               new InsnNode(Opcodes::DRETURN),
                           },
                           1,
                           0,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_f32_from_bits((uint32_t)0x80000000u)"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("-0f"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsBitExactNanLiteral)
{
    std::string reason;
    auto        result = emitRegC("nanLiteral",
                           "()D",
                           {
                               new LdcInsnNode(std::numeric_limits<double>::quiet_NaN()),
                               new InsnNode(Opcodes::DRETURN),
                           },
                           2,
                           0,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_f64_from_bits((uint64_t)0x7ff8000000000000ULL)"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find(" nan"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsLoopControlFlowWithEdgeCopies)
{
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());

    std::string reason;
    auto        result = emitRegC("sum",
                           "(I)I",
                                  {
                               new InsnNode(Opcodes::ICONST_0),
                               new VarInsnNode(Opcodes::ISTORE, 1),
                               new InsnNode(Opcodes::ICONST_0),
                               new VarInsnNode(Opcodes::ISTORE, 2),
                               loop,
                               new VarInsnNode(Opcodes::ILOAD, 2),
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new JumpInsnNode(Opcodes::IF_ICMPGE, done->label),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new VarInsnNode(Opcodes::ILOAD, 2),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ISTORE, 1),
                               new IincInsnNode(2, 1),
                               new JumpInsnNode(Opcodes::GOTO, loop->label),
                               done,
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           3,
                           3,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("goto block_b"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("edge_"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("if (r"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsSwitchControlFlow)
{
    auto* case0 = new LabelNode(labelScratchArena().create<Label>());
    auto* case1 = new LabelNode(labelScratchArena().create<Label>());
    auto* def   = new LabelNode(labelScratchArena().create<Label>());

    std::string reason;
    auto        result =
        emitRegC("pick",
                 "(I)I",
                 {
                     new VarInsnNode(Opcodes::ILOAD, 0),
                     new TableSwitchInsnNode(0, 1, def->label, {case0->label, case1->label}),
                     case0,
                     new IntInsnNode(Opcodes::BIPUSH, 7),
                     new InsnNode(Opcodes::IRETURN),
                     case1,
                     new IntInsnNode(Opcodes::BIPUSH, 9),
                     new InsnNode(Opcodes::IRETURN),
                     def,
                     new InsnNode(Opcodes::ICONST_M1),
                     new InsnNode(Opcodes::IRETURN),
                 },
                 2,
                 1,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("switch (r0)"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("case 0"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("default"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsArrayHelpersWithoutStackMachineAnchors)
{
    std::string reason;
    auto        result = emitRegC("arrayOps",
                           "()I",
                                  {
                               new InsnNode(Opcodes::ICONST_2),
                               new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_INT),
                               new InsnNode(Opcodes::DUP),
                               new InsnNode(Opcodes::ICONST_0),
                               new InsnNode(Opcodes::ICONST_5),
                               new InsnNode(Opcodes::IASTORE),
                               new InsnNode(Opcodes::DUP),
                               new InsnNode(Opcodes::ARRAYLENGTH),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           4,
                           0,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_alloc_array(\"[I\""), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_i32_array"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_array_length"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsMultiArrayHelperWithoutStackMachineAnchors)
{
    std::string reason;
    auto        result =
        emitRegC("multiArray",
                 "(II)[[I",
                 {
                     new VarInsnNode(Opcodes::ILOAD, 0),
                     new VarInsnNode(Opcodes::ILOAD, 1),
                     new MultiANewArrayInsnNode(ca::str::Utf8StringRef::from_cstr("[[I"), 2),
                     new InsnNode(Opcodes::ARETURN),
                 },
                 2,
                 2,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("int32_t dims[2]"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_alloc_multi_array(\"[[I\", 2, dims)"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsObjectFieldAndCallHelpers)
{
    std::string reason;
    auto        result =
        emitRegC("objectOps",
                 "(I)I",
                 {
                     new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("Foo")),
                     new InsnNode(Opcodes::DUP),
                     new VarInsnNode(Opcodes::ILOAD, 0),
                     new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                        ca::str::Utf8StringRef::from_cstr("Foo"),
                                        ca::str::Utf8StringRef::from_cstr("<init>"),
                                        ca::str::Utf8StringRef::from_cstr("(I)V"),
                                        false),
                     new InsnNode(Opcodes::DUP),
                     new FieldInsnNode(Opcodes::GETFIELD,
                                       ca::str::Utf8StringRef::from_cstr("Foo"),
                                       ca::str::Utf8StringRef::from_cstr("x"),
                                       ca::str::Utf8StringRef::from_cstr("I")),
                     new FieldInsnNode(Opcodes::PUTSTATIC,
                                       ca::str::Utf8StringRef::from_cstr("Foo"),
                                       ca::str::Utf8StringRef::from_cstr("sink"),
                                       ca::str::Utf8StringRef::from_cstr("I")),
                     new FieldInsnNode(Opcodes::GETSTATIC,
                                       ca::str::Utf8StringRef::from_cstr("Foo"),
                                       ca::str::Utf8StringRef::from_cstr("sink"),
                                       ca::str::Utf8StringRef::from_cstr("I")),
                     new MethodInsnNode(Opcodes::INVOKESTATIC,
                                        ca::str::Utf8StringRef::from_cstr("Foo"),
                                        ca::str::Utf8StringRef::from_cstr("mix"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I"),
                                        false),
                     new InsnNode(Opcodes::IRETURN),
                 },
                 4,
                 1,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_alloc_object(\"Foo\")"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_call_void(\"Foo\", \"<init>\", \"(I)V\""),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_i32_field"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_regc_static_field_cache"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("{\"Foo\", \"sink\", \"I\", NULL, NULL}"), std::string::npos)
        << result.source;
    EXPECT_EQ(countSubstring(result.source, "{\"Foo\", \"sink\", \"I\", NULL, NULL}"), 1u)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_cached_i32_field(&_mj2x_regc_field_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_cached_i32_field(&_mj2x_regc_field_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_store_i32_field(1, NULL"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("{\"Foo\", \"mix\", \"(I)I\", NULL, NULL}"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_cached_static_method(&_mj2x_regc_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"Foo\", \"mix\", \"(I)I\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, InternsRepeatedStaticMethodAndFieldCachesWithinFunction)
{
    std::string reason;
    auto        result = emitRegC("cacheCse",
                           "()I",
                                  {
                               new FieldInsnNode(Opcodes::GETSTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("sink"),
                                                 ca::str::Utf8StringRef::from_cstr("I")),
                               new InsnNode(Opcodes::POP),
                               new InsnNode(Opcodes::ICONST_1),
                               new FieldInsnNode(Opcodes::PUTSTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("sink"),
                                                 ca::str::Utf8StringRef::from_cstr("I")),
                               new FieldInsnNode(Opcodes::GETSTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("sink"),
                                                 ca::str::Utf8StringRef::from_cstr("I")),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  ca::str::Utf8StringRef::from_cstr("Foo"),
                                                  ca::str::Utf8StringRef::from_cstr("mix"),
                                                  ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                  false),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  ca::str::Utf8StringRef::from_cstr("Foo"),
                                                  ca::str::Utf8StringRef::from_cstr("mix"),
                                                  ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                  false),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           1,
                           0,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(countSubstring(result.source, "{\"Foo\", \"sink\", \"I\", NULL, NULL}"), 1u)
        << result.source;
    EXPECT_EQ(countSubstring(result.source, "{\"Foo\", \"mix\", \"(I)I\", NULL, NULL}"), 1u)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_cached_i32_field(&_mj2x_regc_field_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_cached_i32_field(&_mj2x_regc_field_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_cached_static_method(&_mj2x_regc_cache_"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsSelfStaticCallAsDirectCDispatch)
{
    // 自递归直调仅在 All 档发射（默认 Off 保持 JVM 栈帧语义）。
    std::string reason;
    auto        result = emitRegC("self",
                           "(I)I",
                                  {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new InsnNode(Opcodes::ICONST_1),
                               new InsnNode(Opcodes::ISUB),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  ca::str::Utf8StringRef::from_cstr("InlineRegC"),
                                                  ca::str::Utf8StringRef::from_cstr("self"),
                                                  ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                  false),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           2,
                           1,
                           reason,
                           codegen::DirectCallTier::All);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("regc_InlineRegC_self__I_I((int32_t)("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_cached_i32"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsJniCallForSelfStaticByDefault)
{
    // 默认（Off 档）时自递归走 cached JNI（保 JVM 栈帧语义），
    // 不发射 regc_* 直调（issue #814 regc 2.6 ReTrace）。
    std::string reason;
    auto        result = emitRegC("self",
                           "(I)I",
                                  {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new InsnNode(Opcodes::ICONST_1),
                               new InsnNode(Opcodes::ISUB),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  ca::str::Utf8StringRef::from_cstr("InlineRegC"),
                                                  ca::str::Utf8StringRef::from_cstr("self"),
                                                  ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                  false),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           2,
                           1,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(result.source.find("regc_InlineRegC_self__I_I((int32_t)("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_call_cached_i32"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsNarrowPrimitiveFieldAndArrayHelpers)
{
    std::string reason;
    auto        result = emitRegC("narrow",
                           "(LFoo;I)I",
                                  {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new FieldInsnNode(Opcodes::PUTFIELD,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("b"),
                                                 ca::str::Utf8StringRef::from_cstr("B")),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new FieldInsnNode(Opcodes::PUTFIELD,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("s"),
                                                 ca::str::Utf8StringRef::from_cstr("S")),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new FieldInsnNode(Opcodes::PUTFIELD,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("c"),
                                                 ca::str::Utf8StringRef::from_cstr("C")),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new FieldInsnNode(Opcodes::PUTFIELD,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("z"),
                                                 ca::str::Utf8StringRef::from_cstr("Z")),
                               new InsnNode(Opcodes::ICONST_1),
                               new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_BYTE),
                               new InsnNode(Opcodes::DUP),
                               new InsnNode(Opcodes::ICONST_0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::BASTORE),
                               new InsnNode(Opcodes::ICONST_0),
                               new InsnNode(Opcodes::BALOAD),
                               new InsnNode(Opcodes::POP),
                               new InsnNode(Opcodes::ICONST_1),
                               new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_CHAR),
                               new InsnNode(Opcodes::DUP),
                               new InsnNode(Opcodes::ICONST_0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::CASTORE),
                               new InsnNode(Opcodes::ICONST_0),
                               new InsnNode(Opcodes::CALOAD),
                               new InsnNode(Opcodes::POP),
                               new InsnNode(Opcodes::ICONST_1),
                               new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_SHORT),
                               new InsnNode(Opcodes::DUP),
                               new InsnNode(Opcodes::ICONST_0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::SASTORE),
                               new InsnNode(Opcodes::ICONST_0),
                               new InsnNode(Opcodes::SALOAD),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new FieldInsnNode(Opcodes::GETFIELD,
                                                 ca::str::Utf8StringRef::from_cstr("Foo"),
                                                 ca::str::Utf8StringRef::from_cstr("b"),
                                                 ca::str::Utf8StringRef::from_cstr("B")),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           4,
                           2,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_store_i8_field(0,"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_i16_field(0,"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_char_field(0,"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_bool_field(0,"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_i8_field(0,"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_i8_array"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_i8_array"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_char_array"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_char_array"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_store_i16_array"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_load_i16_array"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsReferenceCheckAndMonitorHelpers)
{
    std::string reason;
    auto        result =
        emitRegC("checks",
                 "(Ljava/lang/Object;)I",
                 {
                     new VarInsnNode(Opcodes::ALOAD, 0),
                     new TypeInsnNode(Opcodes::CHECKCAST,
                                      ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                     new InsnNode(Opcodes::DUP),
                     new InsnNode(Opcodes::MONITORENTER),
                     new InsnNode(Opcodes::DUP),
                     new InsnNode(Opcodes::MONITOREXIT),
                     new TypeInsnNode(Opcodes::INSTANCEOF,
                                      ca::str::Utf8StringRef::from_cstr("java/lang/CharSequence")),
                     new InsnNode(Opcodes::IRETURN),
                 },
                 2,
                 1,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_checkcast_cached"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_monitor_enter"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_monitor_exit"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_instanceof_cached"), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

/// monitorenter/monitorexit 的对象为 NULL 时必须抛 NPE（JVMS 语义）：守卫
/// （_mj2x_regc_throw_null）必须先于 MonitorEnter/MonitorExit JNI 调用发射，
/// 否则 NULL jobject 递进 JNI 是未定义行为（HotSpot 按"调用方保证非空"处理）。
TEST(RegCTest, MonitorNullOperandThrowsNpe)
{
    std::string reason;
    auto        result =
        emitRegC("monitorNull",
                 "(Ljava/lang/Object;)V",
                 {
                     new VarInsnNode(Opcodes::ALOAD, 0),
                     new InsnNode(Opcodes::MONITORENTER),
                     new VarInsnNode(Opcodes::ALOAD, 0),
                     new InsnNode(Opcodes::MONITOREXIT),
                     new InsnNode(Opcodes::RETURN),
                 },
                 2,
                 1,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    const auto firstGuard = result.source.find("_mj2x_regc_throw_null(\"monitor\")");
    const auto enter      = result.source.find("_mj2x_regc_monitor_enter(");
    ASSERT_NE(firstGuard, std::string::npos) << result.source;
    ASSERT_NE(enter, std::string::npos) << result.source;
    const auto secondGuard =
        result.source.find("_mj2x_regc_throw_null(\"monitor\")", firstGuard + 1);
    const auto exitCall = result.source.find("_mj2x_regc_monitor_exit(");
    ASSERT_NE(secondGuard, std::string::npos) << result.source;
    ASSERT_NE(exitCall, std::string::npos) << result.source;
    EXPECT_LT(firstGuard, enter) << result.source;
    EXPECT_LT(enter, secondGuard) << result.source;
    EXPECT_LT(secondGuard, exitCall) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, InternsRepeatedClassCachesWithinFunction)
{
    std::string reason;
    auto        result =
        emitRegC("classCacheCse",
                 "(Ljava/lang/Object;)I",
                 {
                     new VarInsnNode(Opcodes::ALOAD, 0),
                     new TypeInsnNode(Opcodes::CHECKCAST,
                                      ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                     new InsnNode(Opcodes::POP),
                     new VarInsnNode(Opcodes::ALOAD, 0),
                     new TypeInsnNode(Opcodes::INSTANCEOF,
                                      ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                     new InsnNode(Opcodes::IRETURN),
                 },
                 1,
                 1,
                 reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(countSubstring(result.source, "{\"java/lang/String\", NULL}"), 1u) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_checkcast_cached"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_instanceof_cached"), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsThrowTerminatorWithImmediateReturn)
{
    std::string reason;
    auto        result = emitRegC(
        "raise",
        "()I",
        {
            new TypeInsnNode(Opcodes::NEW,
                             ca::str::Utf8StringRef::from_cstr("java/lang/IllegalStateException")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/IllegalStateException"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(Opcodes::ATHROW),
        },
        2,
        0,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_throw("), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_result = 0;"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("goto _mj2x_regc_cleanup;"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsTryCatchExceptionDispatch)
{
    auto      protectedBlock = std::make_unique<RegBlock>(SSABlockId{0});
    auto      handlerBlock   = std::make_unique<RegBlock>(SSABlockId{1});
    RegBlock* protectedRaw   = protectedBlock.get();
    RegBlock* handlerRaw     = handlerBlock.get();

    protectedBlock->terminator = RegThrowTerminator{RegOperand::nullRef()};
    RegReturnTerminator handlerReturn;
    handlerReturn.hasValue   = true;
    handlerReturn.value      = RegOperand::fromInt(2, SSAType::I32);
    handlerBlock->terminator = handlerReturn;

    RegFunction fn;
    fn.symbol.owner      = "InlineRegC";
    fn.symbol.name       = "catchOwn";
    fn.symbol.desc       = "()I";
    fn.symbol.returnType = SSAType::I32;
    fn.entry             = protectedRaw;
    fn.blocks.push_back(std::move(protectedBlock));
    fn.blocks.push_back(std::move(handlerBlock));

    RegExceptionRegion region;
    region.protectedBlocks.push_back(protectedRaw);
    region.handler        = handlerRaw;
    region.exceptionValue = RegValue{RegId{10}, SSAType::REF};
    region.caughtType     = "java/lang/IllegalArgumentException";
    fn.exceptionRegions.push_back(region);

    RegCEmitter emitter;
    auto        result = emitter.emitFunction(fn);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_exception_check()"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_exception_clear()"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_exception_matches"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("\"java/lang/IllegalArgumentException\""), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("goto block_b"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

// H3 守门：CFF 的 state 寄存器必须以 volatile 发射。
// 不加 volatile 时，state 是可跨基本块常量传播的普通局部量，C 编译器的
// jump-threading 会把 `dispatcher: switch(state)` 整块消解回原始结构化控制流
// ——产物源码看着复杂，二进制里根本没有平坦化（clang -O2 实测，见
// `work/cff_probe/st_cff.ll` vs `st_vol.ll`）。
TEST(RegCTest, CffStateRegisterIsEmittedVolatile)
{
    RegFunction fn;
    auto        entry      = std::make_unique<RegBlock>(SSABlockId{0});
    auto        trueBlock  = std::make_unique<RegBlock>(SSABlockId{1});
    auto        falseBlock = std::make_unique<RegBlock>(SSABlockId{2});

    const RegValue input{RegId{0}, SSAType::I32};
    const RegValue result{RegId{1}, SSAType::I32};
    const RegValue edgeValue{RegId{2}, SSAType::I32};
    fn.parameters.push_back(input);
    fn.symbol.owner      = "CffVolatile";
    fn.symbol.name       = "branch";
    fn.symbol.desc       = "(I)I";
    fn.symbol.returnType = SSAType::I32;

    RegInstruction add;
    add.kind            = RegInstructionKind::Binary;
    add.hasDest         = true;
    add.dest            = result;
    add.lhs             = RegOperand::fromRegister(input);
    add.rhs             = RegOperand::fromInt(1, SSAType::I32);
    add.binaryOp        = SSABinaryOp::Add;
    add.effect          = SSAEffect::pure();
    entry->instructions = {add};

    RegSuccessor trueTarget;
    trueTarget.block = trueBlock.get();
    trueTarget.copies.push_back(RegCopy{edgeValue, RegOperand::fromRegister(result)});
    RegSuccessor falseTarget;
    falseTarget.block = falseBlock.get();
    falseTarget.copies.push_back(RegCopy{edgeValue, RegOperand::fromRegister(input)});
    entry->terminator = RegBranchTerminator{
        RegOperand::fromInt(1, SSAType::BOOL), std::move(trueTarget), std::move(falseTarget)};
    trueBlock->terminator  = RegReturnTerminator{true, RegOperand::fromRegister(edgeValue)};
    falseBlock->terminator = RegReturnTerminator{true, RegOperand::fromRegister(edgeValue)};
    fn.entry               = entry.get();
    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(trueBlock));
    fn.blocks.push_back(std::move(falseBlock));

    RegTransformOptions options;
    options.enableControlFlowFlattening = true;
    const auto stats = applyRegTransformPasses(fn, options);
    ASSERT_TRUE(stats.controlFlowFlattened);
    ASSERT_TRUE(fn.cffVolatileState.has_value()) << "CFF must publish its state register";

    RegCEmitter emitter;
    auto        emitted = emitter.emitFunction(fn);
    ASSERT_TRUE(emitted.ok) << emitted.reason;

    // state 声明带 volatile，且它正是 dispatcher switch 的输入变量。
    const std::string stateName = "r" + std::to_string(fn.cffVolatileState->id.value);
    EXPECT_NE(emitted.source.find("volatile int32_t " + stateName + " = 0;"), std::string::npos)
        << "state must be volatile: " << emitted.source;
    EXPECT_NE(emitted.source.find("switch (" + stateName + ")"), std::string::npos)
        << "dispatcher must switch on the state register: " << emitted.source;
    expectNoStackAnchors(emitted.source);
}

// 关闭开关（cffStateVolatile=false）时回到普通局部量，产物形态与历史一致。
TEST(RegCTest, CffStateVolatileCanBeDisabled)
{
    RegFunction fn;
    auto        entry = std::make_unique<RegBlock>(SSABlockId{0});
    fn.symbol.owner   = "CffVolatile";
    fn.symbol.name    = "straight";
    fn.symbol.desc    = "()I";
    fn.symbol.returnType = SSAType::I32;
    entry->terminator    = RegReturnTerminator{true, RegOperand::fromInt(7, SSAType::I32)};
    // 人为给一个分支，让 CFF 有可平坦化的控制流。
    auto other = std::make_unique<RegBlock>(SSABlockId{1});
    other->terminator = RegReturnTerminator{true, RegOperand::fromInt(8, SSAType::I32)};
    entry->terminator = RegJumpTerminator{RegSuccessor{other.get(), {}}};
    fn.entry          = entry.get();
    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(other));

    RegTransformOptions options;
    options.enableControlFlowFlattening = true;
    options.cffStateVolatile            = false;
    const auto stats = applyRegTransformPasses(fn, options);
    ASSERT_TRUE(stats.controlFlowFlattened);
    EXPECT_FALSE(fn.cffVolatileState.has_value());

    RegCEmitter emitter;
    auto        emitted = emitter.emitFunction(fn);
    ASSERT_TRUE(emitted.ok) << emitted.reason;
    EXPECT_EQ(emitted.source.find("volatile int32_t"), std::string::npos) << emitted.source;
}

TEST(RegCTest, LowersHandlerLocalCopiesForExceptionalEdges)
{
    auto start   = new LabelNode(labelScratchArena().create<Label>());
    auto end     = new LabelNode(labelScratchArena().create<Label>());
    auto handler = new LabelNode(labelScratchArena().create<Label>());
    const std::string methodName = "monitorCatch";
    const std::string methodDesc = "(Ljava/lang/Object;)I";
    auto mn      = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        methodName,
        methodDesc,
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new InsnNode(Opcodes::DUP),
            new VarInsnNode(Opcodes::ASTORE, 1),
            start,
            new InsnNode(Opcodes::MONITORENTER),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new InsnNode(Opcodes::MONITOREXIT),
            end,
            new InsnNode(Opcodes::ICONST_1),
            new InsnNode(Opcodes::IRETURN),
            handler,
            new VarInsnNode(Opcodes::ASTORE, 2),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new InsnNode(Opcodes::MONITOREXIT),
            new VarInsnNode(Opcodes::ALOAD, 2),
            new InsnNode(Opcodes::ATHROW),
        },
        2,
        3);
    mn->tryCatchBlocks.push_back(mn->create<TryCatchBlockNode>(
        start->label, end->label, handler->label, ca::str::Utf8StringRef::from_cstr("")));

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    std::string              reason;
    auto                     ssa = builder.build(blocks,
                             "InlineRegC",
                             "monitorCatch",
                             "(Ljava/lang/Object;)I",
                             true,
                             3,
                             arena,
                             reason);
    ASSERT_NE(ssa, nullptr) << reason;

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    ASSERT_NE(reg, nullptr) << reason;
    ASSERT_FALSE(reg->exceptionRegions.empty());
    ASSERT_FALSE(reg->exceptionRegions.front().edges.empty());
    ASSERT_FALSE(reg->exceptionRegions.front().edges.front().copies.empty());

    const RegId handlerLocal = reg->exceptionRegions.front().edges.front().copies.front().dest.id;
    RegCEmitter emitter;
    auto        result = emitter.emitFunction(*reg);
    ASSERT_TRUE(result.ok) << result.reason;

    const std::string handlerName = "r" + std::to_string(handlerLocal.value);
    EXPECT_NE(result.source.find("void* " + handlerName + " = 0;"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find(handlerName + " = edge_tmp_"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_monitor_exit(" + handlerName + ")"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsIntegerDivRemHelpers)
{
    std::string reason;
    auto        result = emitRegC("divRem",
                           "(III)I",
                                  {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::IDIV),
                               new VarInsnNode(Opcodes::ILOAD, 2),
                               new InsnNode(Opcodes::IREM),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           2,
                           3,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_regc_div_i32(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_regc_rem_i32(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

/// 带 resolved dispatch + convertedFunctions 的发射辅助（静态绑定测试用）。
/// converted 表里预置目标方法键 -> regc_* 函数名。
RegCEmitResult emitRegCResolved(const std::string& name, const std::string& desc,
                                std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                int maxLocals, std::string& reason,
                                const std::unordered_map<std::string, std::string>& converted,
                                codegen::DirectCallTier tier = codegen::DirectCallTier::Off)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineRegC", name, desc, true, maxLocals, arena, reason);
    if (!ssa) {
        RegCEmitResult result;
        result.reason = reason;
        return result;
    }

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        RegCEmitResult result;
        result.reason = reason;
        return result;
    }

    RegCCachePool            pool;
    codegen::CodeOptions     options;
    options.resolvedDispatch = true;
    options.directCallTier   = tier;
    RegCEmitter emitter(&pool, options, nullptr, 0, nullptr, &converted, nullptr);
    return emitter.emitFunction(*reg);
}

TEST(RegCTest, EmitsStaticBoundCallForSingleAllocationReceiver)
{
    // 动态实例调用 receiver 是单一分配点（new Bar()）且目标方法已转换：
    // 跳过 resolved 运行时类检查，直接调用 regc_* 函数（保留 null 检查）。
    // 该直调仅在 All 档发射（默认 Off 保持 JVM 栈帧语义）。
    std::string reason;
    auto result = emitRegCResolved("staticBound",
                                   "(I)I",
                                   {
                                       new TypeInsnNode(Opcodes::NEW,
                                                        ca::str::Utf8StringRef::from_cstr("Bar")),
                                       new InsnNode(Opcodes::DUP),
                                       new InsnNode(Opcodes::ICONST_1),
                                       new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                                          ca::str::Utf8StringRef::from_cstr("Bar"),
                                                          ca::str::Utf8StringRef::from_cstr("<init>"),
                                                          ca::str::Utf8StringRef::from_cstr("(I)V"),
                                                          false),
                                       new VarInsnNode(Opcodes::ILOAD, 0),
                                       new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                          ca::str::Utf8StringRef::from_cstr("Bar"),
                                                          ca::str::Utf8StringRef::from_cstr("compute"),
                                                          ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                          false),
                                       new InsnNode(Opcodes::IRETURN),
                                   },
                                   2,
                                   1,
                                   reason,
                                   {{"Bar\ncompute\n(I)I", "regc_test_Bar_compute__I_I"}},
                                   codegen::DirectCallTier::All);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("regc_test_Bar_compute__I_I("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_resolved_site"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_throw_null(\"instance method call\")"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, KeepsResolvedDispatchForUnknownReceiverType)
{
    // receiver 是方法参数（类型不可静态确定）：保持 resolved dispatch 检查链。
    std::string reason;
    auto        result = emitRegCResolved("paramReceiver",
                                   "(LBar;I)I",
                                          {
                                       new VarInsnNode(Opcodes::ALOAD, 0),
                                       new VarInsnNode(Opcodes::ILOAD, 1),
                                       new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                          ca::str::Utf8StringRef::from_cstr("Bar"),
                                                          ca::str::Utf8StringRef::from_cstr("compute"),
                                                          ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                          false),
                                       new InsnNode(Opcodes::IRETURN),
                                   },
                                   2,
                                   2,
                                   reason,
                                   {{"Bar\ncompute\n(I)I", "regc_test_Bar_compute__I_I"}});

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_resolved_site"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

// ---- 直调档位（issue #18）----
//
// 三档按"栈失真范围"划分，不是布尔开关：Off 全走 JNI、IntraClass 只放行同类内
// 直调、All 放行全部确定性分派。用自递归（必同类）验证阶梯：Off 走 cached JNI，
// IntraClass 起直调。

TEST(RegCTest, DirectCallTierOffKeepsCachedJniForSelfCall)
{
    std::string reason;
    auto        result = emitRegC("self",
                          "(I)I",
                                  {
                              new VarInsnNode(Opcodes::ILOAD, 0),
                              new InsnNode(Opcodes::ICONST_1),
                              new InsnNode(Opcodes::ISUB),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("InlineRegC"),
                                                 ca::str::Utf8StringRef::from_cstr("self"),
                                                 ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                 false),
                              new InsnNode(Opcodes::IRETURN),
                          },
                          2,
                          1,
                          reason,
                          codegen::DirectCallTier::Off);

    ASSERT_TRUE(result.ok) << result.reason;
    // Off：自递归退回 cached JNI（保 JVM 栈帧语义，issue #814）。
    // 负向断言要带实参强转——只写函数名会命中函数定义本身那一行。
    EXPECT_EQ(result.source.find("regc_InlineRegC_self__I_I((int32_t)("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
}

TEST(RegCTest, DirectCallTierIntraClassEnablesSelfCall)
{
    std::string reason;
    auto        result = emitRegC("self",
                          "(I)I",
                                  {
                              new VarInsnNode(Opcodes::ILOAD, 0),
                              new InsnNode(Opcodes::ICONST_1),
                              new InsnNode(Opcodes::ISUB),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("InlineRegC"),
                                                 ca::str::Utf8StringRef::from_cstr("self"),
                                                 ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                 false),
                              new InsnNode(Opcodes::IRETURN),
                          },
                          2,
                          1,
                          reason,
                          codegen::DirectCallTier::IntraClass);

    ASSERT_TRUE(result.ok) << result.reason;
    // 自递归必同类，IntraClass 档即应直调。
    EXPECT_NE(result.source.find("regc_InlineRegC_self__I_I((int32_t)("), std::string::npos)
        << result.source;
}

// 档位判定本身：同类/跨类在各档下的取值。RegC 与 LLVM 共用同一函数，两端口径
// 不一致正是本档位要消掉的东西。
TEST(RegCTest, AllowsDirectCallFollowsTierAndClassBoundary)
{
    using codegen::DirectCallTier;
    // 确定性虚调用任何档位都放行：resolved 路径本来就直调 fn，语义与直调一致。
    EXPECT_TRUE(codegen::allowsDirectCall(DirectCallTier::Off, false, true));
    EXPECT_TRUE(codegen::allowsDirectCall(DirectCallTier::Off, true, true));

    // 非确定性分派：按档位 + 是否同类。
    EXPECT_FALSE(codegen::allowsDirectCall(DirectCallTier::Off, true, false));
    EXPECT_FALSE(codegen::allowsDirectCall(DirectCallTier::Off, false, false));

    EXPECT_TRUE(codegen::allowsDirectCall(DirectCallTier::IntraClass, true, false));
    EXPECT_FALSE(codegen::allowsDirectCall(DirectCallTier::IntraClass, false, false));

    EXPECT_TRUE(codegen::allowsDirectCall(DirectCallTier::All, true, false));
    EXPECT_TRUE(codegen::allowsDirectCall(DirectCallTier::All, false, false));
}

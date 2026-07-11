#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const std::string& name, const std::string& desc,
                                       std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                       int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn       = std::make_unique<MethodNode>(access,
                                                 ca::str::Utf8StringRef::from_cstr(name.c_str()),
                                                 ca::str::Utf8StringRef::from_cstr(desc.c_str()),
                                                 ca::str::Utf8StringRef::from_cstr(""),
                                                 noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

std::unique_ptr<RegFunction> buildRegIR(const std::string& name, const std::string& desc,
                                        std::initializer_list<AbstractInsnNode*> insns,
                                        int maxStack, int maxLocals, std::string& reason)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);

    CodeConverter converter;
    auto          blocks = converter.convert(*mn);
    SSABuilder    builder;
    auto          ssa = builder.build(blocks, "InlineRegC", name, desc, true, maxLocals, reason);
    if (!ssa) return nullptr;

    RegLowering lowering;
    return lowering.lower(*ssa, reason);
}

RegCEmitResult emitRegC(const std::string& name, const std::string& desc,
                        std::initializer_list<AbstractInsnNode*> insns, int maxStack, int maxLocals,
                        std::string& reason)
{
    auto reg = buildRegIR(name, desc, insns, maxStack, maxLocals, reason);
    if (!reg) {
        RegCEmitResult result;
        result.reason = reason;
        return result;
    }

    RegCEmitter emitter;
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
    EXPECT_NE(result.source.find("goto _mj2x_regc_cleanup;"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("return _mj2x_regc_result;"), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsLoopControlFlowWithEdgeCopies)
{
    auto* loop = new LabelNode();
    auto* done = new LabelNode();

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
    auto* case0 = new LabelNode();
    auto* case1 = new LabelNode();
    auto* def   = new LabelNode();

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
    EXPECT_NE(result.source.find("_mj2x_regc_call_cached_i32(&_mj2x_regc_cache_"),
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
    EXPECT_NE(result.source.find("_mj2x_regc_call_cached_i32(&_mj2x_regc_cache_"),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCTest, EmitsSelfStaticCallAsDirectCDispatch)
{
    std::string reason;
    auto        result = emitRegC("self",
                                  "(I)I",
                                  {
                                      new VarInsnNode(Opcodes::ILOAD, 0),
                                      new InsnNode(Opcodes::ICONST_1),
                                      new InsnNode(Opcodes::ISUB),
                                      new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                         ca::str::Utf8StringRef::from_cstr(
                                                             "InlineRegC"),
                                                         ca::str::Utf8StringRef::from_cstr("self"),
                                                         ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                         false),
                                      new InsnNode(Opcodes::IRETURN),
                                  },
                                  2,
                                  1,
                                  reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("regc_InlineRegC_self__I_I((int32_t)("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_cached_i32"), std::string::npos)
        << result.source;
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
    EXPECT_EQ(countSubstring(result.source, "{\"java/lang/String\", NULL}"), 1u)
        << result.source;
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
    EXPECT_NE(result.source.find("goto _mj2x_regc_cleanup;"), std::string::npos)
        << result.source;
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

// Unit tests for the RegC JDK-intrinsic recognition in the Call emitter path.
//
// These tests construct bytecode that calls java.lang.Math / Integer.parseInt
// and assert that the emitted C lowers those calls to mj2x_math_* /
// _mj2x_intrinsic_parse_int instead of the generic _mj2x_regc_call_* JNI
// dispatch helper.  The bodies are not executed here; that is the E2E suite's
// job.  Here we only verify the intrinsic is recognized and the JNI call
// surface is absent from the generated source.

#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
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
    auto&                               arena = Type::staticArena();
    auto mn       = std::make_unique<MethodNode>(access,
                                                 arena.intern(name.c_str()),
                                                 arena.intern(desc.c_str()),
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
    auto          ssa = builder.build(blocks, "IntrinsicRegC", name, desc, true, maxLocals, reason);
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
}

// Shorthand for the Utf8StringRef owner/name/desc arguments MethodInsnNode
// requires; keeps the intrinsic test bodies readable.
ca::str::Utf8StringRef U(const char* s)
{
    return ca::str::Utf8StringRef::from_cstr(s);
}

RegInstruction makeStaticCall(const char* owner, const char* name, const char* desc,
                              std::initializer_list<RegOperand> args)
{
    RegInstruction ins;
    ins.kind                = RegInstructionKind::Call;
    ins.method.owner        = owner;
    ins.method.name         = name;
    ins.method.desc         = desc;
    ins.method.invokeOpcode = Opcodes::INVOKESTATIC;
    ins.args.assign(args.begin(), args.end());
    return ins;
}

}   // namespace

TEST(RegCIntrinsicRegistryTest, MatchesMathIntrinsicThroughRegistry)
{
    auto ins = makeStaticCall("java/lang/Math",
                              "sqrt",
                              "(D)D",
                              {
                                  RegOperand::fromFloat(9.0, SSAType::F64),
                              });

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(decision.emitKind, RegCIntrinsicEmitKind::RuntimeFunction);
    EXPECT_EQ(decision.symbol, "mj2x_math_sqrt");
    EXPECT_FALSE(decision.passContext);
}

TEST(RegCIntrinsicRegistryTest, UnsupportedOverloadFallsBackWithReason)
{
    auto ins = makeStaticCall("java/lang/Math",
                              "sqrt",
                              "(I)I",
                              {
                                  RegOperand::fromInt(9, SSAType::I32),
                              });

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Fallback);
    EXPECT_NE(decision.reason.find("unsupported intrinsic overload: java/lang/Math.sqrt(I)I"),
              std::string::npos);
}

TEST(RegCIntrinsicRegistryTest, MalformedIntrinsicArgumentCountIsUnsupported)
{
    auto ins = makeStaticCall("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", {});

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Unsupported);
    EXPECT_NE(decision.reason.find("malformed RegIR argument count for intrinsic"),
              std::string::npos);
}

TEST(RegCIntrinsicRegistryTest, MatchesWrapperValueOfAndUnbox)
{
    auto valueOf       = makeStaticCall("java/lang/Integer",
                                        "valueOf",
                                        "(I)Ljava/lang/Integer;",
                                        {
                                            RegOperand::fromInt(7, SSAType::I32),
                                        });
    auto valueDecision = RegCIntrinsicRegistry::builtins().matchCall(valueOf);
    EXPECT_EQ(valueDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(valueDecision.emitKind, RegCIntrinsicEmitKind::WrapperValueOf);
    EXPECT_EQ(valueDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::Int);
    EXPECT_EQ(valueDecision.symbol, "mj2x_int_value_of");

    RegInstruction unbox;
    unbox.kind                = RegInstructionKind::Call;
    unbox.method.owner        = "java/lang/Integer";
    unbox.method.name         = "intValue";
    unbox.method.desc         = "()I";
    unbox.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    unbox.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto unboxDecision = RegCIntrinsicRegistry::builtins().matchCall(unbox);
    EXPECT_EQ(unboxDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(unboxDecision.emitKind, RegCIntrinsicEmitKind::WrapperUnbox);
    EXPECT_EQ(unboxDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::Int);
    EXPECT_EQ(unboxDecision.symbol, "mj2x_int_int_value");
}

TEST(RegCIntrinsicRegistryTest, MatchesRandomInitAndNextInt)
{
    RegInstruction init;
    init.kind                = RegInstructionKind::Call;
    init.method.owner        = "java/util/Random";
    init.method.name         = "<init>";
    init.method.desc         = "(J)V";
    init.method.invokeOpcode = Opcodes::INVOKESPECIAL;
    init.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    init.args.push_back(RegOperand::fromInt(42, SSAType::I64));

    auto initDecision = RegCIntrinsicRegistry::builtins().matchCall(init);
    EXPECT_EQ(initDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(initDecision.emitKind, RegCIntrinsicEmitKind::RandomInit);
    EXPECT_EQ(initDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::Random);

    RegInstruction next;
    next.kind                = RegInstructionKind::Call;
    next.method.owner        = "java/util/Random";
    next.method.name         = "nextInt";
    next.method.desc         = "()I";
    next.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    next.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto nextDecision = RegCIntrinsicRegistry::builtins().matchCall(next);
    EXPECT_EQ(nextDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(nextDecision.emitKind, RegCIntrinsicEmitKind::NativeObjectMethod);
    EXPECT_EQ(nextDecision.symbol, "mj2x_random_next_int");
}

TEST(RegCIntrinsicRegistryTest, MatchesStringBuilderIntrinsics)
{
    RegInstruction init;
    init.kind                = RegInstructionKind::Call;
    init.method.owner        = "java/lang/StringBuilder";
    init.method.name         = "<init>";
    init.method.desc         = "()V";
    init.method.invokeOpcode = Opcodes::INVOKESPECIAL;
    init.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto initDecision = RegCIntrinsicRegistry::builtins().matchCall(init);
    EXPECT_EQ(initDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(initDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderInit);
    EXPECT_EQ(initDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);

    RegInstruction append;
    append.kind                = RegInstructionKind::Call;
    append.method.owner        = "java/lang/StringBuilder";
    append.method.name         = "append";
    append.method.desc         = "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
    append.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    append.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    append.args.push_back(RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}));

    auto appendDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);

    append.method.desc = "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});
    auto appendObjectDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendObjectDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendObjectDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendObjectDecision.symbol, "_mj2x_intrinsic_string_builder_append_object");

    append.method.desc = "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});
    auto appendCharSequenceDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendCharSequenceDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendCharSequenceDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendCharSequenceDecision.symbol, "_mj2x_intrinsic_string_builder_append_object");

    append.method.desc = "(Z)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::BOOL});
    auto appendBoolDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendBoolDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendBoolDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendBoolDecision.symbol, "_mj2x_intrinsic_string_builder_append_bool");

    append.method.desc = "(C)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::CHAR});
    auto appendCharDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendCharDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendCharDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendCharDecision.symbol, "_mj2x_intrinsic_string_builder_append_char");

    append.method.desc = "(F)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F32});
    auto appendFloatDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendFloatDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendFloatDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendFloatDecision.symbol, "_mj2x_intrinsic_string_builder_append_float");

    append.method.desc = "(D)Ljava/lang/StringBuilder;";
    append.args[1] = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F64});
    auto appendDoubleDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendDoubleDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendDoubleDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendDoubleDecision.symbol, "_mj2x_intrinsic_string_builder_append_double");

    RegInstruction toString;
    toString.kind                = RegInstructionKind::Call;
    toString.method.owner        = "java/lang/StringBuilder";
    toString.method.name         = "toString";
    toString.method.desc         = "()Ljava/lang/String;";
    toString.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    toString.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto toStringDecision = RegCIntrinsicRegistry::builtins().matchCall(toString);
    EXPECT_EQ(toStringDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(toStringDecision.emitKind, RegCIntrinsicEmitKind::NativeObjectMethod);
    EXPECT_EQ(toStringDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);
}

TEST(RegCIntrinsicRegistryTest, MatchesStringIntrinsics)
{
    RegInstruction length;
    length.kind                = RegInstructionKind::Call;
    length.method.owner        = "java/lang/String";
    length.method.name         = "length";
    length.method.desc         = "()I";
    length.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    length.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto lengthDecision = RegCIntrinsicRegistry::builtins().matchCall(length);
    EXPECT_EQ(lengthDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(lengthDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(lengthDecision.symbol, "_mj2x_intrinsic_string_length");

    RegInstruction charAt;
    charAt.kind                = RegInstructionKind::Call;
    charAt.method.owner        = "java/lang/String";
    charAt.method.name         = "charAt";
    charAt.method.desc         = "(I)C";
    charAt.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    charAt.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    charAt.args.push_back(RegOperand::fromInt(1, SSAType::I32));

    auto charAtDecision = RegCIntrinsicRegistry::builtins().matchCall(charAt);
    EXPECT_EQ(charAtDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(charAtDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(charAtDecision.symbol, "_mj2x_intrinsic_string_char_at");

    RegInstruction isEmpty;
    isEmpty.kind                = RegInstructionKind::Call;
    isEmpty.method.owner        = "java/lang/String";
    isEmpty.method.name         = "isEmpty";
    isEmpty.method.desc         = "()Z";
    isEmpty.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    isEmpty.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto isEmptyDecision = RegCIntrinsicRegistry::builtins().matchCall(isEmpty);
    EXPECT_EQ(isEmptyDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(isEmptyDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(isEmptyDecision.symbol, "_mj2x_intrinsic_string_is_empty");

    RegInstruction contains;
    contains.kind                = RegInstructionKind::Call;
    contains.method.owner        = "java/lang/String";
    contains.method.name         = "contains";
    contains.method.desc         = "(Ljava/lang/CharSequence;)Z";
    contains.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    contains.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    contains.args.push_back(RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}));

    auto containsDecision = RegCIntrinsicRegistry::builtins().matchCall(contains);
    EXPECT_EQ(containsDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(containsDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(containsDecision.symbol, "_mj2x_intrinsic_string_contains");

    RegInstruction valueOf;
    valueOf.kind                = RegInstructionKind::Call;
    valueOf.method.owner        = "java/lang/String";
    valueOf.method.name         = "valueOf";
    valueOf.method.desc         = "(D)Ljava/lang/String;";
    valueOf.method.invokeOpcode = Opcodes::INVOKESTATIC;
    valueOf.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F64}));

    auto valueOfDecision = RegCIntrinsicRegistry::builtins().matchCall(valueOf);
    EXPECT_EQ(valueOfDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(valueOfDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(valueOfDecision.symbol, "_mj2x_intrinsic_double_to_string");
}

TEST(RegCIntrinsicTest, MathSinLoweredToRuntimeCallNotJni)
{
    std::string reason;
    // static double sinCall() { return Math.sin(1.0); }
    auto result =
        emitRegC("sinCall",
                 "()D",
                 {
                     new LdcInsnNode(1.0),
                     new MethodInsnNode(
                         Opcodes::INVOKESTATIC, U("java/lang/Math"), U("sin"), U("(D)D"), false),
                     new InsnNode(Opcodes::DRETURN),
                 },
                 2,
                 0,
                 reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_sin("), std::string::npos) << result.source;
    // The Math.sin call site must not dispatch through the JNI call helper.
    EXPECT_EQ(result.source.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, MathAbsIntPicksI32Overload)
{
    std::string reason;
    // static int absCall(int x) { return Math.abs(x); }
    auto result =
        emitRegC("absCall",
                 "(I)I",
                 {
                     new VarInsnNode(Opcodes::ILOAD, 0),
                     new MethodInsnNode(
                         Opcodes::INVOKESTATIC, U("java/lang/Math"), U("abs"), U("(I)I"), false),
                     new InsnNode(Opcodes::IRETURN),
                 },
                 1,
                 1,
                 reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_abs_i32("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Math\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, MathMaxDoubleLoweredToF64Variant)
{
    std::string reason;
    // static double maxCall(double a, double b) { return Math.max(a, b); }
    auto result =
        emitRegC("maxCall",
                 "(DD)D",
                 {
                     new VarInsnNode(Opcodes::DLOAD, 0),
                     new VarInsnNode(Opcodes::DLOAD, 2),
                     new MethodInsnNode(
                         Opcodes::INVOKESTATIC, U("java/lang/Math"), U("max"), U("(DD)D"), false),
                     new InsnNode(Opcodes::DRETURN),
                 },
                 4,
                 4,
                 reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_max_f64("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, IntegerParseIntLoweredToIntrinsicNotJni)
{
    std::string reason;
    // static int parse(String s) { return Integer.parseInt(s); }
    auto result = emitRegC("parse",
                           "(Ljava/lang/String;)I",
                           {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Integer"),
                                                  U("parseInt"),
                                                  U("(Ljava/lang/String;)I"),
                                                  false),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           1,
                           1,
                           reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_int("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringMethodsLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int strings(String s, Object o) {
    //   return s.length() + s.charAt(1) + s.hashCode() + (s.equals(o) ? 1 : 0);
    // }
    auto result = emitRegC("strings",
                           "(Ljava/lang/String;Ljava/lang/Object;)I",
                           {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("length"),
                                                  U("()I"),
                                                  false),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new InsnNode(Opcodes::ICONST_1),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("charAt"),
                                                  U("(I)C"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("hashCode"),
                                                  U("()I"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new VarInsnNode(Opcodes::ALOAD, 1),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("equals"),
                                                  U("(Ljava/lang/Object;)Z"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           3,
                           2,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_length("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_char_at("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_hash_code("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_equals("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"length\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"charAt\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"hashCode\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"equals\""), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, DiscardedStringIntrinsicReturnStillLowers)
{
    std::string reason;
    // static int discard(String s) { s.length(); return 7; }
    auto result = emitRegC("discardStringLength",
                           "(Ljava/lang/String;)I",
                           {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("length"),
                                                  U("()I"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new IntInsnNode(Opcodes::BIPUSH, 7),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           1,
                           1,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_length("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/String\", \"length\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, MoreStringMethodsLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int more(String s) {
    //   return (s.isEmpty() ? 1 : 0) + (s.startsWith("pre") ? 1 : 0)
    //       + (s.endsWith("fix") ? 1 : 0) + (s.contains("needle") ? 1 : 0)
    //       + s.indexOf("needle");
    // }
    auto result = emitRegC("moreStrings",
                           "(Ljava/lang/String;)I",
                           {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("isEmpty"),
                                                  U("()Z"),
                                                  false),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new LdcInsnNode(std::string("pre")),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("startsWith"),
                                                  U("(Ljava/lang/String;)Z"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new LdcInsnNode(std::string("fix")),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("endsWith"),
                                                  U("(Ljava/lang/String;)Z"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new LdcInsnNode(std::string("needle")),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("contains"),
                                                  U("(Ljava/lang/CharSequence;)Z"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new LdcInsnNode(std::string("needle")),
                               new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                  U("java/lang/String"),
                                                  U("indexOf"),
                                                  U("(Ljava/lang/String;)I"),
                                                  false),
                               new InsnNode(Opcodes::IADD),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           3,
                           1,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_is_empty("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_starts_with("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_ends_with("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_index_of("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_contains("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"isEmpty\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"startsWith\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"endsWith\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"indexOf\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/String\", \"contains\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, StringLdcFeedsParseIntWithLiteral)
{
    std::string reason;
    // static int parseLit() { return Integer.parseInt("42"); }
    auto result = emitRegC("parseLit",
                           "()I",
                           {
                               new LdcInsnNode(std::string("42")),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Integer"),
                                                  U("parseInt"),
                                                  U("(Ljava/lang/String;)I"),
                                                  false),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           1,
                           0,
                           reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // The String literal must be materialized via the intrinsic helper and
    // flow into parseInt without a JNI call.
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_literal("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_int("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, WrapperParseCompareToStringLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static long wrappers(int i, long l) {
    //   return Long.parseLong(Integer.toString(i))
    //       + Integer.compare(i, 7)
    //       + Long.compare(l, 9L)
    //       + Long.parseLong(Long.toString(l));
    // }
    auto result = emitRegC(
        "wrapperMore",
        "(IJ)J",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Integer"),
                               U("toString"),
                               U("(I)Ljava/lang/String;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Long"),
                               U("parseLong"),
                               U("(Ljava/lang/String;)J"),
                               false),
            new VarInsnNode(Opcodes::ILOAD, 0),
            new IntInsnNode(Opcodes::BIPUSH, 7),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("compare"), U("(II)I"), false),
            new InsnNode(Opcodes::I2L),
            new InsnNode(Opcodes::LADD),
            new VarInsnNode(Opcodes::LLOAD, 1),
            new LdcInsnNode(static_cast<int64_t>(9)),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Long"), U("compare"), U("(JJ)I"), false),
            new InsnNode(Opcodes::I2L),
            new InsnNode(Opcodes::LADD),
            new VarInsnNode(Opcodes::LLOAD, 1),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Long"),
                               U("toString"),
                               U("(J)Ljava/lang/String;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Long"),
                               U("parseLong"),
                               U("(Ljava/lang/String;)J"),
                               false),
            new InsnNode(Opcodes::LADD),
            new InsnNode(Opcodes::LRETURN),
        },
        4,
        3,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_int_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_long_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_long("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_int_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_long_compare("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Integer\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i64(\"java/lang/Long\", \"parseLong\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Long\", \"compare\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, StringValueOfLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int values(int i, long l, boolean b, char c, float f, double d, Object o) {
    //   String.valueOf(i); String.valueOf(l); String.valueOf(b); String.valueOf(c);
    //   String.valueOf(f); String.valueOf(d); String.valueOf(o); return 0;
    // }
    auto result = emitRegC(
        "stringValues",
        "(IJZCFDLjava/lang/Object;)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(I)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::LLOAD, 1),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(J)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ILOAD, 3),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(Z)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ILOAD, 4),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(C)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::FLOAD, 5),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(F)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::DLOAD, 6),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(D)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ALOAD, 8),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/String"),
                               U("valueOf"),
                               U("(Ljava/lang/Object;)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_0),
            new InsnNode(Opcodes::IRETURN),
        },
        3,
        9,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_int_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_long_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_bool_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_char_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_float_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_double_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_object_to_string("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/String\", \"valueOf\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, IntegerValueOfIntValueLoweredToNativeWrapper)
{
    std::string reason;
    // static int boxed(int x) { return Integer.valueOf(x).intValue(); }
    auto result = emitRegC(
        "boxed",
        "(I)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Integer"),
                               U("valueOf"),
                               U("(I)Ljava/lang/Integer;"),
                               false),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/Integer"), U("intValue"), U("()I"), false),
            new InsnNode(Opcodes::IRETURN),
        },
        1,
        1,
        reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_int native_r"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_int_value_of("), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_int_int_value("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Integer\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, EscapingWrapperValueFailsClosedWithReason)
{
    std::string reason;
    // static Object boxed(int x) { return Integer.valueOf(x); }
    auto result = emitRegC("boxedObject",
                           "(I)Ljava/lang/Object;",
                           {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Integer"),
                                                  U("valueOf"),
                                                  U("(I)Ljava/lang/Integer;"),
                                                  false),
                               new InsnNode(Opcodes::ARETURN),
                           },
                           1,
                           1,
                           reason);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("native object escaped through method return"), std::string::npos)
        << result.reason;
}

TEST(RegCIntrinsicTest, RandomNewInitNextIntLoweredToNativeState)
{
    std::string reason;
    // static int next(int seed) { return new Random((long)seed).nextInt(); }
    auto result = emitRegC(
        "next",
        "(I)I",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/util/Random")),
            new InsnNode(Opcodes::DUP),
            new VarInsnNode(Opcodes::ILOAD, 0),
            new InsnNode(Opcodes::I2L),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/util/Random"), U("<init>"), U("(J)V"), false),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/util/Random"), U("nextInt"), U("()I"), false),
            new InsnNode(Opcodes::IRETURN),
        },
        4,
        1,
        reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_random native_r"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_random_init(&native_r"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_random_next_int(&native_r"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_alloc_object(\"java/util/Random\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/util/Random\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, EscapingRandomStateFailsClosedWithReason)
{
    std::string reason;
    // static Object make(long seed) { return new Random(seed); }
    auto result = emitRegC(
        "makeRandom",
        "(J)Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/util/Random")),
            new InsnNode(Opcodes::DUP),
            new VarInsnNode(Opcodes::LLOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/util/Random"), U("<init>"), U("(J)V"), false),
            new InsnNode(Opcodes::ARETURN),
        },
        4,
        2,
        reason);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("native object escaped through method return"), std::string::npos)
        << result.reason;
}

TEST(RegCIntrinsicTest, StringBuilderChainLoweredToNativeState)
{
    std::string reason;
    // static String build(String s, int x, Object o, CharSequence cs, float f, double d) {
    //   return new StringBuilder().append(s).append(x).append(7L).append(true).append('!').append(f).append(d).append(o).append(cs).toString();
    // }
    auto result = emitRegC(
        "buildString",
        "(Ljava/lang/String;ILjava/lang/Object;Ljava/lang/CharSequence;FD)Ljava/lang/String;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                               false),
            new VarInsnNode(Opcodes::ILOAD, 1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(I)Ljava/lang/StringBuilder;"),
                               false),
            new LdcInsnNode(static_cast<int64_t>(7)),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(J)Ljava/lang/StringBuilder;"),
                               false),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Z)Ljava/lang/StringBuilder;"),
                               false),
            new IntInsnNode(Opcodes::BIPUSH, 33),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(C)Ljava/lang/StringBuilder;"),
                               false),
            new VarInsnNode(Opcodes::FLOAD, 4),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(F)Ljava/lang/StringBuilder;"),
                               false),
            new VarInsnNode(Opcodes::DLOAD, 5),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(D)Ljava/lang/StringBuilder;"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 2),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/Object;)Ljava/lang/StringBuilder;"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 3),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("toString"),
                               U("()Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        6,
        7,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_string_builder native_r"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_init_empty(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_string(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_int(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_long(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_bool(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_char(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_float(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_double(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_append_object(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(
        result.source.find("_mj2x_regc_call_ref(\"java/lang/StringBuilder\", \"append\""),
        std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_builder_to_string(&native_r"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuilder\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/StringBuilder\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_void(\"java/lang/StringBuilder\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, EscapingStringBuilderStateFailsClosedWithReason)
{
    std::string reason;
    // static Object make() { return new StringBuilder(); }
    auto result = emitRegC(
        "makeStringBuilder",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new InsnNode(Opcodes::ARETURN),
        },
        2,
        0,
        reason);

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("native object escaped through method return"), std::string::npos)
        << result.reason;
}

TEST(RegCIntrinsicTest, NonIntrinsicCallStillUsesJniHelper)
{
    std::string reason;
    // An unhandled JDK call (Integer.signum) must fall through to the generic
    // JNI call helper so we never silently drop a call site.
    auto result = emitRegC(
        "signum",
        "(I)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("signum"), U("(I)I"), false),
            new InsnNode(Opcodes::IRETURN),
        },
        1,
        1,
        reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("{\"java/lang/Integer\", \"signum\", \"(I)I\", NULL, NULL}"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_call_cached_i32(&_mj2x_regc_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
    // signum must not have been misrecognized as an intrinsic call site.
    EXPECT_EQ(result.source.find("= mj2x_math_"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("= _mj2x_intrinsic_parse_int"), std::string::npos)
        << result.source;
}

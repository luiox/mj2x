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

std::unique_ptr<MethodNode> makeMethod(
    int access,
    const std::string& name,
    const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(
        access,
        ca::str::Utf8StringRef::from_cstr(name.c_str()),
        ca::str::Utf8StringRef::from_cstr(desc.c_str()),
        ca::str::Utf8StringRef::from_cstr(""),
        noExcepts);
    mn->maxStack = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

std::unique_ptr<RegFunction> buildRegIR(
    const std::string& name,
    const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals,
    std::string& reason)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         name, desc, insns, maxStack, maxLocals);
    CodeConverter converter;
    auto blocks = converter.convert(*mn);
    SSABuilder builder;
    auto ssa = builder.build(blocks, "IntrinsicRegC", name, desc, true, maxLocals, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    return lowering.lower(*ssa, reason);
}

RegCEmitResult emitRegC(
    const std::string& name,
    const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals,
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

void expectNoStackAnchors(const std::string& source) {
    std::string reason;
    EXPECT_TRUE(validateRegCNoStackAnchors(source, reason)) << reason << "\n" << source;
}

// Shorthand for the Utf8StringRef owner/name/desc arguments MethodInsnNode
// requires; keeps the intrinsic test bodies readable.
ca::str::Utf8StringRef U(const char* s) {
    return ca::str::Utf8StringRef::from_cstr(s);
}

RegInstruction makeStaticCall(
    const char* owner,
    const char* name,
    const char* desc,
    std::initializer_list<RegOperand> args)
{
    RegInstruction ins;
    ins.kind = RegInstructionKind::Call;
    ins.method.owner = owner;
    ins.method.name = name;
    ins.method.desc = desc;
    ins.method.invokeOpcode = Opcodes::INVOKESTATIC;
    ins.args.assign(args.begin(), args.end());
    return ins;
}

} // namespace

TEST(RegCIntrinsicRegistryTest, MatchesMathIntrinsicThroughRegistry) {
    auto ins = makeStaticCall("java/lang/Math", "sqrt", "(D)D", {
        RegOperand::fromFloat(9.0, SSAType::F64),
    });

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(decision.emitKind, RegCIntrinsicEmitKind::RuntimeFunction);
    EXPECT_EQ(decision.symbol, "mj2x_math_sqrt");
    EXPECT_FALSE(decision.passContext);
}

TEST(RegCIntrinsicRegistryTest, UnsupportedOverloadFallsBackWithReason) {
    auto ins = makeStaticCall("java/lang/Math", "sqrt", "(I)I", {
        RegOperand::fromInt(9, SSAType::I32),
    });

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Fallback);
    EXPECT_NE(decision.reason.find("unsupported intrinsic overload: java/lang/Math.sqrt(I)I"),
              std::string::npos);
}

TEST(RegCIntrinsicRegistryTest, MalformedIntrinsicArgumentCountIsUnsupported) {
    auto ins = makeStaticCall("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", {});

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Unsupported);
    EXPECT_NE(decision.reason.find("malformed RegIR argument count for intrinsic"),
              std::string::npos);
}

TEST(RegCIntrinsicTest, MathSinLoweredToRuntimeCallNotJni) {
    std::string reason;
    // static double sinCall() { return Math.sin(1.0); }
    auto result = emitRegC("sinCall", "()D", {
        new LdcInsnNode(1.0),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Math"), U("sin"), U("(D)D"), false),
        new InsnNode(Opcodes::DRETURN),
    }, 2, 0, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_sin("), std::string::npos) << result.source;
    // The Math.sin call site must not dispatch through the JNI call helper.
    EXPECT_EQ(result.source.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, MathAbsIntPicksI32Overload) {
    std::string reason;
    // static int absCall(int x) { return Math.abs(x); }
    auto result = emitRegC("absCall", "(I)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Math"), U("abs"), U("(I)I"), false),
        new InsnNode(Opcodes::IRETURN),
    }, 1, 1, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_abs_i32("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Math\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, MathMaxDoubleLoweredToF64Variant) {
    std::string reason;
    // static double maxCall(double a, double b) { return Math.max(a, b); }
    auto result = emitRegC("maxCall", "(DD)D", {
        new VarInsnNode(Opcodes::DLOAD, 0),
        new VarInsnNode(Opcodes::DLOAD, 2),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Math"), U("max"), U("(DD)D"), false),
        new InsnNode(Opcodes::DRETURN),
    }, 4, 4, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_math_max_f64("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, IntegerParseIntLoweredToIntrinsicNotJni) {
    std::string reason;
    // static int parse(String s) { return Integer.parseInt(s); }
    auto result = emitRegC("parse", "(Ljava/lang/String;)I", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("parseInt"),
                           U("(Ljava/lang/String;)I"), false),
        new InsnNode(Opcodes::IRETURN),
    }, 1, 1, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_int("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringLdcFeedsParseIntWithLiteral) {
    std::string reason;
    // static int parseLit() { return Integer.parseInt("42"); }
    auto result = emitRegC("parseLit", "()I", {
        new LdcInsnNode(std::string("42")),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("parseInt"),
                           U("(Ljava/lang/String;)I"), false),
        new InsnNode(Opcodes::IRETURN),
    }, 1, 0, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // The String literal must be materialized via the intrinsic helper and
    // flow into parseInt without a JNI call.
    EXPECT_NE(result.source.find("_mj2x_intrinsic_string_literal("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_int("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, NonIntrinsicCallStillUsesJniHelper) {
    std::string reason;
    // An unhandled JDK call (Integer.signum) must fall through to the generic
    // JNI call helper so we never silently drop a call site.
    auto result = emitRegC("signum", "(I)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new MethodInsnNode(Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("signum"), U("(I)I"), false),
        new InsnNode(Opcodes::IRETURN),
    }, 1, 1, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
    // signum must not have been misrecognized as an intrinsic call site.  The
    // prelude still declares the intrinsic helpers, so only check the body by
    // excluding the declaration lines.
    EXPECT_EQ(result.source.find("= mj2x_math_"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("= _mj2x_intrinsic_parse_int"), std::string::npos)
        << result.source;
}

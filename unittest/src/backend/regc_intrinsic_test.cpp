// Unit tests for the RegC JDK-intrinsic recognition in the Call emitter path.
//
// These tests construct bytecode that calls java.lang.Math / Integer.parseInt
// and assert that the emitted C lowers those calls to mj2x_math_* /
// _mj2x_intrinsic_parse_int instead of the generic _mj2x_regc_call_* JNI
// dispatch helper.  The bodies are not executed here; that is the E2E suite's
// job.  Here we only verify the intrinsic is recognized and the JNI call
// surface is absent from the generated source.

#include "garena/arena.hpp"
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
    // #921 staticArena 退役：Type::staticArena() 已删除。测试夹具的节点名/描述符
    // 借用须活过本 helper 返回——用本测试文件私有静态池（测试专属，非产物路径）。
    static ca::str::Utf8StringArena     testArena;
    auto&                               arena = testArena;
    auto                                mn    = std::make_unique<MethodNode>(access,
                                           arena.intern(name.c_str()),
                                           arena.intern(desc.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack                              = maxStack;
    mn->maxLocals                             = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

// 注意：arena 必须活到 emitFunction 返回——MIR/SSA/RegIR 节点字段是 arena-backed
// Utf8StringRef（spec §6.1），arena 提前析构会让 emit 读到悬垂指针（参见 #698）。
// 生产侧 pipeline 把 arena 放在 CompilerIndex（job 级寿命）；测试侧把 arena 与 emit
// 放同一作用域，对齐该寿命约定。
RegCEmitResult emitRegC(const std::string& name, const std::string& desc,
                        std::initializer_list<AbstractInsnNode*> insns, int maxStack, int maxLocals,
                        std::string& reason)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto                     ssa = builder.build(blocks, "IntrinsicRegC", name, desc, true, maxLocals, arena, reason);
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
    ins.method.owner        = U(owner);
    ins.method.name         = U(name);
    ins.method.desc         = U(desc);
    ins.method.invokeOpcode = Opcodes::INVOKESTATIC;
    ins.args.assign(args.begin(), args.end());
    return ins;
}

}   // namespace

TEST(RegCIntrinsicRegistryTest, MatchesIntegerBitIntrinsicsThroughRegistry)
{
    // issue #778：Integer/Long 位操作族曾走通用 JNI helper（热循环 66x 差距），
    // registry 条目存在但必须验证 matchCall 真正命中并发射 runtime 符号。
    struct BitCase
    {
        const char* owner;
        const char* name;
        const char* desc;
        const char* symbol;
    };
    const BitCase kCases[] = {
        {"java/lang/Integer", "rotateLeft", "(II)I", "mj2x_intrinsic_int_rotate_left"},
        {"java/lang/Integer", "rotateRight", "(II)I", "mj2x_intrinsic_int_rotate_right"},
        {"java/lang/Integer", "reverse", "(I)I", "mj2x_intrinsic_int_reverse"},
        {"java/lang/Integer", "reverseBytes", "(I)I", "mj2x_intrinsic_int_reverse_bytes"},
        {"java/lang/Integer", "bitCount", "(I)I", "mj2x_intrinsic_int_bit_count"},
        {"java/lang/Integer",
         "numberOfLeadingZeros",
         "(I)I",
         "mj2x_intrinsic_int_leading_zeros"},
        {"java/lang/Integer",
         "numberOfTrailingZeros",
         "(I)I",
         "mj2x_intrinsic_int_trailing_zeros"},
        {"java/lang/Integer", "highestOneBit", "(I)I", "mj2x_intrinsic_int_highest_one_bit"},
        {"java/lang/Integer", "lowestOneBit", "(I)I", "mj2x_intrinsic_int_lowest_one_bit"},
        {"java/lang/Integer", "signum", "(I)I", "mj2x_intrinsic_int_signum"},
        {"java/lang/Long", "rotateLeft", "(JI)J", "mj2x_intrinsic_long_rotate_left"},
        {"java/lang/Long", "rotateRight", "(JI)J", "mj2x_intrinsic_long_rotate_right"},
        {"java/lang/Long", "reverse", "(J)J", "mj2x_intrinsic_long_reverse"},
        {"java/lang/Long", "reverseBytes", "(J)J", "mj2x_intrinsic_long_reverse_bytes"},
        {"java/lang/Long", "bitCount", "(J)I", "mj2x_intrinsic_long_bit_count"},
        {"java/lang/Long", "numberOfLeadingZeros", "(J)I", "mj2x_intrinsic_long_leading_zeros"},
        {"java/lang/Long", "numberOfTrailingZeros", "(J)I", "mj2x_intrinsic_long_trailing_zeros"},
        {"java/lang/Long", "highestOneBit", "(J)J", "mj2x_intrinsic_long_highest_one_bit"},
        {"java/lang/Long", "lowestOneBit", "(J)J", "mj2x_intrinsic_long_lowest_one_bit"},
        {"java/lang/Long", "signum", "(J)I", "mj2x_intrinsic_long_signum"},
    };
    for (const auto& c : kCases) {
        // desc 形如 `(II)I` / `(JI)J` / `(I)I` / `(J)I`：括号内字符数即参数个数。
        const std::string desc(c.desc);
        const size_t      argCount = desc.find(')') - 1;
        RegInstruction    ins      = makeStaticCall(c.owner, c.name, c.desc, {});
        ins.args.reserve(argCount);
        for (size_t i = 0; i < argCount; ++i) {
            const char ty = desc[1 + i];
            ins.args.push_back(ty == 'J' ? RegOperand::fromInt(1, SSAType::I64)
                                         : RegOperand::fromInt(1, SSAType::I32));
        }
        auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

        EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Emit) << c.owner << "." << c.name;
        EXPECT_EQ(decision.emitKind, RegCIntrinsicEmitKind::RuntimeFunction)
            << c.owner << "." << c.name;
        EXPECT_EQ(decision.symbol, c.symbol) << c.owner << "." << c.name;
    }
}

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

TEST(RegCIntrinsicRegistryTest, MatchesSystemArraycopy)
{
    auto decision = RegCIntrinsicRegistry::builtins().matchCall(makeStaticCall(
        "java/lang/System",
        "arraycopy",
        "(Ljava/lang/Object;ILjava/lang/Object;II)V",
        {RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}),
         RegOperand::fromInt(0, SSAType::I32),
         RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}),
         RegOperand::fromInt(1, SSAType::I32),
         RegOperand::fromInt(2, SSAType::I32)}));

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(decision.symbol, "mj2x_intrinsic_arraycopy");
    EXPECT_TRUE(decision.passContext);
    // RegCLocalHelper（非 RuntimeFunction）：arraycopy 会抛 NPE/AIOOBE，
    // 发射必须保留调用后 ExceptionCheck 与 local-ref 帧。
    EXPECT_EQ(decision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
}

TEST(RegCIntrinsicRegistryTest, MalformedIntrinsicArgumentCountIsUnsupported)
{
    auto ins = makeStaticCall("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", {});

    auto decision = RegCIntrinsicRegistry::builtins().matchCall(ins);

    EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Unsupported);
    EXPECT_NE(decision.reason.find("malformed RegIR argument count for intrinsic"),
              std::string::npos);
}

TEST(RegCIntrinsicRegistryTest, MatchesParseRadixIntrinsics)
{
    auto parseInt    = makeStaticCall("java/lang/Integer",
                                   "parseInt",
                                   "(Ljava/lang/String;I)I",
                                      {
                                       RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}),
                                       RegOperand::fromInt(16, SSAType::I32),
                                   });
    auto intDecision = RegCIntrinsicRegistry::builtins().matchCall(parseInt);
    EXPECT_EQ(intDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(intDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(intDecision.symbol, "_mj2x_intrinsic_parse_int_radix");
    EXPECT_TRUE(intDecision.passContext);

    auto parseLong    = makeStaticCall("java/lang/Long",
                                    "parseLong",
                                    "(Ljava/lang/String;I)J",
                                       {
                                        RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}),
                                        RegOperand::fromInt(36, SSAType::I32),
                                    });
    auto longDecision = RegCIntrinsicRegistry::builtins().matchCall(parseLong);
    EXPECT_EQ(longDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(longDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(longDecision.symbol, "_mj2x_intrinsic_parse_long_radix");
    EXPECT_TRUE(longDecision.passContext);
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
    unbox.method.owner        = U("java/lang/Integer");
    unbox.method.name         = U("intValue");
    unbox.method.desc         = U("()I");
    unbox.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    unbox.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto unboxDecision = RegCIntrinsicRegistry::builtins().matchCall(unbox);
    EXPECT_EQ(unboxDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(unboxDecision.emitKind, RegCIntrinsicEmitKind::WrapperUnbox);
    EXPECT_EQ(unboxDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::Int);
    EXPECT_EQ(unboxDecision.symbol, "mj2x_int_int_value");
}

TEST(RegCIntrinsicRegistryTest, MatchesWrapperToStringIntrinsics)
{
    auto byteToString = makeStaticCall("java/lang/Byte",
                                       "toString",
                                       "(B)Ljava/lang/String;",
                                       {
                                           RegOperand::fromInt(7, SSAType::I32),
                                       });
    auto byteDecision = RegCIntrinsicRegistry::builtins().matchCall(byteToString);
    EXPECT_EQ(byteDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(byteDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(byteDecision.symbol, "_mj2x_intrinsic_int_to_string");

    auto boolToString = makeStaticCall("java/lang/Boolean",
                                       "toString",
                                       "(Z)Ljava/lang/String;",
                                       {
                                           RegOperand::fromInt(1, SSAType::BOOL),
                                       });
    auto boolDecision = RegCIntrinsicRegistry::builtins().matchCall(boolToString);
    EXPECT_EQ(boolDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(boolDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(boolDecision.symbol, "_mj2x_intrinsic_bool_to_string");

    auto doubleToString =
        makeStaticCall("java/lang/Double",
                       "toString",
                       "(D)Ljava/lang/String;",
                       {
                           RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F64}),
                       });
    auto doubleDecision = RegCIntrinsicRegistry::builtins().matchCall(doubleToString);
    EXPECT_EQ(doubleDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(doubleDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(doubleDecision.symbol, "_mj2x_intrinsic_double_to_string");
}

TEST(RegCIntrinsicRegistryTest, MatchesWrapperCompareIntrinsics)
{
    auto byteCompare  = makeStaticCall("java/lang/Byte",
                                      "compare",
                                      "(BB)I",
                                       {
                                          RegOperand::fromInt(1, SSAType::I8),
                                          RegOperand::fromInt(2, SSAType::I8),
                                      });
    auto byteDecision = RegCIntrinsicRegistry::builtins().matchCall(byteCompare);
    EXPECT_EQ(byteDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(byteDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(byteDecision.symbol, "_mj2x_intrinsic_byte_compare");

    auto boolCompare  = makeStaticCall("java/lang/Boolean",
                                      "compare",
                                      "(ZZ)I",
                                       {
                                          RegOperand::fromInt(1, SSAType::BOOL),
                                          RegOperand::fromInt(0, SSAType::BOOL),
                                      });
    auto boolDecision = RegCIntrinsicRegistry::builtins().matchCall(boolCompare);
    EXPECT_EQ(boolDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(boolDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(boolDecision.symbol, "_mj2x_intrinsic_bool_compare");

    auto charCompare  = makeStaticCall("java/lang/Character",
                                      "compare",
                                      "(CC)I",
                                       {
                                          RegOperand::fromInt(1, SSAType::CHAR),
                                          RegOperand::fromInt(2, SSAType::CHAR),
                                      });
    auto charDecision = RegCIntrinsicRegistry::builtins().matchCall(charCompare);
    EXPECT_EQ(charDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(charDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(charDecision.symbol, "_mj2x_intrinsic_char_compare");

    auto floatCompare =
        makeStaticCall("java/lang/Float",
                       "compare",
                       "(FF)I",
                       {
                           RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F32}),
                           RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F32}),
                       });
    auto floatDecision = RegCIntrinsicRegistry::builtins().matchCall(floatCompare);
    EXPECT_EQ(floatDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(floatDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(floatDecision.symbol, "_mj2x_intrinsic_float_compare");

    auto doubleCompare =
        makeStaticCall("java/lang/Double",
                       "compare",
                       "(DD)I",
                       {
                           RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F64}),
                           RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F64}),
                       });
    auto doubleDecision = RegCIntrinsicRegistry::builtins().matchCall(doubleCompare);
    EXPECT_EQ(doubleDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(doubleDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(doubleDecision.symbol, "_mj2x_intrinsic_double_compare");
}

TEST(RegCIntrinsicRegistryTest, MatchesRandomInitAndNextInt)
{
    RegInstruction init;
    init.kind                = RegInstructionKind::Call;
    init.method.owner        = U("java/util/Random");
    init.method.name         = U("<init>");
    init.method.desc         = U("(J)V");
    init.method.invokeOpcode = Opcodes::INVOKESPECIAL;
    init.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    init.args.push_back(RegOperand::fromInt(42, SSAType::I64));

    auto initDecision = RegCIntrinsicRegistry::builtins().matchCall(init);
    EXPECT_EQ(initDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(initDecision.emitKind, RegCIntrinsicEmitKind::RandomInit);
    EXPECT_EQ(initDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::Random);

    RegInstruction next;
    next.kind                = RegInstructionKind::Call;
    next.method.owner        = U("java/util/Random");
    next.method.name         = U("nextInt");
    next.method.desc         = U("()I");
    next.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    next.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto nextDecision = RegCIntrinsicRegistry::builtins().matchCall(next);
    EXPECT_EQ(nextDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(nextDecision.emitKind, RegCIntrinsicEmitKind::NativeObjectMethod);
    EXPECT_EQ(nextDecision.symbol, "mj2x_random_next_int");
    // #879 Y4：预检位随权威表单源——无参面 None，bound 面由 decision 透传
    // PositiveBound（发射端按位发预检，不再 symbol 硬编码）。
    EXPECT_EQ(nextDecision.precheck, RegCIntrinsicPrecheck::None);

    RegInstruction nextBound;
    nextBound.kind                = RegInstructionKind::Call;
    nextBound.method.owner        = U("java/util/Random");
    nextBound.method.name         = U("nextInt");
    nextBound.method.desc         = U("(I)I");
    nextBound.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    nextBound.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    nextBound.args.push_back(RegOperand::fromInt(100, SSAType::I32));

    auto nextBoundDecision = RegCIntrinsicRegistry::builtins().matchCall(nextBound);
    EXPECT_EQ(nextBoundDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(nextBoundDecision.emitKind, RegCIntrinsicEmitKind::NativeObjectMethod);
    EXPECT_EQ(nextBoundDecision.symbol, "mj2x_random_next_int_bound");
    EXPECT_EQ(nextBoundDecision.precheck, RegCIntrinsicPrecheck::PositiveBound);
}

TEST(RegCIntrinsicRegistryTest, MatchesStringBuilderIntrinsics)
{
    RegInstruction init;
    init.kind                = RegInstructionKind::Call;
    init.method.owner        = U("java/lang/StringBuilder");
    init.method.name         = U("<init>");
    init.method.desc         = U("()V");
    init.method.invokeOpcode = Opcodes::INVOKESPECIAL;
    init.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto initDecision = RegCIntrinsicRegistry::builtins().matchCall(init);
    EXPECT_EQ(initDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(initDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderInit);
    EXPECT_EQ(initDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);

    RegInstruction append;
    append.kind                = RegInstructionKind::Call;
    append.method.owner        = U("java/lang/StringBuilder");
    append.method.name         = U("append");
    append.method.desc         = U("(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    append.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    append.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    append.args.push_back(RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}));

    auto appendDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);

    append.method.desc        = U("(Ljava/lang/Object;)Ljava/lang/StringBuilder;");
    append.args[1]            = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});
    auto appendObjectDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendObjectDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendObjectDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendObjectDecision.symbol, "mj2x_string_builder_append_object");

    append.method.desc              = U("(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;");
    append.args[1]                  = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});
    auto appendCharSequenceDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendCharSequenceDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendCharSequenceDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendCharSequenceDecision.symbol, "mj2x_string_builder_append_char_sequence");

    append.method.desc      = U("(Z)Ljava/lang/StringBuilder;");
    append.args[1]          = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::BOOL});
    auto appendBoolDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendBoolDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendBoolDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendBoolDecision.symbol, "mj2x_string_builder_append_bool");

    append.method.desc      = U("(C)Ljava/lang/StringBuilder;");
    append.args[1]          = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::CHAR});
    auto appendCharDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendCharDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendCharDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendCharDecision.symbol, "mj2x_string_builder_append_char");

    append.method.desc       = U("([C)Ljava/lang/StringBuilder;");
    append.args[1]           = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});
    auto appendCharsDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendCharsDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendCharsDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendCharsDecision.symbol, "mj2x_string_builder_append_char_array");

    append.method.desc       = U("(F)Ljava/lang/StringBuilder;");
    append.args[1]           = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F32});
    auto appendFloatDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendFloatDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendFloatDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendFloatDecision.symbol, "mj2x_string_builder_append_f32");

    append.method.desc        = U("(D)Ljava/lang/StringBuilder;");
    append.args[1]            = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::F64});
    auto appendDoubleDecision = RegCIntrinsicRegistry::builtins().matchCall(append);
    EXPECT_EQ(appendDoubleDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(appendDoubleDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(appendDoubleDecision.symbol, "mj2x_string_builder_append_f64");

    RegInstruction toString;
    toString.kind                = RegInstructionKind::Call;
    toString.method.owner        = U("java/lang/StringBuilder");
    toString.method.name         = U("toString");
    toString.method.desc         = U("()Ljava/lang/String;");
    toString.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    toString.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto toStringDecision = RegCIntrinsicRegistry::builtins().matchCall(toString);
    EXPECT_EQ(toStringDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(toStringDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderMethod);
    EXPECT_EQ(toStringDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuilder);
}

TEST(RegCIntrinsicRegistryTest, MatchesMutableStringRangeCapacityAndBufferIntrinsics)
{
    RegInstruction call;
    call.kind                = RegInstructionKind::Call;
    call.method.owner        = U("java/lang/StringBuilder");
    call.method.name         = U("append");
    call.method.desc         = U("(Ljava/lang/CharSequence;II)Ljava/lang/StringBuilder;");
    call.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    call.args                = {
        RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}),
        RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}),
        RegOperand::fromInt(1, SSAType::I32),
        RegOperand::fromInt(3, SSAType::I32),
    };
    auto rangeDecision = RegCIntrinsicRegistry::builtins().matchCall(call);
    EXPECT_EQ(rangeDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(rangeDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderAppend);
    EXPECT_EQ(rangeDecision.symbol, "mj2x_string_builder_append_char_sequence_range");

    call.method.name = U("capacity");
    call.method.desc = U("()I");
    call.args.resize(1);
    auto capacityDecision = RegCIntrinsicRegistry::builtins().matchCall(call);
    EXPECT_EQ(capacityDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(capacityDecision.emitKind, RegCIntrinsicEmitKind::StringBuilderMethod);
    EXPECT_EQ(capacityDecision.symbol, "mj2x_string_builder_capacity");

    call.method.owner = U("java/lang/StringBuffer");
    call.method.name  = U("append");
    call.method.desc  = U("([CII)Ljava/lang/StringBuffer;");
    call.args         = {
        RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}),
        RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}),
        RegOperand::fromInt(0, SSAType::I32),
        RegOperand::fromInt(2, SSAType::I32),
    };
    auto bufferDecision = RegCIntrinsicRegistry::builtins().matchCall(call);
    EXPECT_EQ(bufferDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(bufferDecision.nativeObjectKind, RegCIntrinsicNativeObjectKind::StringBuffer);
    EXPECT_EQ(bufferDecision.symbol, "mj2x_string_buffer_append_char_array_range");

    call.method.owner = U("java/lang/String");
    call.method.name  = U("split");
    call.method.desc  = U("(Ljava/lang/String;I)[Ljava/lang/String;");
    call.args.resize(3);
    auto splitDecision = RegCIntrinsicRegistry::builtins().matchCall(call);
    EXPECT_EQ(splitDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(splitDecision.emitKind, RegCIntrinsicEmitKind::StringRuntimeMethod);
    EXPECT_EQ(splitDecision.symbol, "mj2x_string_split_limit");
}

TEST(RegCIntrinsicRegistryTest, MatchesStringIntrinsics)
{
    RegInstruction length;
    length.kind                = RegInstructionKind::Call;
    length.method.owner        = U("java/lang/String");
    length.method.name         = U("length");
    length.method.desc         = U("()I");
    length.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    length.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto lengthDecision = RegCIntrinsicRegistry::builtins().matchCall(length);
    EXPECT_EQ(lengthDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(lengthDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(lengthDecision.symbol, "_mj2x_intrinsic_string_length");

    RegInstruction charAt;
    charAt.kind                = RegInstructionKind::Call;
    charAt.method.owner        = U("java/lang/String");
    charAt.method.name         = U("charAt");
    charAt.method.desc         = U("(I)C");
    charAt.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    charAt.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    charAt.args.push_back(RegOperand::fromInt(1, SSAType::I32));

    auto charAtDecision = RegCIntrinsicRegistry::builtins().matchCall(charAt);
    // charAt/hashCode/equals 不再走 StringMethod intrinsic：多 JNI 数据管道
    // 比回退缓存 methodID 的 1-JNI CallXxxMethodA 更慢（HotSpot 对这三个方法
    // 有 VM intrinsic + memoization），保留 length/isEmpty 等 1-JNI 平价项。
    EXPECT_EQ(charAtDecision.kind, RegCIntrinsicDecisionKind::Fallback);

    RegInstruction isEmpty;
    isEmpty.kind                = RegInstructionKind::Call;
    isEmpty.method.owner        = U("java/lang/String");
    isEmpty.method.name         = U("isEmpty");
    isEmpty.method.desc         = U("()Z");
    isEmpty.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    isEmpty.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));

    auto isEmptyDecision = RegCIntrinsicRegistry::builtins().matchCall(isEmpty);
    EXPECT_EQ(isEmptyDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(isEmptyDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(isEmptyDecision.symbol, "_mj2x_intrinsic_string_is_empty");

    RegInstruction contains;
    contains.kind                = RegInstructionKind::Call;
    contains.method.owner        = U("java/lang/String");
    contains.method.name         = U("contains");
    contains.method.desc         = U("(Ljava/lang/CharSequence;)Z");
    contains.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
    contains.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF}));
    contains.args.push_back(RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF}));

    auto containsDecision = RegCIntrinsicRegistry::builtins().matchCall(contains);
    EXPECT_EQ(containsDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(containsDecision.emitKind, RegCIntrinsicEmitKind::StringMethod);
    EXPECT_EQ(containsDecision.symbol, "_mj2x_intrinsic_string_contains");

    RegInstruction valueOf;
    valueOf.kind                = RegInstructionKind::Call;
    valueOf.method.owner        = U("java/lang/String");
    valueOf.method.name         = U("valueOf");
    valueOf.method.desc         = U("(D)Ljava/lang/String;");
    valueOf.method.invokeOpcode = Opcodes::INVOKESTATIC;
    valueOf.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F64}));

    auto valueOfDecision = RegCIntrinsicRegistry::builtins().matchCall(valueOf);
    EXPECT_EQ(valueOfDecision.kind, RegCIntrinsicDecisionKind::Emit);
    EXPECT_EQ(valueOfDecision.emitKind, RegCIntrinsicEmitKind::RegCLocalHelper);
    EXPECT_EQ(valueOfDecision.symbol, "_mj2x_intrinsic_double_to_string");
}

TEST(RegCIntrinsicRegistryTest, MatchesUtf16StringRuntimeIntrinsics)
{
    auto makeCall = [](const char* name, const char* desc, std::vector<RegOperand> args) {
        RegInstruction call;
        call.kind                = RegInstructionKind::Call;
        call.method.owner        = U("java/lang/String");
        call.method.name         = U(name);
        call.method.desc         = U(desc);
        call.method.invokeOpcode = Opcodes::INVOKEVIRTUAL;
        call.args                = std::move(args);
        return call;
    };
    const RegOperand receiver  = RegOperand::fromRegister(RegValue{RegId{1}, SSAType::REF});
    const RegOperand stringArg = RegOperand::fromRegister(RegValue{RegId{2}, SSAType::REF});

    const std::vector<std::pair<RegInstruction, std::string>> cases = {
        {makeCall("concat", "(Ljava/lang/String;)Ljava/lang/String;", {receiver, stringArg}),
         "mj2x_string_concat"},
        {makeCall("compareTo", "(Ljava/lang/String;)I", {receiver, stringArg}),
         "mj2x_string_compare_to"},
        {makeCall("indexOf", "(I)I", {receiver, RegOperand::fromInt(65, SSAType::I32)}),
         "mj2x_string_index_of_char"},
        {makeCall("indexOf",
                  "(II)I",
                  {receiver,
                   RegOperand::fromInt(65, SSAType::I32),
                   RegOperand::fromInt(1, SSAType::I32)}),
         "mj2x_string_index_of_char_from"},
        {makeCall("indexOf", "(Ljava/lang/String;)I", {receiver, stringArg}),
         "mj2x_string_index_of_string"},
        {makeCall("indexOf",
                  "(Ljava/lang/String;I)I",
                  {receiver, stringArg, RegOperand::fromInt(1, SSAType::I32)}),
         "mj2x_string_index_of_string_from"},
        {makeCall("replace",
                  "(CC)Ljava/lang/String;",
                  {receiver,
                   RegOperand::fromInt('a', SSAType::CHAR),
                   RegOperand::fromInt('b', SSAType::CHAR)}),
         "mj2x_string_replace_char"},
        {makeCall("trim", "()Ljava/lang/String;", {receiver}), "mj2x_string_trim"},
    };

    for (const auto& [call, symbol] : cases) {
        auto decision = RegCIntrinsicRegistry::builtins().matchCall(call);
        EXPECT_EQ(decision.kind, RegCIntrinsicDecisionKind::Emit) << symbol;
        EXPECT_EQ(decision.emitKind, RegCIntrinsicEmitKind::StringRuntimeMethod) << symbol;
        EXPECT_EQ(decision.symbol, symbol);
    }
    // substring(I)/(II) 已回退：CRT 版本 4 JNI + 全串拷贝，劣于 1-JNI 缓存
    // methodID 的 JDK 调用（String 底层是拷贝共享数组，JDK substring 本就 O(1)）。
    auto subFrom = makeCall(
        "substring", "(I)Ljava/lang/String;", {receiver, RegOperand::fromInt(1, SSAType::I32)});
    EXPECT_EQ(RegCIntrinsicRegistry::builtins().matchCall(subFrom).kind,
              RegCIntrinsicDecisionKind::Fallback);
    auto subRange = makeCall("substring",
                             "(II)Ljava/lang/String;",
                             {receiver,
                              RegOperand::fromInt(1, SSAType::I32),
                              RegOperand::fromInt(2, SSAType::I32)});
    EXPECT_EQ(RegCIntrinsicRegistry::builtins().matchCall(subRange).kind,
              RegCIntrinsicDecisionKind::Fallback);
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

TEST(RegCIntrinsicTest, ParseRadixLoweredToIntrinsicNotJni)
{
    std::string reason;
    // static long parseRadix(String i, String l) {
    //   return Integer.parseInt(i, 16) + Long.parseLong(l, 36);
    // }
    auto result = emitRegC("parseRadix",
                           "(Ljava/lang/String;Ljava/lang/String;)J",
                           {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new IntInsnNode(Opcodes::BIPUSH, 16),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Integer"),
                                                  U("parseInt"),
                                                  U("(Ljava/lang/String;I)I"),
                                                  false),
                               new InsnNode(Opcodes::I2L),
                               new VarInsnNode(Opcodes::ALOAD, 1),
                               new IntInsnNode(Opcodes::BIPUSH, 36),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Long"),
                                                  U("parseLong"),
                                                  U("(Ljava/lang/String;I)J"),
                                                  false),
                               new InsnNode(Opcodes::LADD),
                               new InsnNode(Opcodes::LRETURN),
                           },
                           3,
                           2,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_int_radix("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_parse_long_radix("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"parseInt\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i64(\"java/lang/Long\", \"parseLong\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, StringMethodsLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int strings(String s, Object o) {
    //   return s.length() + s.charAt(1) + s.hashCode() + (s.equals(o) ? 1 : 0);
    // }
    auto result = emitRegC(
        "strings",
        "(Ljava/lang/String;Ljava/lang/Object;)I",
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("length"), U("()I"), false),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("charAt"), U("(I)C"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("hashCode"), U("()I"), false),
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
    // charAt/hashCode/equals 回退缓存 methodID 调用（1 JNI），不再发射
    // StringMethod intrinsic（多 JNI 数据管道实测更慢）。
    EXPECT_EQ(result.source.find("_mj2x_intrinsic_string_char_at("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_intrinsic_string_hash_code("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_intrinsic_string_equals("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("java/lang/String\", \"length\""), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("java/lang/String\", \"charAt\""), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("java/lang/String\", \"hashCode\""), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("java/lang/String\", \"equals\""), std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, DiscardedStringIntrinsicReturnStillLowers)
{
    std::string reason;
    // static int discard(String s) { s.length(); return 7; }
    auto result = emitRegC(
        "discardStringLength",
        "(Ljava/lang/String;)I",
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("length"), U("()I"), false),
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
    auto result = emitRegC(
        "moreStrings",
        "(Ljava/lang/String;)I",
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("isEmpty"), U("()Z"), false),
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
    EXPECT_NE(result.source.find("mj2x_string_index_of_string("), std::string::npos)
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

TEST(RegCIntrinsicTest, Utf16StringOperationsLowerToCrtAbi)
{
    std::string reason;
    auto        result = emitRegC(
        "utf16Strings",
        "(Ljava/lang/String;Ljava/lang/String;)I",
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("concat"),
                               U("(Ljava/lang/String;)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("substring"),
                               U("(I)Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ICONST_0),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("substring"),
                               U("(II)Ljava/lang/String;"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("compareTo"),
                               U("(Ljava/lang/String;)I"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new LdcInsnNode(static_cast<int32_t>(0x1F600)),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("indexOf"), U("(I)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new IntInsnNode(Opcodes::BIPUSH, 'x'),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("indexOf"), U("(II)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("indexOf"),
                               U("(Ljava/lang/String;)I"),
                               false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new InsnNode(Opcodes::ICONST_1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("indexOf"),
                               U("(Ljava/lang/String;I)I"),
                               false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new IntInsnNode(Opcodes::BIPUSH, 'a'),
            new IntInsnNode(Opcodes::BIPUSH, 'b'),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("replace"),
                               U("(CC)Ljava/lang/String;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/String"),
                               U("trim"),
                               U("()Ljava/lang/String;"),
                               false),
            new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL, U("java/lang/String"), U("length"), U("()I"), false),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        5,
        2,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    const char* symbols[] = {
        "mj2x_string_concat(",
        "mj2x_string_compare_to(",
        "mj2x_string_index_of_char(",
        "mj2x_string_index_of_char_from(",
        "mj2x_string_index_of_string(",
        "mj2x_string_index_of_string_from(",
        "mj2x_string_replace_char(",
        "mj2x_string_trim(",
    };
    for (const char* symbol : symbols) {
        EXPECT_NE(result.source.find(symbol), std::string::npos) << symbol << "\n" << result.source;
    }
    // substring(I)/(II) 回退为缓存 methodID 的 1-JNI JDK 调用（CRT 版本 4 JNI +
    // 全串拷贝更慢），因此允许 substring 出现 JNI helper，其余方法不允许。
    EXPECT_NE(result.source.find("java/lang/String\", \"substring\""), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("\"concat\""), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("\"compareTo\""), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("\"indexOf\""), std::string::npos) << result.source;
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
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_int_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_long_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static mj2x_ref _mj2x_intrinsic_int_to_string"),
              std::string::npos)
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

TEST(RegCIntrinsicTest, RemainingWrapperParsersUseDedicatedIntrinsics)
{
    std::string reason;
    auto        result = emitRegC("parseWrappers",
                           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                                  "String;Ljava/lang/String;)D",
                                  {
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Short"),
                                                  U("parseShort"),
                                                  U("(Ljava/lang/String;)S"),
                                                  false),
                               new InsnNode(Opcodes::I2D),
                               new VarInsnNode(Opcodes::ALOAD, 0),
                               new IntInsnNode(Opcodes::BIPUSH, 16),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Short"),
                                                  U("parseShort"),
                                                  U("(Ljava/lang/String;I)S"),
                                                  false),
                               new InsnNode(Opcodes::I2D),
                               new InsnNode(Opcodes::DADD),
                               new VarInsnNode(Opcodes::ALOAD, 1),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Byte"),
                                                  U("parseByte"),
                                                  U("(Ljava/lang/String;)B"),
                                                  false),
                               new InsnNode(Opcodes::I2D),
                               new InsnNode(Opcodes::DADD),
                               new VarInsnNode(Opcodes::ALOAD, 1),
                               new IntInsnNode(Opcodes::BIPUSH, 16),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Byte"),
                                                  U("parseByte"),
                                                  U("(Ljava/lang/String;I)B"),
                                                  false),
                               new InsnNode(Opcodes::I2D),
                               new InsnNode(Opcodes::DADD),
                               new VarInsnNode(Opcodes::ALOAD, 2),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Boolean"),
                                                  U("parseBoolean"),
                                                  U("(Ljava/lang/String;)Z"),
                                                  false),
                               new InsnNode(Opcodes::I2D),
                               new InsnNode(Opcodes::DADD),
                               new VarInsnNode(Opcodes::ALOAD, 3),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Float"),
                                                  U("parseFloat"),
                                                  U("(Ljava/lang/String;)F"),
                                                  false),
                               new InsnNode(Opcodes::F2D),
                               new InsnNode(Opcodes::DADD),
                               new VarInsnNode(Opcodes::ALOAD, 4),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Double"),
                                                  U("parseDouble"),
                                                  U("(Ljava/lang/String;)D"),
                                                  false),
                               new InsnNode(Opcodes::DADD),
                               new InsnNode(Opcodes::DRETURN),
                           },
                           6,
                           5,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    const char* symbols[] = {
        "_mj2x_intrinsic_parse_short(",
        "_mj2x_intrinsic_parse_short_radix(",
        "_mj2x_intrinsic_parse_byte(",
        "_mj2x_intrinsic_parse_byte_radix(",
        "_mj2x_intrinsic_parse_boolean(",
        "_mj2x_intrinsic_parse_float(",
        "_mj2x_intrinsic_parse_double(",
    };
    for (const char* symbol : symbols) {
        EXPECT_NE(result.source.find(symbol), std::string::npos) << symbol << "\n" << result.source;
    }
    EXPECT_EQ(result.source.find("_mj2x_regc_call_"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, MoreWrapperToStringLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int wrappers(byte b, short s, boolean z, char c, float f, double d) {
    //   Byte.toString(b); Short.toString(s); Boolean.toString(z); Character.toString(c);
    //   Float.toString(f); Double.toString(d); return 0;
    // }
    auto result = emitRegC("moreWrapperStrings",
                           "(BSZCFD)I",
                           {
                               new VarInsnNode(Opcodes::ILOAD, 0),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Byte"),
                                                  U("toString"),
                                                  U("(B)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::ILOAD, 1),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Short"),
                                                  U("toString"),
                                                  U("(S)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::ILOAD, 2),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Boolean"),
                                                  U("toString"),
                                                  U("(Z)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::ILOAD, 3),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Character"),
                                                  U("toString"),
                                                  U("(C)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::FLOAD, 4),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Float"),
                                                  U("toString"),
                                                  U("(F)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new VarInsnNode(Opcodes::DLOAD, 5),
                               new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                  U("java/lang/Double"),
                                                  U("toString"),
                                                  U("(D)Ljava/lang/String;"),
                                                  false),
                               new InsnNode(Opcodes::POP),
                               new InsnNode(Opcodes::ICONST_0),
                               new InsnNode(Opcodes::IRETURN),
                           },
                           2,
                           7,
                           reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_int_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_bool_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_char_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_float_to_string("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_double_to_string("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Byte\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Short\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Boolean\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Character\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Float\", \"toString\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/Double\", \"toString\""),
              std::string::npos)
        << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCIntrinsicTest, MoreWrapperCompareLoweredToIntrinsicsNotJni)
{
    std::string reason;
    // static int compares(byte b, short s, boolean z, char c, float f, double d) {
    //   return Byte.compare(b, (byte)7) + Short.compare(s, (short)3)
    //       + Boolean.compare(z, false) + Character.compare(c, 'A')
    //       + Float.compare(f, 0.0f) + Double.compare(d, 0.0d);
    // }
    auto result = emitRegC(
        "moreWrapperCompares",
        "(BSZCFD)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new IntInsnNode(Opcodes::BIPUSH, 7),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Byte"), U("compare"), U("(BB)I"), false),
            new VarInsnNode(Opcodes::ILOAD, 1),
            new InsnNode(Opcodes::ICONST_3),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Short"), U("compare"), U("(SS)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ILOAD, 2),
            new InsnNode(Opcodes::ICONST_0),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Boolean"), U("compare"), U("(ZZ)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ILOAD, 3),
            new IntInsnNode(Opcodes::BIPUSH, 65),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Character"), U("compare"), U("(CC)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::FLOAD, 4),
            new InsnNode(Opcodes::FCONST_0),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Float"), U("compare"), U("(FF)I"), false),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::DLOAD, 5),
            new InsnNode(Opcodes::DCONST_0),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Double"), U("compare"), U("(DD)I"), false),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        4,
        7,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_byte_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_short_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_bool_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_char_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_float_compare("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_intrinsic_double_compare("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_byte_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_short_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_bool_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_char_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_float_compare"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static int32_t _mj2x_intrinsic_double_compare"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Byte\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Short\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Boolean\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Character\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Float\", \"compare\""),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Double\", \"compare\""),
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
    auto result = emitRegC("stringValues",
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
    EXPECT_EQ(result.source.find("static mj2x_ref _mj2x_intrinsic_int_to_string"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static mj2x_ref _mj2x_intrinsic_long_to_string"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static mj2x_ref _mj2x_intrinsic_bool_to_string"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("static mj2x_ref _mj2x_intrinsic_char_to_string"),
              std::string::npos)
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

TEST(RegCIntrinsicTest, EscapingWrapperValueUsesCachedBoxHelper)
{
    std::string reason;
    // static Object boxed(int x) { return Integer.valueOf(x); }
    // #887 O3 装箱缓存（a0e0c021d）：逃逸装箱走 _mj2x_regc_box_i32 缓存 helper
    // （JLS 5.1.7 恒一性），不再回落通用 valueOf JNI 派发；执行级语义由
    // RegCSafeEnvE2E 数值断言覆盖。
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
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_box_i32("), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("java/lang/Integer"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("valueOf"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("mj2x_int native_r"), std::string::npos) << result.source;
}

TEST(RegCIntrinsicTest, WrapperFieldAndUnknownCallEscapesFallBack)
{
    std::string reason;
    auto        fieldResult = emitRegC(
        "storeBox",
        "(I)V",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Integer"),
                               U("valueOf"),
                               U("(I)Ljava/lang/Integer;"),
                               false),
            new FieldInsnNode(Opcodes::PUTSTATIC, U("Foo"), U("sink"), U("Ljava/lang/Object;")),
            new InsnNode(Opcodes::RETURN),
        },
        1,
        1,
        reason);
    ASSERT_TRUE(fieldResult.ok) << fieldResult.reason;
    EXPECT_EQ(fieldResult.source.find("mj2x_int native_r"), std::string::npos)
        << fieldResult.source;
    EXPECT_NE(fieldResult.source.find("_mj2x_regc_store_"), std::string::npos)
        << fieldResult.source;

    auto callResult = emitRegC(
        "consumeBox",
        "(I)V",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               U("java/lang/Integer"),
                               U("valueOf"),
                               U("(I)Ljava/lang/Integer;"),
                               false),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("Foo"), U("consume"), U("(Ljava/lang/Object;)V"), false),
            new InsnNode(Opcodes::RETURN),
        },
        1,
        1,
        reason);
    ASSERT_TRUE(callResult.ok) << callResult.reason;
    EXPECT_EQ(callResult.source.find("mj2x_int native_r"), std::string::npos) << callResult.source;
    EXPECT_NE(callResult.source.find("{\"Foo\", \"consume\", \"(Ljava/lang/Object;)V\""),
              std::string::npos)
        << callResult.source;
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
    // static String build(String s, int x, Object o, CharSequence cs, float f, double d, char[]
    // chars) {
    //   return new
    //   StringBuilder().append(s).append(x).append(7L).append(true).append('!').append(chars).append(f).append(d).append(o).append(cs).toString();
    // }
    auto result = emitRegC(
        "buildString",
        "(Ljava/lang/String;ILjava/lang/Object;Ljava/lang/CharSequence;FD[C)Ljava/lang/String;",
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
            new VarInsnNode(Opcodes::ALOAD, 7),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("([C)Ljava/lang/StringBuilder;"),
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
        8,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_string_builder* native_owner_r"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_init(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_jstring(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_i32(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_i64(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_bool(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_char(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(
        result.source.find("mj2x_string_builder_append_char_array(_mj2x_regc_current_context()"),
        std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_f32(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_f64(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_append_object(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_ref(\"java/lang/StringBuilder\", \"append\""),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_to_jstring(_mj2x_regc_current_context()"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_destroy(native_owner_r"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("mj2x_regc_local_string_builder"), std::string::npos)
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

TEST(RegCIntrinsicTest, ReturningStringBuilderMaterializesAndCleansUpNativeState)
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

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_string_builder_materialize("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_destroy(native_owner_r"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("goto _mj2x_regc_cleanup;"), std::string::npos) << result.source;
}

TEST(RegCIntrinsicTest, StringBuilderUnknownCallArgumentMaterializesLazily)
{
    std::string reason;
    auto        result = emitRegC(
        "consumeBuilder",
        "()V",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new LdcInsnNode(U("value")),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                               false),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("Foo"), U("consume"), U("(Ljava/lang/Object;)V"), false),
            new InsnNode(Opcodes::RETURN),
        },
        3,
        0,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_string_builder_materialize("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("native_materialized_r"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("{\"Foo\", \"consume\", \"(Ljava/lang/Object;)V\", NULL, NULL}"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuilder\")"),
              std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringBuilderFieldAndArrayStoresMaterialize)
{
    std::string reason;
    auto        fieldResult = emitRegC(
        "storeBuilderField",
        "()V",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new FieldInsnNode(Opcodes::PUTSTATIC, U("Foo"), U("sink"), U("Ljava/lang/Object;")),
            new InsnNode(Opcodes::RETURN),
        },
        2,
        0,
        reason);
    ASSERT_TRUE(fieldResult.ok) << fieldResult.reason;
    EXPECT_NE(fieldResult.source.find("mj2x_string_builder_materialize("), std::string::npos)
        << fieldResult.source;
    EXPECT_NE(fieldResult.source.find("_mj2x_regc_store_cached_ref_field"), std::string::npos)
        << fieldResult.source;

    auto arrayResult = emitRegC(
        "storeBuilderArray",
        "()V",
        {
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY, U("java/lang/Object")),
            new InsnNode(Opcodes::ICONST_0),
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new InsnNode(Opcodes::AASTORE),
            new InsnNode(Opcodes::RETURN),
        },
        4,
        0,
        reason);
    ASSERT_TRUE(arrayResult.ok) << arrayResult.reason;
    EXPECT_NE(arrayResult.source.find("mj2x_string_builder_materialize("), std::string::npos)
        << arrayResult.source;
    EXPECT_NE(arrayResult.source.find("_mj2x_regc_store_ref_array"), std::string::npos)
        << arrayResult.source;
}

TEST(RegCIntrinsicTest, StringBuilderCfgAliasKeepsOpaqueHandle)
{
    auto*       skipAppend = new LabelNode(labelScratchArena().create<Label>());
    std::string reason;
    auto        result = emitRegC(
        "cfgBuilder",
        "(Z)Ljava/lang/String;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new VarInsnNode(Opcodes::ASTORE, 1),
            new VarInsnNode(Opcodes::ILOAD, 0),
            new JumpInsnNode(Opcodes::IFEQ, skipAppend->label),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new LdcInsnNode(U("x")),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                               false),
            new InsnNode(Opcodes::POP),
            skipAppend,
            new VarInsnNode(Opcodes::ALOAD, 1),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("toString"),
                               U("()Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        3,
        2,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("edge_tmp_"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_string_builder_to_jstring("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuilder\")"),
              std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringBuilderUseAfterEscapeFallsBackToJavaObject)
{
    std::string reason;
    auto        result = emitRegC(
        "mutateAfterEscape",
        "()Ljava/lang/String;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuilder")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuilder"), U("<init>"), U("()V"), false),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("Foo"), U("consume"), U("(Ljava/lang/Object;)V"), false),
            new LdcInsnNode(U("x")),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuilder"),
                               U("toString"),
                               U("()Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        3,
        0,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuilder\")"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("mj2x_string_builder_materialize("), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringBufferChainUsesSynchronizedOpaqueState)
{
    std::string reason;
    auto        result = emitRegC(
        "bufferChain",
        "()Ljava/lang/String;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuffer")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuffer"), U("<init>"), U("()V"), false),
            new LdcInsnNode(U("x")),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuffer"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuffer;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuffer"),
                               U("toString"),
                               U("()Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        3,
        0,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_string_buffer_init("), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("mj2x_string_buffer_append_jstring("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_buffer_to_jstring("), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("mj2x_string_buffer_destroy("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuffer\")"),
              std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, StringBufferUseAfterEscapeFallsBackToJavaObject)
{
    std::string reason;
    auto        result = emitRegC(
        "bufferMutateAfterEscape",
        "()Ljava/lang/String;",
        {
            new TypeInsnNode(Opcodes::NEW, U("java/lang/StringBuffer")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESPECIAL, U("java/lang/StringBuffer"), U("<init>"), U("()V"), false),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("Foo"), U("consume"), U("(Ljava/lang/Object;)V"), false),
            new LdcInsnNode(U("x")),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuffer"),
                               U("append"),
                               U("(Ljava/lang/String;)Ljava/lang/StringBuffer;"),
                               false),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               U("java/lang/StringBuffer"),
                               U("toString"),
                               U("()Ljava/lang/String;"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        3,
        0,
        reason);

    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("_mj2x_regc_alloc_object(\"java/lang/StringBuffer\")"),
              std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("mj2x_string_buffer_materialize("), std::string::npos)
        << result.source;
}

TEST(RegCIntrinsicTest, NonIntrinsicCallStillUsesJniHelper)
{
    std::string reason;
    // An unhandled JDK call (Integer.remainderUnsigned) must fall through to the
    // generic JNI call helper so we never silently drop a call site.
    auto result = emitRegC(
        "remainderUnsigned",
        "(II)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new VarInsnNode(Opcodes::ILOAD, 1),
            new MethodInsnNode(
                Opcodes::INVOKESTATIC, U("java/lang/Integer"), U("remainderUnsigned"), U("(II)I"), false),
            new InsnNode(Opcodes::IRETURN),
        },
        2,
        2,
        reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("mj2x_regc_static_method_cache"), std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("{\"java/lang/Integer\", \"remainderUnsigned\", \"(II)I\", NULL, NULL}"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_cached_static_method(&_mj2x_regc_cache_"),
              std::string::npos)
        << result.source;
    EXPECT_NE(result.source.find("(*_mj2x_regc_env)->CallStaticIntMethodA("), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_cached_i32"), std::string::npos)
        << result.source;
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"java/lang/Integer\""), std::string::npos)
        << result.source;
    // remainderUnsigned must not have been misrecognized as an intrinsic call site.
    EXPECT_EQ(result.source.find("= mj2x_math_"), std::string::npos) << result.source;
    EXPECT_EQ(result.source.find("= _mj2x_intrinsic_parse_int"), std::string::npos)
        << result.source;
}

// ---- cmp.nan_* intrinsic（LCMP/FCMPG/FCMPL/DCMPG/DCMPL 三值比较）----
//
// ssa_builder 把这 5 个比较 opcode 统一翻译为 cmp.nan_greater（FCMPG/DCMPG）或
// cmp.nan_less（LCMP/FCMPL/DCMPL）intrinsic。emitter 按 args[0].type 分派：浮点
// (F32/F64) 走 NaN-aware 三值，整型 (I64/LCMP) 走无 NaN 三值。这里既验证全链路
// (策略 A，opcode → SSA → RegIR → emitter)，也验证 emitter 直接发射 (策略 B)。

/// 直接构造一个仅含单条 cmp.nan_* Intrinsic 指令的 RegFunction，用于精确检查发射的 C 表达式。
RegCEmitResult emitCmpNanIntrinsic(const char* intrinsicName, SSAType operandType,
                                   std::string& reason)
{
    auto block  = std::make_unique<RegBlock>(SSABlockId{0});
    RegBlock* b = block.get();

    RegInstruction ins;
    ins.kind          = RegInstructionKind::Intrinsic;
    ins.hasDest       = true;
    ins.dest          = RegValue{RegId{3}, SSAType::I32};
    ins.intrinsicName = intrinsicName;
    ins.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, operandType}));
    ins.args.push_back(RegOperand::fromRegister(RegValue{RegId{2}, operandType}));
    b->instructions.push_back(std::move(ins));

    RegReturnTerminator ret;
    ret.hasValue = true;
    ret.value    = RegOperand::fromRegister(RegValue{RegId{3}, SSAType::I32});
    b->terminator = std::move(ret);

    RegFunction fn;
    fn.symbol.owner      = "CmpNanRegC";
    fn.symbol.name       = "cmp";
    fn.symbol.desc       = "()I";
    fn.symbol.returnType = SSAType::I32;
    fn.entry             = b;
    fn.blocks.push_back(std::move(block));

    RegCEmitter emitter;
    auto        result = emitter.emitFunction(fn);
    reason             = result.reason;
    return result;
}

TEST(RegCCompareIntrinsicTest, EmitF64NanGreaterProducesNanAwareThreeWay)
{
    std::string reason;
    auto        result = emitCmpNanIntrinsic("cmp.nan_greater", SSAType::F64, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // dcmpg 语义：任一 NaN → 1，否则 (l>r)-(l<r)。
    EXPECT_NE(result.source.find("isnan"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("? 1 :"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("(r1 > r2) - (r1 < r2)"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCCompareIntrinsicTest, EmitF32NanLessProducesNegativeNanFallback)
{
    std::string reason;
    auto        result = emitCmpNanIntrinsic("cmp.nan_less", SSAType::F32, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // fcmpl 语义：任一 NaN → -1。
    EXPECT_NE(result.source.find("isnan"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("? -1 :"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCCompareIntrinsicTest, EmitI64LongCompareHasNoNanCheck)
{
    std::string reason;
    // LCMP 恒为 nan_less（nanGreater=false），long 无 NaN 概念，不应出现 isnan。
    auto result = emitCmpNanIntrinsic("cmp.nan_less", SSAType::I64, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(result.source.find("isnan"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("(r1 > r2) - (r1 < r2)"), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

TEST(RegCCompareIntrinsicTest, RejectsCmpNanWithTooFewOperands)
{
    auto block  = std::make_unique<RegBlock>(SSABlockId{0});
    RegBlock* b = block.get();
    RegInstruction ins;
    ins.kind          = RegInstructionKind::Intrinsic;
    ins.hasDest       = true;
    ins.dest          = RegValue{RegId{3}, SSAType::I32};
    ins.intrinsicName = "cmp.nan_greater";
    ins.args.push_back(RegOperand::fromRegister(RegValue{RegId{1}, SSAType::F64}));
    // 故意只给一个操作数。
    b->instructions.push_back(std::move(ins));
    RegReturnTerminator ret;
    ret.hasValue = true;
    ret.value    = RegOperand::fromRegister(RegValue{RegId{3}, SSAType::I32});
    b->terminator = std::move(ret);

    RegFunction fn;
    fn.symbol.owner      = "CmpNanRegC";
    fn.symbol.name       = "cmp";
    fn.symbol.desc       = "()I";
    fn.symbol.returnType = SSAType::I32;
    fn.entry             = b;
    fn.blocks.push_back(std::move(block));

    RegCEmitter emitter;
    auto        result = emitter.emitFunction(fn);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("cmp.nan_*"), std::string::npos) << result.reason;
}

// 端到端：用真实 opcode 走 CodeConverter → SSABuilder → RegLowering → RegCEmitter，
// 验证 5 个比较 opcode 都不再 fail-closed（P0 修复前 dcmpg/dcmpl/fcmpg/fcmpl/lcmp 全部静默失败）。
TEST(RegCCompareIntrinsicTest, EndToEndAllCompareOpcodesCompile)
{
    struct Case
    {
        int         opcode;
        const char* label;
    };
    const Case cases[] = {
        {Opcodes::DCMPG, "dcmpg"},
        {Opcodes::DCMPL, "dcmpl"},
        {Opcodes::FCMPG, "fcmpg"},
        {Opcodes::FCMPL, "fcmpl"},
        {Opcodes::LCMP, "lcmp"},
    };
    for (const auto& c : cases) {
        std::string reason;
        // (DD)I：dload 0/2 → dcmpg/dcmpl → ireturn。long 用 (JJ)I 的 lload。
        const bool isLong = c.opcode == Opcodes::LCMP;
        const char* desc  = isLong ? "(JJ)I" : "(DD)I";
        const int   load0 = isLong ? Opcodes::LLOAD : Opcodes::DLOAD;
        const int   load2 = isLong ? Opcodes::LLOAD : Opcodes::DLOAD;
        auto result = emitRegC(
            "cmp",
            desc,
            {
                new VarInsnNode(load0, 0),
                new VarInsnNode(load2, isLong ? 2 : 2),
                new InsnNode(c.opcode),
                new InsnNode(Opcodes::IRETURN),
            },
            4,
            isLong ? 4 : 4,
            reason);
        ASSERT_TRUE(result.ok) << c.label << ": " << result.reason;
        // 整型 LCMP 不应出现 isnan；浮点应出现 isnan。
        if (isLong) {
            EXPECT_EQ(result.source.find("isnan"), std::string::npos)
                << c.label << " should have no NaN check:\n"
                << result.source;
        }
        else {
            EXPECT_NE(result.source.find("isnan"), std::string::npos)
                << c.label << " should have NaN check:\n"
                << result.source;
        }
        expectNoStackAnchors(result.source);
    }
}

// ---- LDC Class 常量（Foo.class）----
//
// 修复前：ssa_builder.constantValue 不识别 Type，LDC Class 落到 opaque:ldc_unknown，
// emitter 渲染成 NULL，导致反射调用（SomeClass.class.getXxx()）在 NULL receiver 上执行。
// 修复后：Type 经 "ldc_class:" 前缀透传，emitter 发射 _mj2x_regc_class_literal（FindClass）。
TEST(RegCLdcClassTest, LdcClassConstantEmitsFindClass)
{
    std::string reason;
    // ()Ljava/lang/Class;：仅一条 ldc 类常量 + areturn。
    // #921 staticArena 退役：Type::staticArena() 已删除，改用测试文件私有静态池
    //（LdcInsnNode 的 desc 借用须活过 emitRegC 调用期）。
    static ca::str::Utf8StringArena testArena;
    auto& arena = testArena;
    auto result = emitRegC(
        "classLiteral",
        "()Ljava/lang/Class;",
        {
            new LdcInsnNode(Type::getObjectType(arena.intern("java/lang/String"))),
            new InsnNode(Opcodes::ARETURN),
        },
        1,
        0,
        reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // 不应是 NULL（ldc_unknown）；应发射 FindClass helper。
    EXPECT_EQ(result.source.find("ldc_unknown"), std::string::npos) << result.source;
    EXPECT_NE(result.source.find("_mj2x_regc_class_literal("), std::string::npos) << result.source;
    // internal name 正确透传。
    EXPECT_NE(result.source.find("\"java/lang/String\""), std::string::npos) << result.source;
    expectNoStackAnchors(result.source);
}

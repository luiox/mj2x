#include <gmock/gmock.h>

#include <memory>
#include <regex>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_terminator.hpp"

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

std::unique_ptr<SSAFunction> buildInlineSSA(const std::string& name, const std::string& desc,
                                            std::initializer_list<AbstractInsnNode*> insns,
                                            int maxStack, int maxLocals, std::string& reason)
{
    ClassNode cn;
    cn.name = ca::str::Utf8StringRef::from_cstr("InlineRegIR");
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);

    CodeConverter converter;
    auto          blocks = converter.convert(*mn);
    SSABuilder    builder;
    return builder.build(blocks, "InlineRegIR", name, desc, true, maxLocals, reason);
}

std::unique_ptr<RegFunction> lowerInlineRegIR(const std::string& name, const std::string& desc,
                                              std::initializer_list<AbstractInsnNode*> insns,
                                              int maxStack, int maxLocals, std::string& reason)
{
    auto ssa = buildInlineSSA(name, desc, insns, maxStack, maxLocals, reason);
    if (!ssa) return nullptr;

    RegLowering lowering;
    return lowering.lower(*ssa, reason);
}

bool hasAnyEdgeCopy(const RegFunction& fn)
{
    for (const auto& block : fn.blocks) {
        bool found = std::visit(
            [](const auto& t) -> bool {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    return !t.target.copies.empty();
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    return !t.trueTarget.copies.empty() || !t.falseTarget.copies.empty();
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    if (!t.defaultTarget.copies.empty()) return true;
                    for (const auto& c : t.cases) {
                        if (!c.target.copies.empty()) return true;
                    }
                    return false;
                }
                else {
                    return false;
                }
            },
            block->terminator);
        if (found) return true;
    }
    return false;
}

bool hasBlockWithArgs(const SSAFunction& fn)
{
    for (const auto& block : fn.blocks) {
        if (!block->args.empty()) return true;
    }
    return false;
}

RegFunction makePureBranchFunction()
{
    RegFunction function;
    auto        entry      = std::make_unique<RegBlock>(SSABlockId{0});
    auto        trueBlock  = std::make_unique<RegBlock>(SSABlockId{1});
    auto        falseBlock = std::make_unique<RegBlock>(SSABlockId{2});

    const RegValue input{RegId{0}, SSAType::I32};
    const RegValue result{RegId{1}, SSAType::I32};
    const RegValue edgeValue{RegId{2}, SSAType::I32};
    function.parameters.push_back(input);

    RegInstruction add;
    add.kind     = RegInstructionKind::Binary;
    add.hasDest  = true;
    add.dest     = result;
    add.lhs      = RegOperand::fromRegister(input);
    add.rhs      = RegOperand::fromInt(1, SSAType::I32);
    add.binaryOp = SSABinaryOp::Add;
    add.effect   = SSAEffect::pure();
    entry->instructions.push_back(add);

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

    function.entry = entry.get();
    function.blocks.push_back(std::move(entry));
    function.blocks.push_back(std::move(trueBlock));
    function.blocks.push_back(std::move(falseBlock));
    return function;
}

}   // namespace

TEST(RegLoweringTest, LowersArithmeticToVirtualRegisters)
{
    std::string reason;
    auto        reg = lowerInlineRegIR("addMul",
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
    ASSERT_NE(reg, nullptr) << reason;

    ASSERT_GE(reg->parameters.size(), 2u);
    EXPECT_EQ(reg->parameters[0].id.value, 0u);
    EXPECT_EQ(reg->parameters[1].id.value, 1u);

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("r0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("+"), std::string::npos) << dump;
    EXPECT_NE(dump.find("*"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("sp"), std::string::npos) << dump;

    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe)) << dump;

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(*reg, validationReason)) << validationReason;
}

TEST(RegLoweringTest, EliminatesLoopBlockArgsIntoEdgeCopies)
{
    auto* loop = new LabelNode();
    auto* done = new LabelNode();

    std::string reason;
    auto        ssa = buildInlineSSA("sum",
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
    ASSERT_NE(ssa, nullptr) << reason;
    ASSERT_TRUE(hasBlockWithArgs(*ssa));

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    ASSERT_NE(reg, nullptr) << reason;
    EXPECT_TRUE(hasAnyEdgeCopy(*reg));

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("<-"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("@slot"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("sp"), std::string::npos) << dump;
}

TEST(RegLoweringTest, PreservesBranchAndSwitchEdgeSpecificCopies)
{
    auto* case0 = new LabelNode();
    auto* case1 = new LabelNode();
    auto* def   = new LabelNode();

    std::string reason;
    auto        reg = lowerInlineRegIR(
        "pick",
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
    ASSERT_NE(reg, nullptr) << reason;

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("switch r0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("case 0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("default"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
}

TEST(RegLoweringTest, LowersObjectsArraysAndIntrinsicsWithoutStackSlots)
{
    std::string reason;
    auto        reg = lowerInlineRegIR("arrayOps",
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
    ASSERT_NE(reg, nullptr) << reason;

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("alloc array [I(2)"), std::string::npos) << dump;
    EXPECT_NE(dump.find("array.store i32"), std::string::npos) << dump;
    EXPECT_NE(dump.find("intrinsic array.length"), std::string::npos) << dump;

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(*reg, validationReason)) << validationReason;
}

TEST(RegDceTest, RemovesUnusedPureInstructionChain)
{
    RegFunction function;
    auto        block = std::make_unique<RegBlock>(SSABlockId{0});

    RegInstruction add;
    add.kind    = RegInstructionKind::Binary;
    add.hasDest = true;
    add.dest    = RegValue{RegId{1}, SSAType::I32};
    add.lhs     = RegOperand::fromInt(3, SSAType::I32);
    add.rhs     = RegOperand::fromInt(4, SSAType::I32);
    add.binaryOp = SSABinaryOp::Add;
    block->instructions.push_back(add);

    RegInstruction multiply;
    multiply.kind    = RegInstructionKind::Binary;
    multiply.hasDest = true;
    multiply.dest    = RegValue{RegId{2}, SSAType::I32};
    multiply.lhs     = RegOperand::fromRegister(add.dest);
    multiply.rhs     = RegOperand::fromInt(2, SSAType::I32);
    multiply.binaryOp = SSABinaryOp::Mul;
    block->instructions.push_back(multiply);
    block->terminator = RegReturnTerminator{};
    function.entry    = block.get();
    function.blocks.push_back(std::move(block));

    EXPECT_EQ(eliminateDeadRegInstructions(function), 2U);
    EXPECT_TRUE(function.blocks.front()->instructions.empty());
}

TEST(RegDceTest, PreservesValueConsumedBySuccessorCopy)
{
    RegFunction function;
    auto        entry = std::make_unique<RegBlock>(SSABlockId{0});
    auto        exit  = std::make_unique<RegBlock>(SSABlockId{1});

    RegInstruction add;
    add.kind    = RegInstructionKind::Binary;
    add.hasDest = true;
    add.dest    = RegValue{RegId{1}, SSAType::I32};
    add.lhs     = RegOperand::fromInt(3, SSAType::I32);
    add.rhs     = RegOperand::fromInt(4, SSAType::I32);
    add.binaryOp = SSABinaryOp::Add;
    entry->instructions.push_back(add);

    RegSuccessor successor;
    successor.block = exit.get();
    successor.copies.push_back(RegCopy{RegValue{RegId{2}, SSAType::I32},
                                       RegOperand::fromRegister(add.dest)});
    entry->terminator = RegJumpTerminator{std::move(successor)};
    exit->terminator =
        RegReturnTerminator{true, RegOperand::fromRegister(RegValue{RegId{2}, SSAType::I32})};

    function.entry = entry.get();
    function.blocks.push_back(std::move(entry));
    function.blocks.push_back(std::move(exit));

    EXPECT_EQ(eliminateDeadRegInstructions(function), 0U);
    ASSERT_EQ(function.blocks.front()->instructions.size(), 1U);
    EXPECT_EQ(function.blocks.front()->instructions.front().dest.id.value, 1U);
}

TEST(RegTransformPassTest, RewritesI32AdditionWithMbaIdentity)
{
    RegFunction    function;
    auto           block = std::make_unique<RegBlock>(SSABlockId{0});
    const RegValue result{RegId{0}, SSAType::I32};

    RegInstruction add;
    add.kind     = RegInstructionKind::Binary;
    add.hasDest  = true;
    add.dest     = result;
    add.lhs      = RegOperand::fromInt(9, SSAType::I32);
    add.rhs      = RegOperand::fromInt(4, SSAType::I32);
    add.binaryOp = SSABinaryOp::Add;
    add.effect   = SSAEffect::pure();
    block->instructions.push_back(add);
    block->terminator = RegReturnTerminator{true, RegOperand::fromRegister(result)};
    function.entry    = block.get();
    function.blocks.push_back(std::move(block));

    RegTransformOptions options;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = false;
    const auto stats                    = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.mbaRewrites, 1U);
    ASSERT_EQ(function.blocks.front()->instructions.size(), 3U);
    EXPECT_EQ(function.blocks.front()->instructions[0].binaryOp, SSABinaryOp::Or);
    EXPECT_EQ(function.blocks.front()->instructions[1].binaryOp, SSABinaryOp::And);
    EXPECT_EQ(function.blocks.front()->instructions[2].binaryOp, SSABinaryOp::Add);

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, MergesOpaquePredicateIntoBranchCondition)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableControlFlowFlattening = false;
    const auto stats                    = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.opaquePredicates, 1U);
    const auto* branch = std::get_if<RegBranchTerminator>(&function.blocks.front()->terminator);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->condition.kind, RegOperandKind::Register);
    ASSERT_EQ(function.blocks.front()->instructions.size(), 5U);
    EXPECT_EQ(function.blocks.front()->instructions[1].binaryOp, SSABinaryOp::Or);
    EXPECT_EQ(function.blocks.front()->instructions[2].binaryOp, SSABinaryOp::And);
    EXPECT_EQ(function.blocks.front()->instructions[4].binaryOp, SSABinaryOp::And);

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, FlattensPureControlFlowAndRetainsEdgeCopies)
{
    RegFunction function      = makePureBranchFunction();
    RegBlock*   originalEntry = function.entry;

    RegTransformOptions options;
    options.enableMba              = false;
    options.enableOpaquePredicates = false;
    const auto stats               = applyRegTransformPasses(function, options);

    EXPECT_TRUE(stats.controlFlowFlattened);
    EXPECT_EQ(stats.cffEdgeSetters, 2U);
    EXPECT_NE(function.entry, originalEntry);
    EXPECT_EQ(function.blocks.size(), 7U);
    EXPECT_TRUE(hasAnyEdgeCopy(function));

    const RegSwitchTerminator* dispatcher = nullptr;
    for (const auto& block : function.blocks) {
        const auto* candidate = std::get_if<RegSwitchTerminator>(&block->terminator);
        if (candidate != nullptr && candidate->cases.size() == 3U) {
            dispatcher = candidate;
            break;
        }
    }
    ASSERT_NE(dispatcher, nullptr);
    EXPECT_EQ(dispatcher->value.kind, RegOperandKind::Register);

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, SkipsEffectfulAndExceptionalFunctions)
{
    RegTransformOptions options;
    options.enableMba              = false;
    options.enableOpaquePredicates = false;

    RegFunction    effectful = makePureBranchFunction();
    RegInstruction call;
    call.kind   = RegInstructionKind::Call;
    call.effect = SSAEffect::pure();
    effectful.blocks.front()->instructions.push_back(call);
    const auto effectfulStats = applyRegTransformPasses(effectful, options);
    EXPECT_FALSE(effectfulStats.controlFlowFlattened);
    EXPECT_EQ(effectful.blocks.size(), 3U);

    RegFunction exceptional = makePureBranchFunction();
    exceptional.exceptionRegions.push_back(RegExceptionRegion{});
    const auto exceptionalStats = applyRegTransformPasses(exceptional, options);
    EXPECT_FALSE(exceptionalStats.controlFlowFlattened);
    EXPECT_EQ(exceptional.blocks.size(), 3U);
}

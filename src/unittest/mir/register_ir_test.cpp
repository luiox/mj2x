#include <gmock/gmock.h>

#include <memory>
#include <regex>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_terminator.hpp"

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

std::unique_ptr<SSAFunction> buildInlineSSA(
    const std::string& name,
    const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals,
    std::string& reason)
{
    ClassNode cn;
    cn.name = ca::str::Utf8StringRef::from_cstr("InlineRegIR");
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         name, desc, insns, maxStack, maxLocals);

    CodeConverter converter;
    auto blocks = converter.convert(*mn);
    SSABuilder builder;
    return builder.build(blocks, "InlineRegIR", name, desc, true, maxLocals, reason);
}

std::unique_ptr<RegFunction> lowerInlineRegIR(
    const std::string& name,
    const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals,
    std::string& reason)
{
    auto ssa = buildInlineSSA(name, desc, insns, maxStack, maxLocals, reason);
    if (!ssa) return nullptr;

    RegLowering lowering;
    return lowering.lower(*ssa, reason);
}

bool hasAnyEdgeCopy(const RegFunction& fn) {
    for (const auto& block : fn.blocks) {
        bool found = std::visit([](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                return !t.target.copies.empty();
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                return !t.trueTarget.copies.empty() || !t.falseTarget.copies.empty();
            } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (!t.defaultTarget.copies.empty()) return true;
                for (const auto& c : t.cases) {
                    if (!c.target.copies.empty()) return true;
                }
                return false;
            } else {
                return false;
            }
        }, block->terminator);
        if (found) return true;
    }
    return false;
}

bool hasBlockWithArgs(const SSAFunction& fn) {
    for (const auto& block : fn.blocks) {
        if (!block->args.empty()) return true;
    }
    return false;
}

} // namespace

TEST(RegLoweringTest, LowersArithmeticToVirtualRegisters) {
    std::string reason;
    auto reg = lowerInlineRegIR("addMul", "(II)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 1),
        new InsnNode(Opcodes::IADD),
        new InsnNode(Opcodes::ICONST_3),
        new InsnNode(Opcodes::IMUL),
        new InsnNode(Opcodes::IRETURN),
    }, 3, 2, reason);
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

TEST(RegLoweringTest, EliminatesLoopBlockArgsIntoEdgeCopies) {
    auto* loop = new LabelNode();
    auto* done = new LabelNode();

    std::string reason;
    auto ssa = buildInlineSSA("sum", "(I)I", {
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
    }, 3, 3, reason);
    ASSERT_NE(ssa, nullptr) << reason;
    ASSERT_TRUE(hasBlockWithArgs(*ssa));

    RegLowering lowering;
    auto reg = lowering.lower(*ssa, reason);
    ASSERT_NE(reg, nullptr) << reason;
    EXPECT_TRUE(hasAnyEdgeCopy(*reg));

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("<-"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("@slot"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("sp"), std::string::npos) << dump;
}

TEST(RegLoweringTest, PreservesBranchAndSwitchEdgeSpecificCopies) {
    auto* case0 = new LabelNode();
    auto* case1 = new LabelNode();
    auto* def = new LabelNode();

    std::string reason;
    auto reg = lowerInlineRegIR("pick", "(I)I", {
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
    }, 2, 1, reason);
    ASSERT_NE(reg, nullptr) << reason;

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("switch r0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("case 0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("default"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
}

TEST(RegLoweringTest, LowersObjectsArraysAndIntrinsicsWithoutStackSlots) {
    std::string reason;
    auto reg = lowerInlineRegIR("arrayOps", "()I", {
        new InsnNode(Opcodes::ICONST_2),
        new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_INT),
        new InsnNode(Opcodes::DUP),
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::ICONST_5),
        new InsnNode(Opcodes::IASTORE),
        new InsnNode(Opcodes::DUP),
        new InsnNode(Opcodes::ARRAYLENGTH),
        new InsnNode(Opcodes::IRETURN),
    }, 4, 0, reason);
    ASSERT_NE(reg, nullptr) << reason;

    std::string dump = dumpRegIR(*reg);
    EXPECT_NE(dump.find("alloc array [I(2)"), std::string::npos) << dump;
    EXPECT_NE(dump.find("array.store i32"), std::string::npos) << dump;
    EXPECT_NE(dump.find("intrinsic array.length"), std::string::npos) << dump;

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(*reg, validationReason)) << validationReason;
}

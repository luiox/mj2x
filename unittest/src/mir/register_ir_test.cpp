#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <cstdint>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/passes/const_key_derivation.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_terminator.hpp"

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
using ca::str::Utf8StringRef;

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

// buildInlineSSA / lowerInlineRegIR 的返回包：把 arena 与 IR 绑在一起。
// SSA/RegIR 节点字段是 arena-backed Utf8StringRef（spec §6.1），arena 析构会让 IR 内的
// ref 全部悬垂——故不能用局部 arena + 返回 IR 的旧写法（参见 #698）。
// 成员声明顺序：先 ir 后 arena，保证 ir 先析构、arena 后释放。
struct InlineSsaBundle
{
    std::unique_ptr<SSAFunction>      fn;
    std::unique_ptr<Utf8StringArena>  arena;
    operator bool() const { return fn != nullptr; }
    bool          operator==(std::nullptr_t) const { return fn == nullptr; }
    bool          operator!=(std::nullptr_t) const { return fn != nullptr; }
    SSAFunction&  operator*() const { return *fn; }
    SSAFunction*  operator->() const { return fn.get(); }
};

struct InlineRegBundle
{
    std::unique_ptr<RegFunction>      reg;
    std::unique_ptr<Utf8StringArena>  arena;
    operator bool() const { return reg != nullptr; }
    bool          operator==(std::nullptr_t) const { return reg == nullptr; }
    bool          operator!=(std::nullptr_t) const { return reg != nullptr; }
    RegFunction&  operator*() const { return *reg; }
    RegFunction*  operator->() const { return reg.get(); }
};

InlineSsaBundle buildInlineSSA(const std::string& name, const std::string& desc,
                               std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                               int maxLocals, std::string& reason)
{
    InlineSsaBundle bundle;
    bundle.arena = std::make_unique<Utf8StringArena>();
    ClassNode     cn;
    cn.name = ca::str::Utf8StringRef::from_cstr("InlineRegIR");
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);

    CodeConverter converter(*bundle.arena);
    auto          blocks = converter.convert(*mn);
    SSABuilder    builder;
    bundle.fn =
        builder.build(blocks, "InlineRegIR", name, desc, true, maxLocals, *bundle.arena, reason);
    return bundle;
}

InlineRegBundle lowerInlineRegIR(const std::string& name, const std::string& desc,
                                 std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                 int maxLocals, std::string& reason)
{
    InlineRegBundle bundle;
    auto            ssa = buildInlineSSA(name, desc, insns, maxStack, maxLocals, reason);
    if (!ssa) return bundle;   // arena 与 fn 都已随 ssa 析构，bundle 保持空

    bundle.arena = std::move(ssa.arena);   // 把 arena 接管过来，陪 reg 走完 dump
    RegLowering   lowering;
    bundle.reg = lowering.lower(*ssa, reason);
    return bundle;
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

/// 单块纯 Add 函数（深 MBA / 基础 MBA 测试用）：int f() { return 9 + 4; }
RegFunction makePureAddFunction()
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
    return function;
}

/// 双参数纯 Add 函数：int f(int a, int b) { return a + b; }
/// （mbaDeep 变体语义求值用——恒等式对具体 (a,b) 代入执行验证）
RegFunction makeParamAddFunction()
{
    RegFunction    function;
    auto           block = std::make_unique<RegBlock>(SSABlockId{0});
    const RegValue lhs{RegId{0}, SSAType::I32};
    const RegValue rhs{RegId{1}, SSAType::I32};
    const RegValue result{RegId{2}, SSAType::I32};

    RegInstruction add;
    add.kind     = RegInstructionKind::Binary;
    add.hasDest  = true;
    add.dest     = result;
    add.lhs      = RegOperand::fromRegister(lhs);
    add.rhs      = RegOperand::fromRegister(rhs);
    add.binaryOp = SSABinaryOp::Add;
    add.effect   = SSAEffect::pure();
    block->instructions.push_back(add);
    block->terminator = RegReturnTerminator{true, RegOperand::fromRegister(result)};
    function.entry    = block.get();
    function.parameters.push_back(lhs);
    function.parameters.push_back(rhs);
    function.blocks.push_back(std::move(block));
    return function;
}

/// 调用目标间接化测试用：单块函数，块内依次放若干 Call 指令后 return。
/// owner 决定是否自递归（applyCallIndirections 跳过同 owner 的 INVOKESTATIC）。
/// 身份串用字符串字面量（静态存储期，无需 arena 保活）。
RegFunction makeCallFunction(const std::string& selfOwner,
                             std::initializer_list<std::tuple<const char*, const char*, const char*,
                                                               int>> calls)
{
    RegFunction    function;
    auto           block    = std::make_unique<RegBlock>(SSABlockId{0});
    const RegValue argument = {RegId{0}, SSAType::I32};
    function.parameters.push_back(argument);
    function.symbol.owner = selfOwner;

    int32_t nextRegister = 1;
    for (const auto& [owner, name, desc, invokeOpcode] : calls) {
        RegInstruction call;
        call.kind   = RegInstructionKind::Call;
        call.effect = SSAEffect::externalCall();
        call.method = SSAMethodRef{Utf8StringRef::from_cstr(owner),
                                   Utf8StringRef::from_cstr(name),
                                   Utf8StringRef::from_cstr(desc),
                                   invokeOpcode};
        if (invokeOpcode == Opcodes::INVOKESTATIC) {
            call.args.push_back(RegOperand::fromRegister(argument));
        }
        else {
            call.args.push_back(RegOperand::fromRegister(argument));
            call.args.push_back(RegOperand::fromRegister(argument));
        }
        if (desc[0] != 'V') {
            call.hasDest = true;
            call.dest    = RegValue{RegId{static_cast<uint32_t>(nextRegister++)}, SSAType::I32};
        }
        block->instructions.push_back(call);
    }
    block->terminator = RegReturnTerminator{true, RegOperand::fromRegister(argument)};
    function.entry    = block.get();
    function.blocks.push_back(std::move(block));
    return function;
}

/// 收集函数内所有 Call 指令的 indirectCall 标记（按指令序）。
std::vector<bool> indirectCallFlags(const RegFunction& function)
{
    std::vector<bool> flags;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions) {
            if (instruction.kind == RegInstructionKind::Call) {
                flags.push_back(instruction.indirectCall);
            }
        }
    }
    return flags;
}

/// 按目标类型宽度窄化一个 64 位位型（JVM 整型窄化：截断后按目标符号扩展）。
int64_t truncateToType(int64_t value, SSAType type)
{
    switch (type) {
    case SSAType::BOOL: return (value & 0xFFu) != 0 ? 1 : 0;
    case SSAType::I8: return static_cast<int64_t>(static_cast<int8_t>(value & 0xFFu));
    case SSAType::I16: return static_cast<int64_t>(static_cast<int16_t>(value & 0xFFFFu));
    case SSAType::CHAR: return static_cast<int64_t>(static_cast<uint16_t>(value & 0xFFFFu));
    case SSAType::I32: return static_cast<int64_t>(static_cast<int32_t>(value & 0xFFFFFFFFu));
    default: return value;
    }
}

/// 直线求值 mbaDeep 变换后的单块指令链：参数寄存器代入具体值，按序执行
/// Unary/Binary/Copy/Convert/Intrinsic（Barrier 跳过），返回 return 操作数
/// 的求值结果（按类型宽度截断）。用于逐 (a,b) 样本断言 r == a+b（模 2^N）。
int64_t evaluateStraightLineRegIR(const RegFunction& fn, int64_t a, int64_t b)
{
    std::map<uint32_t, int64_t> regs;
    regs[fn.parameters.at(0).id.value] = a;
    regs[fn.parameters.at(1).id.value] = b;
    const uint64_t mask =
        fn.parameters.at(0).type == SSAType::I32 ? 0xFFFFFFFFull : ~0ull;
    auto operandValue = [&](const RegOperand& op) -> int64_t {
        if (op.kind == RegOperandKind::IntLiteral) return op.intValue;
        const auto it = regs.find(op.reg.value);
        EXPECT_NE(it, regs.end());
        return it->second;
    };
    const auto wrap = [&](uint64_t v) -> int64_t {
        if (mask == 0xFFFFFFFFull) return static_cast<int64_t>(static_cast<int32_t>(v));
        return static_cast<int64_t>(v);
    };
    for (const auto& ins : fn.blocks.front()->instructions) {
        int64_t result = 0;
        switch (ins.kind) {
        case RegInstructionKind::Unary: {
            result = ins.unaryOp == SSAUnaryOp::BitNot ? ~operandValue(ins.value)
                                                       : -operandValue(ins.value);
            break;
        }
        case RegInstructionKind::Binary: {
            const uint64_t l = static_cast<uint64_t>(operandValue(ins.lhs));
            const uint64_t r = static_cast<uint64_t>(operandValue(ins.rhs));
            uint64_t    v = 0;
            switch (ins.binaryOp) {
            case SSABinaryOp::Add: v = l + r; break;
            case SSABinaryOp::And: v = l & r; break;
            case SSABinaryOp::Or: v = l | r; break;
            case SSABinaryOp::Xor: v = l ^ r; break;
            case SSABinaryOp::Shl: v = l << (r & 63u); break;
            default: ADD_FAILURE() << "unexpected binary op in mba chain"; return 0;
            }
            result = static_cast<int64_t>(v);
            break;
        }
        case RegInstructionKind::Copy: result = operandValue(ins.copy.source); break;
        case RegInstructionKind::Convert: {
            // 立即数隐藏链末位（I64→I32 截断）；按目标类型宽度窄化。
            const int64_t source = operandValue(ins.value);
            result               = truncateToType(source, ins.targetType);
            break;
        }
        case RegInstructionKind::Intrinsic: {
            // 立即数隐藏密钥：const.key:<site> → splitmix64(site ^ kConstKeyOpaque)。
            // 与 RT 双端 mj2x_regc_const_key / mj2x_llvm_const_key 同源
            // （跨语言一致性由 E2E 真实编译+运行守）。
            const std::string prefix(kConstKeyIntrinsicPrefix);
            if (ins.intrinsicName.rfind(prefix, 0) != 0) {
                ADD_FAILURE() << "unsupported intrinsic in straight-line evaluation";
                return 0;
            }
            result = static_cast<int64_t>(
                deriveConstKey(std::stoll(ins.intrinsicName.substr(prefix.size()))));
            break;
        }
        case RegInstructionKind::Barrier: continue;
        default: ADD_FAILURE() << "unexpected instruction kind in mba chain"; return 0;
        }
        regs[ins.dest.id.value] = wrap(static_cast<uint64_t>(result));
    }
    const auto* ret = std::get_if<RegReturnTerminator>(&fn.blocks.front()->terminator);
    EXPECT_NE(ret, nullptr);
    return wrap(static_cast<uint64_t>(operandValue(ret->value)));
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
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());

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
    auto* case0 = new LabelNode(labelScratchArena().create<Label>());
    auto* case1 = new LabelNode(labelScratchArena().create<Label>());
    auto* def   = new LabelNode(labelScratchArena().create<Label>());

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
    options.enableMba                   = true;
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
    options.enableOpaquePredicates      = true;
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
    options.enableMba                   = false;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = true;
    const auto stats = applyRegTransformPasses(function, options);

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

// 审查 G-1 修复回归：CFF 随机 state 码查重——同码会让 dispatcher switch 首个
// case 命中而静默路由错误（加密模式 case 键 = code ^ xorKey，同 code 同键）。
// 20 块直线链 + cffStateEncryption，断言 dispatcher 全部 case 键互异。
TEST(RegTransformPassTest, CffStateCodesAreUnique)
{
    constexpr size_t kChainLength = 20;
    RegFunction      function;
    std::vector<RegBlock*> chain;
    for (size_t i = 0; i < kChainLength; ++i) {
        auto block  = std::make_unique<RegBlock>(SSABlockId{static_cast<ca::u32>(i)});
        RegBlock* p = block.get();
        chain.push_back(p);
        if (i + 1 < kChainLength) {
            RegSuccessor succ;
            succ.block = nullptr;   // 下一块按序补链
            block->terminator = RegJumpTerminator{std::move(succ)};
        }
        else {
            block->terminator = RegReturnTerminator{
                true, RegOperand::fromInt(static_cast<int64_t>(i), SSAType::I32)};
        }
        function.blocks.push_back(std::move(block));
    }
    for (size_t i = 0; i + 1 < kChainLength; ++i) {
        auto& term = std::get<RegJumpTerminator>(chain[i]->terminator);
        term.target.block = chain[i + 1];
    }
    function.entry = chain.front();

    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = true;
    options.cffStateEncryption          = true;
    options.transformSeed               = 12345;
    const auto stats = applyRegTransformPasses(function, options);
    ASSERT_TRUE(stats.controlFlowFlattened);

    const RegSwitchTerminator* dispatcher = nullptr;
    for (const auto& block : function.blocks) {
        const auto* candidate = std::get_if<RegSwitchTerminator>(&block->terminator);
        if (candidate != nullptr && candidate->cases.size() == kChainLength) {
            dispatcher = candidate;
            break;
        }
    }
    ASSERT_NE(dispatcher, nullptr) << "dispatcher switch with " << kChainLength
                                   << " cases not found";

    std::set<int64_t> keys;
    for (const auto& c : dispatcher->cases) {
        EXPECT_TRUE(keys.insert(c.key).second)
            << "duplicate dispatcher case key " << c.key << " (two blocks share a state code)";
    }
    EXPECT_EQ(keys.size(), kChainLength);
}

TEST(RegTransformPassTest, SkipsEffectfulAndExceptionalFunctions)
{
    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = true;

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

// ===========================================================================
// issue #829：Cloakwork 借鉴项（全部默认关闭；开启后 stats 计数 + 结构断言）。
// ===========================================================================

TEST(RegTransformPassTest, MbaDeepExpandsAddWithBarriers)
{
    RegFunction function = makePureAddFunction();

    RegTransformOptions options;
    options.enableMba                   = true;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = false;
    options.mbaDeep                     = true;
    options.transformSeed               = 42;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.mbaRewrites, 0U);
    EXPECT_GT(stats.mbaDeepRewrites, 0U);
    // 展开链夹 barrier（mj2x_regc_barrier 防折叠证据）且结果语义不变
    // （最终 Copy 写回原 dest）。
    bool sawBarrier = false;
    bool sawCopy    = false;
    for (const auto& ins : function.blocks.front()->instructions) {
        if (ins.kind == RegInstructionKind::Barrier) sawBarrier = true;
        if (ins.kind == RegInstructionKind::Copy) sawCopy = true;
    }
    EXPECT_TRUE(sawBarrier) << "deep MBA must interleave barriers to defeat folding";
    EXPECT_TRUE(sawCopy);

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, MbaDeepOffKeepsLegacySingleForm)
{
    RegFunction function = makePureAddFunction();

    RegTransformOptions options;
    options.enableMba                   = true;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = false;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.mbaRewrites, 1U);
    EXPECT_EQ(stats.mbaDeepRewrites, 0U);
    ASSERT_EQ(function.blocks.front()->instructions.size(), 3U);
    EXPECT_EQ(function.blocks.front()->instructions[0].binaryOp, SSABinaryOp::Or);
}

// round-3 审查修复：mbaDe Morgan 变体曾把 t6 写成 ~(a&b)<<1（正确为 (a&b)<<1），
// (0,0) 代入即得 -2 且零测试覆盖。两变体都按具体 (a,b) 求值断言 a+b 语义
// （含 INT32_MIN/INT32_MAX 溢出边界；模 2^32 环绕）。
TEST(RegTransformPassTest, MbaDeepVariantsPreserveAddSemantics)
{
    const std::pair<int64_t, int64_t> samples[] = {
        {0, 0},           {1, 2},                          {9, 4},
        {-7, 3},          {INT32_MIN, -1},                 {INT32_MIN, INT32_MAX},
        {INT32_MAX, 1},   {INT32_MAX, INT32_MAX},          {123456789, -987654321},
        {0x55555555, (int64_t)0xAAAAAAAA},
    };
    bool sawDeMorgan = false;
    bool sawCarry    = false;
    for (uint64_t seed = 0; seed < 64u; ++seed) {
        RegFunction function = makeParamAddFunction();

        RegTransformOptions options;
        options.enableMba                   = true;
        options.enableOpaquePredicates      = false;
        options.enableControlFlowFlattening = false;
        options.mbaDeep                     = true;
        options.transformSeed               = seed;
        const auto stats = applyRegTransformPasses(function, options);
        ASSERT_EQ(stats.mbaDeepRewrites, 1U);

        // 变体识别：De Morgan 展开不含 Shl，carry 两轮含 (a&b)<<1 的 Shl。
        bool hasShift = false;
        for (const auto& ins : function.blocks.front()->instructions) {
            if (ins.kind == RegInstructionKind::Binary && ins.binaryOp == SSABinaryOp::Shl)
                hasShift = true;
        }
        hasShift ? sawCarry = true : sawDeMorgan = true;

        for (const auto& [a, b] : samples) {
            const int64_t expected = static_cast<int64_t>(static_cast<int32_t>(
                static_cast<uint32_t>(a) + static_cast<uint32_t>(b)));
            EXPECT_EQ(evaluateStraightLineRegIR(function, a, b), expected)
                << "seed=" << seed << " a=" << a << " b=" << b;
        }
    }
    EXPECT_TRUE(sawDeMorgan) << "64 个 seed 未覆盖 De Morgan 变体";
    EXPECT_TRUE(sawCarry) << "64 个 seed 未覆盖 carry 两轮变体";
}

TEST(RegTransformPassTest, OpaqueZeroSelectsVariantsBySeed)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableOpaquePredicates      = true;
    options.enableControlFlowFlattening = false;
    options.opaqueZero                  = true;
    options.transformSeed               = 7;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.opaquePredicates, 0U);
    EXPECT_EQ(stats.opaqueZeroPredicates, 1U);
    // 条件仍是合并后的寄存器（恒真谓词 && 原条件）。
    const auto* branch = std::get_if<RegBranchTerminator>(&function.blocks.front()->terminator);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->condition.kind, RegOperandKind::Register);
    EXPECT_GT(function.blocks.front()->instructions.size(), 5U)
        << "variant chain must add instructions beyond the legacy 5";

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, CffDeadBlocksInjectNeverTakenBranches)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = true;
    options.cffDeadBlockDensity         = 1;   // 每个 setter 都插死块
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_TRUE(stats.controlFlowFlattened);
    EXPECT_GT(stats.cffDeadBlocks, 0U);
    // 死块 = 含 barrier 的块（防 DCE 证据）。
    bool sawDeadBarrier = false;
    for (const auto& block : function.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.kind == RegInstructionKind::Barrier) sawDeadBarrier = true;
        }
    }
    EXPECT_TRUE(sawDeadBarrier) << "dead blocks must carry barriers to survive DCE";

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, CffStateEncryptionDerivesRandomizedStateCodes)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableMba                   = false;
    options.enableOpaquePredicates      = false;
    options.enableControlFlowFlattening = true;
    options.cffStateEncryption          = true;
    options.transformSeed               = 99;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_TRUE(stats.controlFlowFlattened);
    const RegSwitchTerminator* dispatcher = nullptr;
    for (const auto& block : function.blocks) {
        const auto* candidate = std::get_if<RegSwitchTerminator>(&block->terminator);
        if (candidate != nullptr && candidate->cases.size() == 3U) {
            dispatcher = candidate;
            break;
        }
    }
    ASSERT_NE(dispatcher, nullptr);
    // 派生 state 码不是顺序 1,2,3；且 case 常量被异或（≠ 写入值）。
    EXPECT_NE(dispatcher->cases[0].key, 1);
    // dispatcher 块含异或指令（状态解密）。
    bool sawXor = false;
    for (const auto& block : function.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.kind == RegInstructionKind::Binary && ins.binaryOp == SSABinaryOp::Xor)
                sawXor = true;
        }
    }
    EXPECT_TRUE(sawXor) << "state encryption must xor state before dispatch";

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

// H2 守门：深 MBA 注入的 Barrier 不得让 CFF 放弃整函数。
// 历史缺陷——pass 顺序 MBA → opaque → CFF，而 CFF 把 Barrier 判为非 pure，结果
// `--regc-mba-deep` 与 CFF 同一函数并开时， ciphertext 形态退化回原始 CFG 且不报错
// （实测：同一纯算术函数 dispatcher 12 块 → 5 块）。Barrier 是无数据流/无异常的
// 不可折叠锚点，不构成 CFF 必须保守的理由。
TEST(RegTransformPassTest, MbaDeepBarriersDoNotSuppressFlattening)
{
    RegFunction function = makePureBranchFunction();   // 内含一条 Add，mba-deep 会展开

    RegTransformOptions options;
    options.enableControlFlowFlattening = true;
    options.enableMba                   = true;
    options.mbaDeep                     = true;
    options.transformSeed               = 4242;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_GT(stats.mbaDeepRewrites, 0U) << "deep MBA must have rewritten something";
    EXPECT_TRUE(stats.controlFlowFlattened)
        << "barrier injection must not disable CFF for the whole function";

    // 结构未退化：bootstrap + dispatcher 确实存在，且 barrier 留在块内。
    const RegSwitchTerminator* dispatcher = nullptr;
    bool                       sawBarrier = false;
    for (const auto& block : function.blocks) {
        if (std::get_if<RegSwitchTerminator>(&block->terminator) != nullptr &&
            std::get<RegSwitchTerminator>(block->terminator).cases.size() == 3U) {
            dispatcher = &std::get<RegSwitchTerminator>(block->terminator);
        }
        for (const auto& ins : block->instructions) {
            if (ins.kind == RegInstructionKind::Barrier) sawBarrier = true;
        }
    }
    EXPECT_NE(dispatcher, nullptr);
    EXPECT_TRUE(sawBarrier) << "barriers must survive the flattening pass";

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

// B1 守门：state 码与 xor key 必须落在 int32 正区间。
// RegSwitchCase::key 以 int64_t 承载，但 RegC 发射成 C `switch` 的 case 常量；
// u32 最高位置 1 会溢出 int32，clang 报 `-Wswitch: overflow converting case value`
// 且踩整型转换实现域。
TEST(RegTransformPassTest, CffStateCodesStayWithinPositiveInt32)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableControlFlowFlattening = true;
    options.cffStateEncryption          = true;
    for (uint64_t seed : {uint64_t{0}, uint64_t{1}, uint64_t{99}, uint64_t{0xDEADBEEFull},
                          uint64_t{0xFFFFFFFFFFFFFFFFull}}) {
        SCOPED_TRACE("seed=" + std::to_string(seed));
        RegFunction        seeded = makePureBranchFunction();
        RegTransformOptions opts  = options;
        opts.transformSeed        = seed;
        const auto stats = applyRegTransformPasses(seeded, opts);
        ASSERT_TRUE(stats.controlFlowFlattened);

        const RegSwitchTerminator* dispatcher = nullptr;
        for (const auto& block : seeded.blocks) {
            const auto* candidate = std::get_if<RegSwitchTerminator>(&block->terminator);
            if (candidate != nullptr && candidate->cases.size() == 3U) {
                dispatcher = candidate;
                break;
            }
        }
        ASSERT_NE(dispatcher, nullptr);
        for (const auto& item : dispatcher->cases) {
            EXPECT_GE(item.key, 1) << "state case key must be a positive int32";
            EXPECT_LE(item.key, INT32_MAX) << "state case key must not overflow int32";
        }
    }
}

// ---------- 批次 2 第 6 项：立即数隐藏（对标 jvmtp obfuscation.constants） ----------

/// 参数 + 立即数：`int f(int a, int b) { return a + imm + b; }`
/// （立即数隐藏语义求值用——隐藏后求值结果须与隐藏前逐样本相同。）
RegFunction makeParamAddImmediateFunction(int64_t immediate, SSAType type)
{
    RegFunction    function;
    auto           block = std::make_unique<RegBlock>(SSABlockId{0});
    const RegValue lhs{RegId{0}, type};
    const RegValue rhs{RegId{1}, type};
    const RegValue sum{RegId{2}, type};
    const RegValue result{RegId{3}, type};

    RegInstruction addImmediate;
    addImmediate.kind     = RegInstructionKind::Binary;
    addImmediate.hasDest  = true;
    addImmediate.dest     = sum;
    addImmediate.lhs      = RegOperand::fromRegister(lhs);
    addImmediate.rhs      = RegOperand::fromInt(immediate, type);
    addImmediate.binaryOp = SSABinaryOp::Add;
    addImmediate.effect   = SSAEffect::pure();

    RegInstruction addParameter;
    addParameter.kind     = RegInstructionKind::Binary;
    addParameter.hasDest  = true;
    addParameter.dest     = result;
    addParameter.lhs      = RegOperand::fromRegister(sum);
    addParameter.rhs      = RegOperand::fromRegister(rhs);
    addParameter.binaryOp = SSABinaryOp::Add;
    addParameter.effect   = SSAEffect::pure();

    block->instructions.push_back(addImmediate);
    block->instructions.push_back(addParameter);
    block->terminator = RegReturnTerminator{true, RegOperand::fromRegister(result)};
    function.entry    = block.get();
    function.parameters.push_back(lhs);
    function.parameters.push_back(rhs);
    function.blocks.push_back(std::move(block));
    return function;
}

/// 收集一条指令的全部操作数槽（与 pass 侧遍历面一致，漏槽位即漏断言）。
std::vector<const RegOperand*> operandSlotsOf(const RegInstruction& instruction)
{
    std::vector<const RegOperand*> slots;
    slots.push_back(&instruction.value);
    slots.push_back(&instruction.lhs);
    slots.push_back(&instruction.rhs);
    slots.push_back(&instruction.receiver);
    slots.push_back(&instruction.index);
    if (instruction.kind == RegInstructionKind::Copy) slots.push_back(&instruction.copy.source);
    for (const auto& argument : instruction.args) slots.push_back(&argument);
    return slots;
}

/// 断言整块指令里不再出现某个立即数值（隐藏效果证据）。
bool blockMentionsImmediate(const RegFunction& function, int64_t immediate)
{
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions) {
            for (const auto* slot : operandSlotsOf(instruction)) {
                if (slot->kind == RegOperandKind::IntLiteral && slot->intValue == immediate)
                    return true;
            }
        }
    }
    return false;
}

// E ^ K == C 的端到端证据：隐藏后求值结果与隐藏前逐样本相同。
// 同时守住「原立即数不再以字面量出现」与 site 单调分配两个不变量。
TEST(RegTransformPassTest, ConstantHidingPreservesImmediateSemantics)
{
    const int64_t immediates[] = {0, 1, -1, 42, 0x12345678, INT32_MIN, INT32_MAX, -123456789};
    for (int64_t immediate : immediates) {
        SCOPED_TRACE("immediate=" + std::to_string(immediate));
        RegFunction original = makeParamAddImmediateFunction(immediate, SSAType::I32);
        const int64_t expected = evaluateStraightLineRegIR(original, 5, 7);

        RegFunction        function = makeParamAddImmediateFunction(immediate, SSAType::I32);
        RegTransformOptions options;
        options.hideImmediateConstants = true;
        const auto stats = applyRegTransformPasses(function, options);

        EXPECT_EQ(stats.constantsHidden, 1U);
        EXPECT_FALSE(blockMentionsImmediate(function, immediate))
            << "hidden immediate must not survive as a literal";
        EXPECT_EQ(evaluateStraightLineRegIR(function, 5, 7), expected);

        std::string validationReason;
        EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
    }
}

// I64 立即数：Xor 结果即目标值，不追加 Convert（多余截断会破坏宽常量）。
TEST(RegTransformPassTest, ConstantHidingKeepsI64WithoutTruncation)
{
    const int64_t immediate = 0x0123456789ABCDEFull;
    RegFunction   function  = makeParamAddImmediateFunction(immediate, SSAType::I64);

    RegTransformOptions options;
    options.hideImmediateConstants = true;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.constantsHidden, 1U);
    const auto& instructions = function.blocks.front()->instructions;
    ASSERT_EQ(instructions.size(), 4U);
    EXPECT_EQ(instructions[0].kind, RegInstructionKind::Intrinsic);
    EXPECT_EQ(instructions[1].kind, RegInstructionKind::Binary);
    EXPECT_EQ(instructions[1].binaryOp, SSABinaryOp::Xor);
    EXPECT_EQ(instructions[2].kind, RegInstructionKind::Binary);   // 原 Add，无 Convert
    EXPECT_EQ(instructions[2].binaryOp, SSABinaryOp::Add);
    EXPECT_EQ(evaluateStraightLineRegIR(function, 5, 7),
              static_cast<int64_t>(5 + immediate + 7));
}

// 浮点立即数不动：浮点没有 XOR 恒等改写面，改写会引入精度/位型风险。
TEST(RegTransformPassTest, ConstantHidingSkipsFloatImmediates)
{
    RegFunction function;
    auto        block = std::make_unique<RegBlock>(SSABlockId{0});
    const RegValue lhs{RegId{0}, SSAType::F64};
    const RegValue result{RegId{1}, SSAType::F64};

    RegInstruction add;
    add.kind     = RegInstructionKind::Binary;
    add.hasDest  = true;
    add.dest     = result;
    add.lhs      = RegOperand::fromRegister(lhs);
    add.rhs      = RegOperand::fromFloat(2.5, SSAType::F64);
    add.binaryOp = SSABinaryOp::Add;
    add.effect   = SSAEffect::pure();
    block->instructions.push_back(add);
    block->terminator = RegReturnTerminator{true, RegOperand::fromRegister(result)};
    function.entry    = block.get();
    function.parameters.push_back(lhs);
    function.blocks.push_back(std::move(block));

    RegTransformOptions options;
    options.hideImmediateConstants = true;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.constantsHidden, 0U);
    ASSERT_EQ(function.blocks.front()->instructions.size(), 1U);
    EXPECT_EQ(function.blocks.front()->instructions[0].rhs.kind, RegOperandKind::FloatLiteral);
}

// site 是函数内局部计数器、不掺 seed：不同 seed 产物必须逐字节相同
// （可复现构建不变量的另一面——seed 只影响 MBA/opaque/CFF 变体与 state 码）。
TEST(RegTransformPassTest, ConstantHidingIsSeedIndependent)
{
    auto fingerprint = [](uint64_t seed) {
        RegFunction        function = makeParamAddImmediateFunction(0x5EED1234, SSAType::I32);
        RegTransformOptions options;
        options.hideImmediateConstants = true;
        options.transformSeed          = seed;
        const auto stats = applyRegTransformPasses(function, options);
        std::string out = std::to_string(stats.constantsHidden);
        for (const auto& instruction : function.blocks.front()->instructions) {
            out += "|" + instruction.intrinsicName;
            out += "|" + std::to_string(instruction.value.intValue);
            out += "|" + std::to_string(instruction.rhs.intValue);
        }
        return out;
    };
    EXPECT_EQ(fingerprint(0), fingerprint(1));
    EXPECT_EQ(fingerprint(0), fingerprint(0xDEADBEEFCAFE1234ull));
}

// 与 CFF 叠加的零交互证据：立即数隐藏排 CFF 之后，dispatcher 的 switch
// case key 保持字面量（分发表完整性），被隐藏的是块内常量。
TEST(RegTransformPassTest, ConstantHidingRunsAfterCffAndKeepsDispatcherKeys)
{
    RegFunction function = makePureBranchFunction();

    RegTransformOptions options;
    options.enableControlFlowFlattening = true;
    options.cffStateEncryption          = true;
    options.hideImmediateConstants      = true;
    options.transformSeed               = 11;
    const auto stats = applyRegTransformPasses(function, options);

    EXPECT_TRUE(stats.controlFlowFlattened);
    EXPECT_GT(stats.constantsHidden, 0U);
    // dispatcher 的 case key 未被改写（仍是字面量）。
    const RegSwitchTerminator* dispatcher = nullptr;
    for (const auto& block : function.blocks) {
        const auto* candidate = std::get_if<RegSwitchTerminator>(&block->terminator);
        if (candidate != nullptr && candidate->cases.size() == 3U) {
            dispatcher = candidate;
            break;
        }
    }
    ASSERT_NE(dispatcher, nullptr);
    for (const auto& item : dispatcher->cases) {
        EXPECT_GE(item.key, 1);
        EXPECT_LE(item.key, INT32_MAX);
    }

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

TEST(RegTransformPassTest, All829OptionsDefaultOff)
{
    RegTransformOptions options;
    // 基础三项默认关闭（RegC 高性能定位：默认产物零混淆）。
    EXPECT_FALSE(options.enableMba);
    EXPECT_FALSE(options.enableOpaquePredicates);
    EXPECT_FALSE(options.enableControlFlowFlattening);
    // #829 增强项默认关闭（fail-closed 基线）。
    EXPECT_EQ(options.cffDeadBlockDensity, 0U);
    EXPECT_FALSE(options.cffStateEncryption);
    EXPECT_FALSE(options.mbaDeep);
    EXPECT_FALSE(options.opaqueZero);
    // 批次 2：立即数隐藏默认关闭（每立即数一次运行时调用，最重的一项）。
    EXPECT_FALSE(options.hideImmediateConstants);
    // 调用目标间接化默认关闭（每次调用多一层 dispatcher 分派）。
    EXPECT_FALSE(options.indirectCallTargets);
    EXPECT_EQ(options.transformSeed, 0ULL);
}

// ===========================================================================
// 调用目标间接化（对标 jvmtp obfuscation.calls.*）
// ===========================================================================

// 默认关闭：不置任何标记（fail-closed 基线）。
TEST(RegTransformPassTest, IndirectCallOffByDefault)
{
    RegFunction function = makeCallFunction("com/app/Foo",
                                            {{"java/lang/Math", "max", "(II)I", Opcodes::INVOKESTATIC}});

    const auto stats = applyRegTransformPasses(function);

    EXPECT_EQ(stats.callsIndirected, 0U);
    EXPECT_EQ(indirectCallFlags(function), std::vector<bool>({false}));
}

// 非自递归调用一律标记；自递归静态调用跳过（走 self-call 直调路径）。
TEST(RegTransformPassTest, IndirectCallMarksCallsButSkipsSelfRecursion)
{
    RegFunction function = makeCallFunction(
        "com/app/Foo",
        {
            {"java/lang/Math", "max", "(II)I", Opcodes::INVOKESTATIC},
            {"com/app/Foo", "helper", "(I)I", Opcodes::INVOKESTATIC},   // 自递归
            {"java/lang/String", "length", "()I", Opcodes::INVOKEVIRTUAL},
            {"com/app/Bar", "build", "()V", Opcodes::INVOKESPECIAL},
        });

    RegTransformOptions options;
    options.indirectCallTargets = true;
    const auto stats           = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.callsIndirected, 3U);
    EXPECT_EQ(indirectCallFlags(function), std::vector<bool>({true, false, true, true}));
}

// 标记不改变 RegIR 结构：仍是无栈形态，DCE/验证面对 indirectCall 无感
// （发射端才按标记分流，IR 层只是多一个布尔）。
TEST(RegTransformPassTest, IndirectCallKeepsRegIRValid)
{
    RegFunction function = makeCallFunction(
        "com/app/Foo",
        {
            {"java/lang/Math", "max", "(II)I", Opcodes::INVOKESTATIC},
            {"java/lang/String", "length", "()I", Opcodes::INVOKEVIRTUAL},
        });

    RegTransformOptions options;
    options.indirectCallTargets = true;
    applyRegTransformPasses(function, options);

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
    EXPECT_EQ(function.blocks.size(), 1U);
    EXPECT_EQ(function.blocks.front()->instructions.size(), 2U);
}

// 标记与 seed 无关（可复现构建不变量：站点表由发射端按遍历序建，
// seed 只影响 MBA/opaque/CFF 的变体选择）。
TEST(RegTransformPassTest, IndirectCallIsSeedIndependent)
{
    auto fingerprint = [](uint64_t seed) {
        RegFunction function = makeCallFunction(
            "com/app/Foo",
            {
                {"java/lang/Math", "max", "(II)I", Opcodes::INVOKESTATIC},
                {"java/lang/String", "length", "()I", Opcodes::INVOKEVIRTUAL},
            });
        RegTransformOptions options;
        options.indirectCallTargets = true;
        options.transformSeed       = seed;
        applyRegTransformPasses(function, options);
        std::string out;
        for (bool flag : indirectCallFlags(function)) out += flag ? "1" : "0";
        return out;
    };
    EXPECT_EQ(fingerprint(0), fingerprint(1));
    EXPECT_EQ(fingerprint(0), fingerprint(0xDEADBEEFCAFE1234ull));
}

// 与立即数隐藏叠加：pass 顺序是 calls → constants，两者互不破坏对方的标记
// （立即数隐藏插合成 Intrinsic，调用指令的 indirectCall 标记不丢）。
TEST(RegTransformPassTest, IndirectCallComposesWithConstantHiding)
{
    RegFunction function = makeCallFunction(
        "com/app/Foo", {{"java/lang/Math", "max", "(II)I", Opcodes::INVOKESTATIC}});
    // 追加一个带立即数的纯指令，供立即数隐藏改写。
    RegInstruction add;
    add.kind     = RegInstructionKind::Binary;
    add.hasDest  = true;
    add.dest     = RegValue{RegId{9}, SSAType::I32};
    add.lhs      = RegOperand::fromRegister(RegValue{RegId{0}, SSAType::I32});
    add.rhs      = RegOperand::fromInt(0x5EED, SSAType::I32);
    add.binaryOp = SSABinaryOp::Add;
    add.effect   = SSAEffect::pure();
    function.blocks.front()->instructions.push_back(add);

    RegTransformOptions options;
    options.indirectCallTargets    = true;
    options.hideImmediateConstants = true;
    const auto stats               = applyRegTransformPasses(function, options);

    EXPECT_EQ(stats.callsIndirected, 1U);
    EXPECT_GT(stats.constantsHidden, 0U);
    EXPECT_EQ(indirectCallFlags(function), std::vector<bool>({true}));

    std::string validationReason;
    EXPECT_TRUE(validateRegIRNoStackSlots(function, validationReason)) << validationReason;
}

// VMC4 虚拟机后端单元测试：Java 字节码 -> L1 -> SSA -> RegIR -> VmEncoder ->
// VmInterpreterEmitter -> C 解释器源码。
//
// 本测试不调用 JNI/DLL（那是 e2e 测试的职责），只验证：
//   1. 纯计算方法能被编码成 VmProgram（非空、含 bytecodes/常量池）。
//   2. 生成的 C 解释器源码结构正确（含 mj2x_vm_slot、解释器函数、opcode 数组）。
//   3. 含对象操作的方法 fail-closed（编码返回 nullptr）。
//   4. 5 种 dispatcher 形态都生成各自特征结构。

#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/vmc4/vm_encoder.hpp"
#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const std::string& name,
                                       const std::string& desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access,
                                           ca::str::Utf8StringRef::from_cstr(name.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(desc.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

/// 完整编码管线：MethodNode -> VmProgram（复用 regc_test 的 emitRegC 结构，末段换 VmEncoder）。
std::unique_ptr<VmProgram> encodeMethod(const std::string& name, const std::string& desc,
                                        std::initializer_list<AbstractInsnNode*> insns,
                                        int maxStack, int maxLocals, const VmInstance& instance,
                                        std::string& reason, const RegFunction** outReg = nullptr)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack,
                         maxLocals);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmc4", name, desc, true, maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    if (outReg) *outReg = reg.get();
    return VmEncoder::encode(*reg, instance, reason);
}

/// @brief 计算 RegIR 中使用的最大寄存器索引（与 EncoderState::scanComputeRegisters
/// 同口径：指令 dest + Register 操作数 + terminator 操作数）。
uint32_t maxComputeRegister(const RegFunction& reg)
{
    uint32_t maxId = 0;
    auto     scan  = [&maxId](const RegOperand& op) {
        if (op.kind == RegOperandKind::Register && op.reg.value > maxId)
            maxId = static_cast<uint32_t>(op.reg.value);
    };
    for (const auto& blk : reg.blocks) {
        for (const auto& ins : blk->instructions) {
            if (ins.hasDest && ins.dest.id.value > maxId)
                maxId = static_cast<uint32_t>(ins.dest.id.value);
            switch (ins.kind) {
            case RegInstructionKind::Copy:
            case RegInstructionKind::Unary:
            case RegInstructionKind::Convert:
                scan(ins.value);
                break;
            case RegInstructionKind::Binary:
            case RegInstructionKind::Compare:
                scan(ins.lhs);
                scan(ins.rhs);
                break;
            default: break;
            }
        }
        const auto& term = blk->terminator;
        std::visit([&scan](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                scan(t.condition);
            } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (t.hasValue) scan(t.value);
            }
        },
                   term);
    }
    return maxId;
}

}   // namespace

// ===========================================================================
// 测试 1：纯算术方法成功编码。
// ===========================================================================
TEST(Vmc4BackendTest, EncodesPureArithmeticMethod)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // int addMul(int a, int b) { return (a + b) * 3; }
    auto prog = encodeMethod("addMul", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::ICONST_3),
                                 new InsnNode(Opcodes::IMUL),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             3, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->bytecodes.empty()) << "bytecode stream must be non-empty";
    EXPECT_GE(prog->numInputs, 2u) << "two int params";
    EXPECT_GE(prog->numRegs, prog->numInputs) << "numRegs >= numInputs";
}

// ===========================================================================
// 测试 2：含循环的方法成功编码（验证控制流 terminator 编码）。
// ===========================================================================
TEST(Vmc4BackendTest, EncodesLoopMethod)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // int sum(int n) { int s=0; for(int i=0;i<n;i++){s+=i;} return s; }
    auto* loop = new LabelNode();
    auto* done = new LabelNode();
    auto prog = encodeMethod("sum", "(I)I",
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
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->bytecodes.empty());
    // 循环意味着至少 2 个 block（loop body + exit），blockTargets 应反映这点。
    EXPECT_GE(prog->blockTargets.size(), 2u);
}

// ===========================================================================
// 测试 3：fail-closed —— 含对象操作（ALOAD + getfield）的方法编码失败。
// ===========================================================================
TEST(Vmc4BackendTest, FailClosedOnObjectFieldAccess)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // int getField(MyObj o) { return o.x; }  —— LoadField 在 MVP fail-closed
    auto prog = encodeMethod("getField", "(Ltest/MyObj;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new FieldInsnNode(Opcodes::GETFIELD,
                                                   ca::str::Utf8StringRef::from_cstr("test/MyObj"),
                                                   ca::str::Utf8StringRef::from_cstr("x"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    // 编码应失败（LoadField 不支持），返回 nullptr 并填 reason。
    EXPECT_EQ(prog, nullptr);
    EXPECT_FALSE(reason.empty());
}

// ===========================================================================
// 测试 4：生成的解释器源码结构正确。
// ===========================================================================
TEST(Vmc4BackendTest, EmitsValidInterpreterSource)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("simpleAdd", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "deadbeef");
    // emit() 只输出方法级结构；runtime prelude（mj2x_vm_slot 定义、div/rem helper）
    // 由 emitRuntimePrelude() 单独输出，finalize 时拼在前面。这里只验证方法级符号。
    EXPECT_NE(src.find("mj2x_vm_exec_deadbeef"), std::string::npos)
        << "interpreter entry function missing\n"
        << src;
    // bytecode 数组。
    EXPECT_NE(src.find("mj2x_vm_code_deadbeef"), std::string::npos)
        << "bytecode array missing\n"
        << src;
    // blockTargets 数组。
    EXPECT_NE(src.find("mj2x_vm_targets_deadbeef"), std::string::npos)
        << "blockTargets array missing\n"
        << src;
    // 常量数组（即使为空也应有声明）。
    EXPECT_NE(src.find("mj2x_vm_consts_deadbeef"), std::string::npos)
        << "constant pool array missing\n"
        << src;
}

// ===========================================================================
// 测试 5：runtime prelude 含完整 JVM 语义 helper。
// ===========================================================================
TEST(Vmc4BackendTest, RuntimePreludeHasJvmDivRemSemantics)
{
    const std::string prelude = VmInterpreterEmitter::emitRuntimePrelude();
    // INT_MIN / -1 = INT_MIN 不抛。
    EXPECT_NE(prelude.find("INT32_MIN && b == -1"), std::string::npos) << prelude;
    EXPECT_NE(prelude.find("INT64_MIN && b == -1"), std::string::npos) << prelude;
}

// ===========================================================================
// 参数化测试：5 种 dispatcher 形态都生成各自特征结构。
// ===========================================================================
struct DispatcherCase
{
    VmDispatcherKind kind;
    const char*      name;
    const char*      marker;   // 该形态的 C 结构特征字符串
};

class Vmc4DispatcherTest : public ::testing::TestWithParam<DispatcherCase>
{};

TEST_P(Vmc4DispatcherTest, EmitsDispatcherKindStructure)
{
    const auto& param = GetParam();
    Vmc4Options opts;
    opts.dispatcherKind = param.kind;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("disp", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "abc123");
    EXPECT_NE(src.find(param.marker), std::string::npos)
        << "dispatcher " << param.name << " missing marker '" << param.marker << "'\n"
        << src;
}

INSTANTIATE_TEST_SUITE_P(Vmc4Dispatchers, Vmc4DispatcherTest,
                         ::testing::Values(
                             DispatcherCase{VmDispatcherKind::SwitchTable, "switch_table",
                                            "switch (opcode)"},
                             DispatcherCase{VmDispatcherKind::FuncPtrTable, "funcptr_table",
                                            "(*mj2x_vm_handler"},
                             DispatcherCase{VmDispatcherKind::IfNest, "if_nest",
                                            "if (opcode == 0x"},
                             DispatcherCase{VmDispatcherKind::StateDispatcher, "state_dispatcher",
                                            "while (state)"}));
// 注意：ComputedGoto 用 GCC 扩展（Labels as Values），MSVC 不支持编译生成的 C，
// 但 emit 阶段（C++ 生成 C 文本字符串）本身不依赖 GCC。测试只验证 emit 出了
// "goto *" 标记，不验证生成的 C 能编译（那是 e2e + 平台守卫的职责）。
// 这里单独加一个非参数化用例验证 ComputedGoto 的 emit 特征。
TEST(Vmc4ComputedGotoEmitTest, EmitsLabelsAsValuesMarker)
{
    Vmc4Options opts;
    opts.dispatcherKind = VmDispatcherKind::ComputedGoto;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("cg", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "cgxyz");
    EXPECT_NE(src.find("goto *"), std::string::npos) << src;
    EXPECT_NE(src.find("&&mj2x_vm_op_"), std::string::npos) << src;
}

// ===========================================================================
// 回归 1（P0-1）：常量槽必须隔离在计算寄存器之后，且相同字面量去重复用。
// ===========================================================================
TEST(Vmc4BackendTest, ConstantSlotsIsolatedFromComputeRegisters)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    const RegFunction* reg = nullptr;
    // int addMul3(int a, int b) { return (a + b) * 3 + 3; }  —— 常量 3 出现两次应复用
    auto prog = encodeMethod("addMul3", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::ICONST_3),
                                 new InsnNode(Opcodes::IMUL),
                                 new InsnNode(Opcodes::ICONST_3),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             3, 2, instance, reason, &reg);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_NE(reg, nullptr);

    const uint32_t maxCompute = maxComputeRegister(*reg);
    int            threeCount = 0;
    for (const auto& e : prog->constantPool) {
        // 常量槽必须高于全部计算寄存器（含参数），否则会被计算指令的 dest 覆盖。
        EXPECT_GT(e.destSlot, maxCompute)
            << "constant slot " << e.destSlot << " overlaps compute registers [0, " << maxCompute
            << "]";
        if (e.type == VmType::I32 && e.intBits == 3) threeCount++;
    }
    EXPECT_EQ(threeCount, 1) << "literal 3 must be deduplicated to a single constant slot";
    EXPECT_GE(prog->numRegs, maxCompute + 1 + static_cast<uint32_t>(prog->constantPool.size()));
}

// ===========================================================================
// 回归 2（P0-2）：FuncPtrTable 的 dispatch body 末尾必须有 mj2x_vm_done label
// 并写回 ctx->pc/status——缺失则 RETURN/BINARY 除零分支引用未定义 label。
// ===========================================================================
TEST(Vmc4BackendTest, FuncPtrDispatcherEmitsDoneLabel)
{
    Vmc4Options opts;
    opts.dispatcherKind = VmDispatcherKind::FuncPtrTable;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("fptr", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "fptr1");
    EXPECT_NE(src.find("mj2x_vm_done:"), std::string::npos)
        << "done label missing in FuncPtrTable dispatch body\n"
        << src;
    EXPECT_NE(src.find("ctx->pc = pc; ctx->status = status;"), std::string::npos)
        << "pc/status write-back missing in FuncPtrTable dispatch body\n"
        << src;
}

// ===========================================================================
// 回归 3（P0-3）：实例方法 MVP 必须 fail-closed（跳过，不产出 C 源码）。
// ===========================================================================
TEST(Vmc4BackendTest, SkipsInstanceMethodFailClosed)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC, "bump", "()I",
                             {
                                 new InsnNode(Opcodes::ICONST_0),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1);
    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    Vmc4Backend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("Test", cls.methods[0], blocks);

    // 实例方法不转 VM：不产出 C_SOURCE，也不标 native。
    EXPECT_EQ(result.kind, BackendResult::Kind::NONE);
    EXPECT_TRUE(backend.generatedSource().empty());
    EXPECT_FALSE(method->access & Opcodes::ACC_NATIVE);
}

// ===========================================================================
// 回归 4（P1-2/P1-3）：runtime prelude 含饱和 f2i/f2l helper（无硬编码魔数），
// SwitchTable 的非法 opcode 状态用 InvalidBytecode（2）而非魔法数 3。
// ===========================================================================
TEST(Vmc4BackendTest, PreludeHasSaturatingFloatConversionsAndNoStatusMagic)
{
    const std::string prelude = VmInterpreterEmitter::emitRuntimePrelude();
    EXPECT_NE(prelude.find("mj2x_vm_f2i32"), std::string::npos) << prelude;
    EXPECT_NE(prelude.find("mj2x_vm_f2i64"), std::string::npos) << prelude;
    EXPECT_NE(prelude.find("(double)INT32_MAX + 1.0"), std::string::npos)
        << "f2i32 saturation bound must derive from standard macros\n"
        << prelude;
    EXPECT_NE(prelude.find("(double)INT64_MAX"), std::string::npos)
        << "f2i64 saturation bound must derive from standard macros\n"
        << prelude;

    Vmc4Options opts;
    opts.dispatcherKind = VmDispatcherKind::SwitchTable;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("sw", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "sw1");
    EXPECT_NE(src.find("status = (status!=0)?status:2u"), std::string::npos)
        << "invalid-bytecode fallback must use status 2\n"
        << src;
    EXPECT_EQ(src.find("?status:3u"), std::string::npos)
        << "no magic '3u' status fallback may survive\n"
        << src;
}

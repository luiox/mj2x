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
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/frontend/code_converter.hpp"
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
                                        std::string& reason)
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
    return VmEncoder::encode(*reg, instance, reason);
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

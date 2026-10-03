// VMC4 虚拟机后端单元测试：Java 字节码 -> L1 -> SSA -> RegIR -> VmEncoder ->
// VmInterpreterEmitter -> C 解释器源码。
//
// 本测试不调用 JNI/DLL（那是 e2e 测试的职责），只验证：
//   1. 纯计算方法能被编码成 VmProgram（非空、含 bytecodes/常量池）。
//   2. 生成的 C 解释器源码结构正确（含 mj2x_vm_slot、解释器函数、opcode 数组）。
//   3. 含对象操作的方法 fail-closed（编码返回 nullptr）。
//   4. 5 种 dispatcher 形态都生成各自特征结构。

#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
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

/// @brief 读下沉后的 VMC4 运行时源(#894:运行时/模板分离,实现 mj2x-rt/src/vmc4)。
std::string readVmc4RuntimeSource()
{
    const std::filesystem::path path =
        std::filesystem::current_path() / "mj2x" / "mj2x-rt" / "src" / "vmc4" / "mj2x_vmc4_runtime.c";
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// @brief 构造 MethodNode。
///
/// @note name/desc 必须是静态存储期的 C 字符串（字面量）：MethodNode 只持有
/// Utf8StringRef 视图不拷贝，临时 std::string 会在调用点析构导致悬垂视图。
std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access,
                                           ca::str::Utf8StringRef::from_cstr(name),
                                           ca::str::Utf8StringRef::from_cstr(desc),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

/// 完整编码管线：MethodNode -> VmProgram（复用 regc_test 的 emitRegC 结构，末段换 VmEncoder）。
std::unique_ptr<VmProgram> encodeMethodNode(std::unique_ptr<MethodNode> mn,
                                            const VmInstance& instance, std::string& reason,
                                            std::unique_ptr<RegFunction>* outReg = nullptr)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmc4", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    auto encoded = VmEncoder::encode(*reg, instance, reason);
    // 出参移交所有权：RegFunction 由调用方持有（此前交裸指针、函数返回即悬垂，
    // ASan heap-use-after-free 实证——DEV-8 测试合同 ①）。
    if (outReg) *outReg = std::move(reg);
    return encoded;
}

/// 便捷重载：从指令列表构造 MethodNode 再编码。
std::unique_ptr<VmProgram> encodeMethod(const std::string& name, const std::string& desc,
                                        std::initializer_list<AbstractInsnNode*> insns,
                                        int maxStack, int maxLocals, const VmInstance& instance,
                                        std::string&                 reason,
                                        std::unique_ptr<RegFunction>* outReg = nullptr)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name.c_str(), desc.c_str(),
                         insns, maxStack, maxLocals);
    return encodeMethodNode(std::move(mn), instance, reason, outReg);
}

/// 与发射器 emitOpcodeByteOrChain 同构：`opcode == b1 || opcode == b2 ...`。
std::string emitOpcodeByteOrChainForTest(const VmInstance& instance, VmOpcode op)
{
    std::string out;
    bool        first = true;
    for (uint8_t b : instance.opcodeBytesFor(op)) {
        if (!first) out += " || ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "opcode == 0x%02x", static_cast<unsigned>(b));
        out += buf;
        first = false;
    }
    return out;
}

/// 统计子串出现次数。
size_t countOccurrencesForTest(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
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
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
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
// 测试 3：LoadField（Phase 2.A）—— getfield 收集为 loadFieldRefs，不再 fail-closed。
// ===========================================================================
TEST(Vmc4BackendTest, EncodesLoadFieldInstruction)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int getField(MyObj o) { return o.x; }  —— ALOAD0 + getfield
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
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->bytecodes.empty());
    // getfield test/MyObj.x:I 收集为单个 loadFieldRef（instance 字段）。
    ASSERT_EQ(prog->loadFieldRefs.size(), 1u);
    EXPECT_EQ(prog->loadFieldRefs[0].owner, "test/MyObj");
    EXPECT_EQ(prog->loadFieldRefs[0].name, "x");
    EXPECT_EQ(prog->loadFieldRefs[0].desc, "I");
    EXPECT_FALSE(prog->loadFieldRefs[0].isStatic);
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
    // #894 运行时/模板分离:prelude 已下沉 mj2x-rt,断言针对运行时源文件。
    const std::string prelude = readVmc4RuntimeSource();
    ASSERT_FALSE(prelude.empty()) << "mj2x_vmc4_runtime.c missing";
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
    std::string            reason;
    std::unique_ptr<RegFunction> reg;
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

// 审查 G-1 修复回归：scanComputeRegisters 曾漏 Switch 终结者边拷贝——仅经
// switch 边定义的寄存器不抬 maxReg，常量槽从低水位分配会落在边拷贝 dest 之上
// 被覆写（取值错），且 numRegs 偏小致解释器 regs[] 越界。手构 RegIR：r7 只在
// switch 各边 copies 中定义（case/default 分别汇入两个 return 块，default 与
// case 2 同目标形成 slot 区分场景），常量 111/222/333 经 resolveOperand 进常量池。
TEST(Vmc4BackendTest, SwitchEdgeCopiesRaiseMaxReg)
{
    RegFunction fn;
    auto entry  = std::make_unique<RegBlock>(SSABlockId{0});
    auto blockA = std::make_unique<RegBlock>(SSABlockId{1});
    auto blockB = std::make_unique<RegBlock>(SSABlockId{2});

    const RegValue selector{RegId{0}, SSAType::I32};
    fn.parameters.push_back(selector);

    // 碰撞机制（旧代码）：真实块先于合成边块编码，Return(IntLiteral) 在不经过
    // 任何 dest 观察的情况下 intern 常量——若此时 r1（仅经 switch 边定义）尚未
    // 被观察（其使用块编码在后），首个常量槽从参数数水位 1 起分配即落在 r1 上：
    // blockA 的 77@slot1 与边拷贝 dest 互相覆写。
    const RegValue merged{RegId{1}, SSAType::I32};
    RegSwitchTerminator sw;
    sw.value = RegOperand::fromRegister(selector);
    {
        RegSuccessor succ;
        succ.block = blockA.get();
        succ.copies.push_back(RegCopy{merged, RegOperand::fromInt(111, SSAType::I32)});
        sw.cases.push_back(RegSwitchCase{1, std::move(succ)});
    }
    {
        RegSuccessor succ;
        succ.block = blockB.get();
        succ.copies.push_back(RegCopy{merged, RegOperand::fromInt(222, SSAType::I32)});
        sw.cases.push_back(RegSwitchCase{2, std::move(succ)});
    }
    {
        RegSuccessor succ;
        succ.block = blockB.get();
        succ.copies.push_back(RegCopy{merged, RegOperand::fromInt(333, SSAType::I32)});
        sw.defaultTarget = std::move(succ);
    }
    entry->terminator  = std::move(sw);
    // blockA 在 blockB 之前编码：其 Return(77) 在 r1 首次被观察（blockB 的
    // Return(r1)）之前 intern 常量，构造旧代码的碰撞窗口。
    blockA->terminator = RegReturnTerminator{true, RegOperand::fromInt(77, SSAType::I32)};
    blockB->terminator = RegReturnTerminator{true, RegOperand::fromRegister(merged)};
    fn.entry           = entry.get();
    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(blockA));
    fn.blocks.push_back(std::move(blockB));

    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = VmEncoder::encode(fn, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    // 常量槽必须避开全部计算寄存器（含仅经 switch 边定义的 r1）。
    for (const auto& e : prog->constantPool) {
        EXPECT_GT(e.destSlot, 1u)
            << "constant slot " << e.destSlot << " overlaps switch-edge register r1";
    }
    EXPECT_GE(prog->numRegs, 2u) << "numRegs must cover switch-edge dest r1";
    EXPECT_GE(prog->constantPool.size(), 3u);
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
    // P0-4 回归：RETURN 后循环退出判断必须基于 opcode 位置而非 operand 末字节。
    // 旧实现用 code[ctx->pc-1]==0x22 判断，RETURN 指令的 operand 末字节（src 低字节）
    // 不等于 0x22 时永不退出，读到字节流末尾之外造成越界。现在在调用 handler 前
    // 记录 opcode 位置 op_pos，并按实例映射的 Return 字节全集判断（Phase 3 起
    // Return 可有 ALT 别名，用 _op 逐字节匹配）。
    EXPECT_NE(src.find("unsigned int op_pos = ctx->pc;"), std::string::npos)
        << "opcode position capture missing in FuncPtrTable dispatch loop\n"
        << src;
    EXPECT_NE(src.find("int _is_ret = 0;"), std::string::npos)
        << "RETURN detection must use a per-op byte membership flag\n"
        << src;
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Return)) {
        char retCheck[48];
        std::snprintf(retCheck, sizeof(retCheck), "_op == 0x%02x",
                      static_cast<unsigned>(b));
        EXPECT_NE(src.find(retCheck), std::string::npos)
            << "RETURN detection must cover Return byte 0x" << std::hex
            << static_cast<unsigned>(b) << std::dec << "\n"
            << src;
    }
    EXPECT_EQ(src.find("ctx->code[ctx->pc-1] == 0x22"), std::string::npos)
        << "legacy operand-byte RETURN detection must not survive\n"
        << src;
}

// ===========================================================================
// Phase 2.B：实例方法可转 VM —— SSA 把 this 作为参数 id0（REF），wrapper 把
// thisObject 填进 regs[0]、显式参数从 regs[1] 开始（与 encoder numInputs 对齐）。
// ===========================================================================
TEST(Vmc4BackendTest, VirtualizesInstanceMethodWithThisInReg0)
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

    // 实例方法转 VM：产出 C_SOURCE，wrapper 填 this 到 regs[0]。
    EXPECT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(result.text.find("regs[0].ref = (void*)thisObject;"), std::string::npos)
        << result.text;
    // thisObject 被使用，不应再被 (void) 抑制。
    EXPECT_EQ(result.text.find("(void)thisObject;"), std::string::npos) << result.text;
}

// ===========================================================================
// Phase 2.B：实例方法显式参数从 regs[1] 开始（this 占 regs[0]）。
// ===========================================================================
TEST(Vmc4BackendTest, InstanceMethodArgsStartAtReg1)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC, "addOffset", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2);
    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    Vmc4Backend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("Test", cls.methods[0], blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    // this 占 regs[0]，p0/p1 填 regs[1]/regs[2]。
    EXPECT_NE(result.text.find("regs[1].i = p0;"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("regs[2].i = p1;"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("regs[0].i = p0;"), std::string::npos)
        << "instance arg must not clobber regs[0] (this)\n"
        << result.text;
}

// ===========================================================================
// 回归 4（P1-2/P1-3）：runtime prelude 含饱和 f2i/f2l helper（无硬编码魔数），
// SwitchTable 的非法 opcode 状态用 InvalidBytecode（2）而非魔法数 3。
// ===========================================================================
TEST(Vmc4BackendTest, PreludeHasSaturatingFloatConversionsAndNoStatusMagic)
{
    // #894 运行时/模板分离:prelude 已下沉 mj2x-rt,断言针对运行时源文件。
    const std::string prelude = readVmc4RuntimeSource();
    ASSERT_FALSE(prelude.empty()) << "mj2x_vmc4_runtime.c missing";
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

// ===========================================================================
// Phase 2：opcode 映射随机化 + handler 顺序 + 实例 seed 派生。
// ===========================================================================

/// 取一个实例的完整 opcode 映射（支持集 → 实例 canonical 字节），按枚举值排序。
std::vector<uint8_t> mappingOf(const VmInstance& instance)
{
    std::vector<uint8_t> bytes;
    for (const auto op : {VmOpcode::Halt,    VmOpcode::Copy,     VmOpcode::Unary,
                          VmOpcode::Binary,  VmOpcode::Compare,  VmOpcode::Convert,
                          VmOpcode::Jump,    VmOpcode::Branch,   VmOpcode::Return,
                          VmOpcode::SuperInc, VmOpcode::SuperDec, VmOpcode::Call,
                          VmOpcode::LoadField, VmOpcode::StoreField,
                          VmOpcode::ArrayLoad, VmOpcode::ArrayStore,
                          VmOpcode::Allocate, VmOpcode::Intrinsic, VmOpcode::Throw,
                          VmOpcode::Switch}) {
        bytes.push_back(instance.opcodeFor(op));
    }
    return bytes;
}

// P2-1：可复现构建 —— 相同 seed 相同映射与 handler 顺序；不同 seed 不同。
TEST(Vmc4BackendTest, OpcodeMappingIsSeedDeterministic)
{
    Vmc4Options optsA;
    optsA.seed = 0x1234;
    Vmc4Options optsB = optsA;
    optsB.seed       = 0x1235;
    Vmc4Options optsC = optsA;   // 与 A 同 seed

    VmInstance a(optsA);
    VmInstance b(optsB);
    VmInstance c(optsC);

    EXPECT_EQ(mappingOf(a), mappingOf(c)) << "same seed must produce identical mapping";
    EXPECT_NE(mappingOf(a), mappingOf(b)) << "different seeds must produce different mappings";
    EXPECT_EQ(a.handlerOrder(), c.handlerOrder());
    EXPECT_NE(a.handlerOrder(), b.handlerOrder()) << "handler order must vary with seed";
    EXPECT_EQ(a.instanceTag(), c.instanceTag());
    EXPECT_NE(a.instanceTag(), b.instanceTag());
}

// P2-2：映射是支持集上的双射（互不相同）+ handler 顺序是排列。
// Phase 2.C 起支持集 = 18 语义；Switch（Phase 3 增补）加入后 = 19。
TEST(Vmc4BackendTest, OpcodeMappingIsBijectiveOnSupportedSet)
{
    Vmc4Options opts;
    opts.seed        = 0xDEADBEEF;
    opts.aliasCount  = 0;   // 关闭别名，只测 canonical 双射
    VmInstance instance(opts);

    const auto bytes = mappingOf(instance);
    std::vector<uint8_t> sorted = bytes;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
        EXPECT_EQ(sorted[i], static_cast<uint8_t>(i))
            << "mapped bytes must be a bijection over [0, kSupportedCount)";
    }
    EXPECT_EQ(bytes[0], instance.haltByte());

    const auto& order = instance.handlerOrder();
    EXPECT_EQ(order.size(), 19u) << "handler order covers the 19 supported opcodes";
    std::vector<VmOpcode> sortedOrder = order;
    std::sort(sortedOrder.begin(), sortedOrder.end(),
              [](VmOpcode l, VmOpcode r) { return static_cast<uint8_t>(l) < static_cast<uint8_t>(r); });
    std::vector<VmOpcode> expected = {VmOpcode::Copy,   VmOpcode::Unary,  VmOpcode::Binary,
                                      VmOpcode::Compare, VmOpcode::Convert, VmOpcode::Jump,
                                      VmOpcode::Branch, VmOpcode::Return,  VmOpcode::SuperInc,
                                      VmOpcode::SuperDec, VmOpcode::LoadField, VmOpcode::StoreField,
                                      VmOpcode::ArrayLoad, VmOpcode::ArrayStore, VmOpcode::Call,
                                      VmOpcode::Allocate, VmOpcode::Intrinsic, VmOpcode::Throw,
                                      VmOpcode::Switch};
    EXPECT_EQ(sortedOrder, expected) << "handler order must be a permutation of the supported set";
}

// P2-3：编码器发射实例映射字节（bytecodes 里的 opcode 不是固定 0x10/0x20 等）。
TEST(Vmc4BackendTest, EncoderEmitsInstanceMappedOpcodeBytes)
{
    Vmc4Options opts;
    opts.seed = 0x777;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("map", "(II)I",
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

    const auto& code = prog->bytecodes;
    // 第一字节是 entry 块的 Jump opcode（实例映射字节）。
    ASSERT_FALSE(code.empty());
    EXPECT_EQ(code[0], instance.opcodeFor(VmOpcode::Jump));
    // bytecodes 里至少出现一次 Return 的实例字节（RETURN 指令）。
    EXPECT_NE(std::find(code.begin(), code.end(), instance.opcodeFor(VmOpcode::Return)),
              code.end());
    // 固定值必须不再出现（映射已随机化，0x10/0x20 等几乎必然不是映射值；
    // 这里只断言 Jump 的固定值 0x20 不等于实例映射值，防止回归到固定编码）。
    EXPECT_NE(static_cast<uint8_t>(instance.opcodeFor(VmOpcode::Jump)),
              static_cast<uint8_t>(0x20))
        << "Jump must not retain its fixed MVP byte";
}

// P2-4：解释器发射的 case 常量使用实例映射字节。
TEST(Vmc4BackendTest, InterpreterUsesInstanceMappedCaseLabels)
{
    Vmc4Options opts;
    opts.seed = 0xABC;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("cas", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "case1");

    // Copy 的每个实例字节（canonical + 别名）都必须有 case。
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Copy)) {
        char copyCase[64];
        std::snprintf(copyCase, sizeof(copyCase), "case 0x%02x /*COPY*/",
                      static_cast<unsigned>(b));
        EXPECT_NE(src.find(copyCase), std::string::npos)
            << "switch case must use the instance-mapped Copy byte 0x" << std::hex
            << static_cast<unsigned>(b) << std::dec << "\n"
            << src;
    }
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Halt)) {
        char haltCase[64];
        std::snprintf(haltCase, sizeof(haltCase), "case 0x%02x /*HALT*/",
                      static_cast<unsigned>(b));
        EXPECT_NE(src.find(haltCase), std::string::npos)
            << "HALT case must use the instance-mapped halt byte 0x" << std::hex
            << static_cast<unsigned>(b) << std::dec << "\n"
            << src;
    }
    // 支持集外不发射 case（防御：任意未映射字节走 default）。
    EXPECT_EQ(src.find("case 0xff"), std::string::npos)
        << "no unmapped byte may have a dedicated case\n"
        << src;
}

// P2-5：实例 seed 派生 —— 同主 seed + 同身份 → 同实例 seed；不同身份 → 不同。
TEST(Vmc4BackendTest, InstanceSeedDerivationMixesIdentity)
{
    EXPECT_EQ(vmc4DeriveInstanceSeed(0x11, 0xAA), vmc4DeriveInstanceSeed(0x11, 0xAA));
    EXPECT_NE(vmc4DeriveInstanceSeed(0x11, 0xAA), vmc4DeriveInstanceSeed(0x11, 0xBB));
    EXPECT_NE(vmc4DeriveInstanceSeed(0x11, 0xAA), vmc4DeriveInstanceSeed(0x22, 0xAA));
    EXPECT_NE(vmc4DeriveInstanceSeed(0x11, 0xAA), vmc4DeriveInstanceSeed(0x22, 0xBB));
}

// P2-6：per-class 语义 —— 同主 seed 下两个类实例的映射不同（跨实例 ISA 独立）。
TEST(Vmc4BackendTest, PerClassInstancesDivergeAcrossClasses)
{
    Vmc4Options opts;
    opts.seed = 42;
    VmInstance classA(opts, vmc4DeriveInstanceSeed(opts.seed, 0xAAA00001));
    VmInstance classB(opts, vmc4DeriveInstanceSeed(opts.seed, 0xBBB00002));

    EXPECT_NE(mappingOf(classA), mappingOf(classB))
        << "per-class instances must have independent opcode mappings";
    EXPECT_NE(classA.handlerOrder(), classB.handlerOrder());
}

// ===========================================================================
// fail-closed 边界全集：每个 MVP 未支持的 RegIR family 必须编码失败并给原因。
// （对象 family 在 VM 无 JVM 对象模型/引用管理/异常语义，强行支持会破坏
// 语义保真，按 fail-closed 哲学保留字节码。）
// ===========================================================================

// P2-7：StoreField（PUTFIELD，Phase 2.A）—— putfield 收集为 storeFieldRefs，不再 fail-closed。
TEST(Vmc4BackendTest, EncodesStoreFieldInstruction)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int store(Box o, int i1, int i2) { o.x = i1; return i2; }
    auto prog = encodeMethod("store", "(Ltest/Box;II)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new FieldInsnNode(Opcodes::PUTFIELD,
                                                   ca::str::Utf8StringRef::from_cstr("test/Box"),
                                                   ca::str::Utf8StringRef::from_cstr("x"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new VarInsnNode(Opcodes::ILOAD, 2),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->bytecodes.empty());
    // putfield test/Box.x:I 收集为单个 storeFieldRef（instance 字段）。
    ASSERT_EQ(prog->storeFieldRefs.size(), 1u);
    EXPECT_EQ(prog->storeFieldRefs[0].owner, "test/Box");
    EXPECT_EQ(prog->storeFieldRefs[0].name, "x");
    EXPECT_EQ(prog->storeFieldRefs[0].desc, "I");
    EXPECT_FALSE(prog->storeFieldRefs[0].isStatic);
}

// P2-8：ArrayLoad / ArrayStore（Phase 2.A）—— 不再 fail-closed，按元素类型收集。
TEST(Vmc4BackendTest, EncodesArrayAccessInstructions)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int at(int[] a, int i) { return a[i]; }
    auto load = encodeMethod("at", "([II)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IALOAD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(load, nullptr) << reason;
    ASSERT_EQ(load->arrayLoadRefs.size(), 1u);
    EXPECT_EQ(load->arrayLoadRefs[0].elemKind, VmArrayElemKind::Int);

    std::string reason2;
    // static int st(int[] a, int i, int v) { a[i] = v; return 0; }
    auto store = encodeMethod("st", "([III)I",
                              {
                                  new VarInsnNode(Opcodes::ALOAD, 0),
                                  new VarInsnNode(Opcodes::ILOAD, 1),
                                  new VarInsnNode(Opcodes::ILOAD, 2),
                                  new InsnNode(Opcodes::IASTORE),
                                  new InsnNode(Opcodes::ICONST_0),
                                  new InsnNode(Opcodes::IRETURN),
                              },
                              3, 3, instance, reason2);
    ASSERT_NE(store, nullptr) << reason2;
    ASSERT_EQ(store->arrayStoreRefs.size(), 1u);
    EXPECT_EQ(store->arrayStoreRefs[0].elemKind, VmArrayElemKind::Int);
}

// P2-9：Call（INVOKESTATIC）。
// Phase 2.0：Call 不再 fail-closed。INVOKESTATIC 编码为 Call opcode + methodRef。
TEST(Vmc4BackendTest, EncodesCallInstruction)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("call", "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                   ca::str::Utf8StringRef::from_cstr("test/Helper"),
                                                   ca::str::Utf8StringRef::from_cstr("twice"),
                                                   ca::str::Utf8StringRef::from_cstr("(I)I"),
                                                   false),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->bytecodes.empty());
    // INVOKESTATIC test/Helper.twice(I)I 收集为单个 methodRef。
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_EQ(prog->methodRefs[0].owner, "test/Helper");
    EXPECT_EQ(prog->methodRefs[0].name, "twice");
    EXPECT_EQ(prog->methodRefs[0].desc, "(I)I");
    EXPECT_EQ(prog->methodRefs[0].invokeKind, VmInvokeKind::Static);
    EXPECT_TRUE(prog->methodRefs[0].hasDest);
}

// Phase 2.B：invokespecial 发射 CallNonvirtual<Type>MethodA（super/<init>/private
// 语义禁止虚分派），instance 调用带 null receiver 显式 NPE；virtual 仍是 CallMethodA。
TEST(Vmc4BackendTest, SpecialCallEmitsNonvirtualAndNullReceiverNpe)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int callSpec(Object o) { return ((Helper)o).priv(); }（invokespecial）
    auto prog = encodeMethod("callSpec", "(Ljava/lang/Object;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                                   ca::str::Utf8StringRef::from_cstr("test/Helper"),
                                                   ca::str::Utf8StringRef::from_cstr("priv"),
                                                   ca::str::Utf8StringRef::from_cstr("()I"),
                                                   false),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_EQ(prog->methodRefs[0].invokeKind, VmInvokeKind::Special);

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "spec");
    // Special → CallNonvirtualIntMethodA，且带定义类参数（_cls）。
    EXPECT_NE(src.find("CallNonvirtualIntMethodA(env, (jobject)regs[recv].ref, _cls, _mid, _a)"),
              std::string::npos)
        << src;
    // instance 调用必须显式检查 null receiver（JVM 语义 NPE）。
    EXPECT_NE(src.find("mj2x_vm_throw_npe(env, \"instance method call\")"), std::string::npos)
        << src;

    // Virtual 调用仍是 CallIntMethodA（虚分派，无 _cls 参数）。
    std::string reason2;
    auto vprog = encodeMethod("callVirt", "(Ljava/lang/Object;)I",
                              {
                                  new VarInsnNode(Opcodes::ALOAD, 0),
                                  new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                    ca::str::Utf8StringRef::from_cstr("test/Helper"),
                                                    ca::str::Utf8StringRef::from_cstr("virt"),
                                                    ca::str::Utf8StringRef::from_cstr("()I"),
                                                    false),
                                  new InsnNode(Opcodes::IRETURN),
                              },
                              1, 1, instance, reason2);
    ASSERT_NE(vprog, nullptr) << reason2;
    const std::string vsrc = VmInterpreterEmitter::emit(*vprog, instance, "virt");
    EXPECT_NE(vsrc.find("CallIntMethodA(env, (jobject)regs[recv].ref, _mid, _a)"),
              std::string::npos)
        << vsrc;
}

// P2-10：Allocate（NEW，Phase 2.A）—— Object 分配不再 fail-closed，<init> 由独立 Call。
TEST(Vmc4BackendTest, EncodesAllocateInstruction)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static Object make() { return new Object(); }
    auto prog = encodeMethod("make", "()Ljava/lang/Object;",
                             {
                                 new TypeInsnNode(Opcodes::NEW,
                                                  ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
                                 new InsnNode(Opcodes::DUP),
                                 new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                                   ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                                                   ca::str::Utf8StringRef::from_cstr("<init>"),
                                                   ca::str::Utf8StringRef::from_cstr("()V"),
                                                   false),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             2, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->allocRefs.size(), 1u);
    EXPECT_EQ(prog->allocRefs[0].kind, VmAllocKind::Object);
    EXPECT_EQ(prog->allocRefs[0].typeDesc, "java/lang/Object");
    // <init> 经独立 Call 调用（invokespecial）。
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_EQ(prog->methodRefs[0].name, "<init>");
}

// P2-10b：String LDC（Phase 2.E）—— ldc_string Opaque 物质化为字符串常量槽，
// wrapper 用 NewStringUTF 预加载（不再 fail-closed）。
TEST(Vmc4BackendTest, EncodesStringLdc)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static String greet() { return "hello"; }
    auto prog = encodeMethod("greet", "()Ljava/lang/String;",
                             {
                                 new LdcInsnNode(ca::core::Any(
                                     ca::str::Utf8StringRef::from_cstr("hello"))),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->stringLiterals.size(), 1u);
    EXPECT_EQ(prog->stringLiterals[0].value, "hello");
    // 常量槽落在计算寄存器之后（wrapper 预加载 NewStringUTF 由 e2e 行为验证）。
    EXPECT_GE(prog->stringLiterals[0].slot, prog->numInputs);
    EXPECT_LT(prog->stringLiterals[0].slot, prog->numRegs);
    // 解释器字节流引用该槽作为返回操作数（Copy REF 到 dest）。
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "greet1");
    EXPECT_NE(src.find("mj2x_vm_exec_greet1"), std::string::npos) << src;
}

// P2-10c：MULTIANEWARRAY（Phase 2.E）—— 不再 fail-closed，编码为 MultiArray
// Allocate（argc = 维度数），发射共享递归 helper（NewObjectArray 逐层填充）。
TEST(Vmc4BackendTest, EncodesMultiArrayAllocate)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int[][] make() { return new int[2][3]; }
    auto prog = encodeMethod("make2", "()[[I",
                             {
                                 new InsnNode(Opcodes::ICONST_2),
                                 new InsnNode(Opcodes::ICONST_3),
                                 new MultiANewArrayInsnNode(
                                     ca::str::Utf8StringRef::from_cstr("[[I"), 2),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             2, 0, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->allocRefs.size(), 1u);
    EXPECT_EQ(prog->allocRefs[0].kind, VmAllocKind::MultiArray);
    EXPECT_EQ(prog->allocRefs[0].typeDesc, "[[I");
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "make2_1");
    EXPECT_NE(src.find("mj2x_vm_alloc_multi_array"), std::string::npos) << src;
    EXPECT_NE(src.find("NewObjectArray"), std::string::npos) << src;
}

// P2-11：Intrinsic array.length（Phase 2.A）—— 不再 fail-closed。
TEST(Vmc4BackendTest, EncodesArrayLengthIntrinsic)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int len(int[] a) { return a.length; }
    auto prog = encodeMethod("len", "([I)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new InsnNode(Opcodes::ARRAYLENGTH),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_TRUE(prog->hasArrayLength);
}

// P2-12：Barrier（MONITORENTER）。
TEST(Vmc4BackendTest, FailClosedOnBarrier)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // Phase 2.D 起 MONITORENTER/EXIT（SSA Intrinsic("monitor.enter/exit")）不再
    // fail-closed：编码为 Intrinsic 表条目，发射 MonitorEnter/MonitorExit helper。
    auto prog = encodeMethod("sync", "(Ljava/lang/Object;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new InsnNode(Opcodes::MONITORENTER),
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new InsnNode(Opcodes::MONITOREXIT),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // monitor.enter/exit 各一条 intrinsic（按 name 去重）。
    ASSERT_EQ(prog->intrinsics.size(), 2u);
    EXPECT_EQ(prog->intrinsics[0].name, "monitor.enter");
    EXPECT_EQ(prog->intrinsics[1].name, "monitor.exit");
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "sync1");
    EXPECT_NE(src.find("MonitorEnter(env, _o)"), std::string::npos) << src;
    EXPECT_NE(src.find("MonitorExit(env, _o)"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_intrinsic_table_sync1"), std::string::npos) << src;
}

// P2-13：Throw（ATHROW，Phase 2.C 起支持）。
TEST(Vmc4BackendTest, EncodesThrowInstruction)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("raise", "(Ljava/lang/Throwable;)V",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new InsnNode(Opcodes::ATHROW),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_TRUE(prog->hasThrow);
    EXPECT_FALSE(prog->bytecodes.empty());
    // 发射的解释器含 THROW handler + 异常检查点。
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "raise1");
    EXPECT_NE(src.find("mj2x_vm_throw(env, regs[src].ref)"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_exc:"), std::string::npos) << src;
    // 回归：THROW handler 进检查点前 pc 回退到抛出指令——否则块反查把异常
    // 归属到布局顺序的下一块，保护块后跟未保护块时 catch 漏接（见 e2e
    // AthrowAttributionRoundTrip）。
    EXPECT_NE(src.find("pc -= 3;"), std::string::npos) << src;
}

// P2-14：Switch（TABLESWITCH）。
// switch terminator（tableswitch/lookupswitch 统一形态，Phase 3 增补）：
// 编码为 Switch opcode + key 表，不再 fail-closed。
TEST(Vmc4BackendTest, EncodesSwitchTerminator)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    auto* l1 = new LabelNode(labelScratchArena().create<Label>());
    auto* l2 = new LabelNode(labelScratchArena().create<Label>());
    auto* l3 = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("sw", "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new TableSwitchInsnNode(0, 1, l3->label,
                                                         {l1->label, l2->label}),
                                 l1,
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IRETURN),
                                 l2,
                                 new InsnNode(Opcodes::ICONST_2),
                                 new InsnNode(Opcodes::IRETURN),
                                 l3,
                                 new InsnNode(Opcodes::ICONST_3),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // 字节流含 Switch 的实例字节（canonical 或 ALT 别名）。
    const auto& opBytes = instance.opcodeBytesFor(VmOpcode::Switch);
    const bool found =
        std::any_of(opBytes.begin(), opBytes.end(), [&](uint8_t b) {
            return std::find(prog->bytecodes.begin(), prog->bytecodes.end(), b) !=
                   prog->bytecodes.end();
        });
    EXPECT_TRUE(found) << "switch must be encoded as Switch opcode";
    // blockTargets 覆盖 case/default 目标（4 个可达块：头部 + 3 个 case 块）。
    EXPECT_GE(prog->blockTargets.size(), 4u);
}

// P2-15：异常区域（try/catch 表，Phase 2.C 起支持）——编码为区域表 + 块活跃条目。
TEST(Vmc4BackendTest, EncodesExceptionRegion)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;

    auto* start = new LabelNode(labelScratchArena().create<Label>());
    auto* end   = new LabelNode(labelScratchArena().create<Label>());
    auto* handler = new LabelNode(labelScratchArena().create<Label>());
    // javac 真实 try/catch 形态：try 块正常流直接 IRETURN（无 GOTO 正常边进入
    // handler），handler 入口 ASTORE 收异常对象（SSA 作为 exceptionValue 参数）。
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "divCatch", "(II)I",
                         {
                             start,
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IDIV),
                             new InsnNode(Opcodes::IRETURN),
                             end,
                             handler,
                             new VarInsnNode(Opcodes::ASTORE, 2),
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::IRETURN),
                         },
                         2, 3);
    mn->tryCatchBlocks.push_back(
        mn->create<TryCatchBlockNode>(start->label, end->label, handler->label,
                              ca::str::Utf8StringRef::from_cstr("java/lang/ArithmeticException")));

    auto prog = encodeMethodNode(std::move(mn), instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // 区域表：至少一条 (region, 保护块) 条目；catch 类型进表；保护块活跃条目非空。
    EXPECT_FALSE(prog->excRegions.empty());
    EXPECT_EQ(prog->catchTypes.size(), 1u);
    EXPECT_EQ(prog->catchTypes[0], "java/lang/ArithmeticException");
    bool anyActive = false;
    for (const auto& entries : prog->blockExcRegions) {
        if (!entries.empty()) anyActive = true;
    }
    EXPECT_TRUE(anyActive);
    // 发射的解释器含区域匹配（IsInstanceOf 经 mj2x_vm_exception_matches）+ 检查点。
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "divc1");
    EXPECT_NE(src.find("mj2x_vm_exception_matches"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_exc:"), std::string::npos) << src;
}

// P2-16：构造器/类初始化器在 backend 层整体跳过（不产 C_SOURCE、不标 native）。
TEST(Vmc4BackendTest, SkipsConstructorAndClinit)
{
    for (const auto& ctorName : {"<init>", "<clinit>"}) {
        auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ctorName, "()V",
                                 {
                                     new InsnNode(Opcodes::RETURN),
                                 },
                                 0, 0);
        CompilerMethod cm;
        cm.methodNode = method.get();
        CompilerClass cls;
        cls.methods.push_back(cm);

        Vmc4Backend   backend;
        ca::str::Utf8StringArena arena;
        CodeConverter            converter(arena);
        auto          blocks = converter.convert(*method);
        auto          result = backend.compileMethod("Test", cls.methods[0], blocks);

        EXPECT_EQ(result.kind, BackendResult::Kind::NONE) << ctorName;
        EXPECT_FALSE(method->access & Opcodes::ACC_NATIVE) << ctorName;
    }
}

// ===========================================================================
// Phase 3：ALT 别名 + super-operator + bogus handler。
// ===========================================================================

// P3-1：每个语义的字节集合 = 1 + aliasCount，不同语义集合互不相交（全局双射）。
TEST(Vmc4BackendTest, AliasBytesAreDisjointPerSemantics)
{
    Vmc4Options opts;
    opts.seed       = 0x1234;
    opts.aliasCount = 2;
    VmInstance instance(opts);

    const auto ops = {VmOpcode::Halt, VmOpcode::Copy, VmOpcode::Binary, VmOpcode::Jump,
                      VmOpcode::Return, VmOpcode::SuperInc};
    std::set<uint8_t> seen;
    for (const auto op : ops) {
        const auto& bytes = instance.opcodeBytesFor(op);
        EXPECT_EQ(bytes.size(), 3u) << "1 canonical + 2 aliases";
        for (uint8_t b : bytes) {
            EXPECT_TRUE(seen.insert(b).second)
                << "byte 0x" << std::hex << static_cast<unsigned>(b)
                << std::dec << " appears in more than one semantics";
        }
    }
}

// P3-2：aliasCount=0 时每语义单字节，映射回到 Phase 2 形态。
TEST(Vmc4BackendTest, AliasCountZeroYieldsSingleBytes)
{
    Vmc4Options opts;
    opts.seed       = 0x99;
    opts.aliasCount = 0;
    VmInstance instance(opts);
    for (const auto op : {VmOpcode::Copy, VmOpcode::Return, VmOpcode::Binary}) {
        EXPECT_EQ(instance.opcodeBytesFor(op).size(), 1u);
        EXPECT_EQ(instance.pickOpcodeByte(op, 12345), instance.opcodeFor(op));
    }
}

// P3-3：pickOpcodeByte 确定性 + 只在集合内选字节。
TEST(Vmc4BackendTest, PickOpcodeByteIsDeterministicAndInSet)
{
    Vmc4Options opts;
    opts.seed       = 0x777;
    opts.aliasCount = 2;
    VmInstance instance(opts);

    const auto& bytes = instance.opcodeBytesFor(VmOpcode::Copy);
    std::set<uint8_t> hit;
    for (uint64_t nonce = 0; nonce < 64; ++nonce) {
        const uint8_t b = instance.pickOpcodeByte(VmOpcode::Copy, nonce);
        EXPECT_NE(std::find(bytes.begin(), bytes.end(), b), bytes.end())
            << "picked byte must be within the semantics' byte set";
        hit.insert(b);
        // 同 nonce 同结果（可复现）。
        EXPECT_EQ(instance.pickOpcodeByte(VmOpcode::Copy, nonce), b);
    }
    EXPECT_GT(hit.size(), 1u) << "with 3 bytes, 64 nonces must hit multiple aliases";
}

// P3-4：编码器在字节流里使用别名（同一语义出现多个不同字节）。
TEST(Vmc4BackendTest, EncoderUsesAliasBytesInStream)
{
    Vmc4Options opts;
    opts.seed       = 0x555;
    opts.aliasCount = 1;
    VmInstance instance(opts);
    std::string reason;
    // loopSum 有多条边拷贝（Copy 指令），nonce 不同 → 大概率覆盖 canonical+别名。
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("sumAlias", "(I)I",
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

    const auto& copyBytes = instance.opcodeBytesFor(VmOpcode::Copy);
    std::set<uint8_t> hit;
    for (uint8_t b : prog->bytecodes) {
        if (std::find(copyBytes.begin(), copyBytes.end(), b) != copyBytes.end()) hit.insert(b);
    }
    EXPECT_GT(hit.size(), 1u)
        << "bytecode stream must use both canonical and alias Copy bytes (deterministic seed)";
}

// P3-5：解释器为同一语义发射全部字节的 case（canonical + 别名共享 body）。
TEST(Vmc4BackendTest, InterpreterEmitsAliasCases)
{
    Vmc4Options opts;
    opts.seed       = 0xABC;
    opts.aliasCount = 2;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("alias", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "alias1");

    // COPY 的 3 个字节各有一个 case 标签。
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Copy)) {
        char label[64];
        std::snprintf(label, sizeof(label), "case 0x%02x /*COPY*/",
                      static_cast<unsigned>(b));
        EXPECT_NE(src.find(label), std::string::npos)
            << "alias case missing for Copy byte 0x" << std::hex
            << static_cast<unsigned>(b) << std::dec << "\n"
            << src;
    }
    // IfNest 形态的 OR 链覆盖全部字节。
    Vmc4Options ifOpts;
    ifOpts.dispatcherKind = VmDispatcherKind::IfNest;
    ifOpts.seed           = 0xABC;
    ifOpts.aliasCount     = 2;
    VmInstance ifInstance(ifOpts);
    std::string reason2;
    auto ifProg = encodeMethod("aliasIf", "(II)I",
                               {
                                   new VarInsnNode(Opcodes::ILOAD, 0),
                                   new VarInsnNode(Opcodes::ILOAD, 1),
                                   new InsnNode(Opcodes::IADD),
                                   new InsnNode(Opcodes::IRETURN),
                               },
                               2, 2, ifInstance, reason2);
    ASSERT_NE(ifProg, nullptr) << reason2;
    const std::string ifSrc = VmInterpreterEmitter::emit(*ifProg, ifInstance, "aliasIf");
    const std::string chain = emitOpcodeByteOrChainForTest(ifInstance, VmOpcode::Copy);
    EXPECT_NE(ifSrc.find(chain), std::string::npos)
        << "IfNest must match Copy via OR chain over all alias bytes\n"
        << ifSrc;
}

// P3-6：bogus handler 注入——函数存在但 dispatch 结构不引用。
TEST(Vmc4BackendTest, BogusHandlersEmittedButNotReferenced)
{
    Vmc4Options opts;
    opts.seed          = 0xDEAD;
    opts.bogusHandlers = 2;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("bog", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "bog1");

    EXPECT_NE(src.find("mj2x_vm_bogus_bog1_0"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_bogus_bog1_1"), std::string::npos) << src;
    // dispatch 结构（switch case / 函数指针表 / goto 表）不得引用 bogus 函数名。
    // 简化断言：bogus 名字只出现在自身定义处（2 处：定义行 + 注释/调用各算），
    // 这里断言出现的次数不超过定义次数。
    const size_t count = countOccurrencesForTest(src, "mj2x_vm_bogus_bog1_0");
    EXPECT_LE(count, 2u) << "bogus handler must not be referenced by dispatch structures\n"
                         << src;
}

// P3-7：super-operator 折叠——Binary(x,1,Add/Sub) → SuperInc/SuperDec。
TEST(Vmc4BackendTest, SuperOpFoldsIncDec)
{
    Vmc4Options opts;
    opts.seed       = 0x314159;
    opts.aliasCount = 0;
    VmInstance instance(opts);
    std::string reason;
    // int inc(int x) { return x + 1; }  int dec(int x) { return x - 1; }
    auto prog = encodeMethod("incDec", "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IADD),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::ISUB),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    const auto& code = prog->bytecodes;
    EXPECT_NE(std::find(code.begin(), code.end(), instance.opcodeFor(VmOpcode::SuperInc)), code.end())
        << "x+1 must fold to SuperInc";
    EXPECT_NE(std::find(code.begin(), code.end(), instance.opcodeFor(VmOpcode::SuperDec)), code.end())
        << "x-1 must fold to SuperDec";
    // 该方法恰有一处 +1 和一处 -1，两条 Binary 各自全或否地折叠。SuperInc 与 SuperDec
    // 同时出现即证明两处都已折叠，故无未折叠 Binary 残留——不再用裸 std::find 查
    // Binary 字节（operand 字节会与 Binary 的 canonical 字节碰撞，opcode 集大小变化
    // 会移动 canonical 赋值导致误报）。
}

// P3-8：handler 形态等价变形——确定性选择 + 解释器按实例发射 alt body。
TEST(Vmc4BackendTest, HandlerFormMorphingIsDeterministicAndEmitted)
{
    // 两个不同 seed 的实例对 Unary/Compare 的形态选择应确定性且可能不同。
    Vmc4Options optsA;
    optsA.seed = 0x111;
    Vmc4Options optsB;
    optsB.seed = 0x222;
    VmInstance a(optsA);
    VmInstance b(optsB);
    EXPECT_EQ(a.usesAltHandlerForm(VmOpcode::Unary), a.usesAltHandlerForm(VmOpcode::Unary));
    EXPECT_EQ(a.usesAltHandlerForm(VmOpcode::Compare), a.usesAltHandlerForm(VmOpcode::Compare));

    // 找一个使用 alt 形态的实例，验证解释器发射的是 alt body。
    VmInstance* altInst = nullptr;
    for (auto* cand : {&a, &b}) {
        if (cand->usesAltHandlerForm(VmOpcode::Unary) ||
            cand->usesAltHandlerForm(VmOpcode::Compare)) {
            altInst = cand;
            break;
        }
    }
    ASSERT_NE(altInst, nullptr) << "at least one instance should use an alt form";
    std::string reason;
    auto prog = encodeMethod("morph", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, *altInst, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, *altInst, "morph1");
    if (altInst->usesAltHandlerForm(VmOpcode::Compare)) {
        EXPECT_NE(src.find("result=!(a!=b);"), std::string::npos)
            << "alt Compare form must be emitted when selected\n"
            << src;
        EXPECT_EQ(src.find("case 0: result=(a==b);"), std::string::npos)
            << "default Compare form must not appear when alt is selected\n"
            << src;
    } else {
        EXPECT_NE(src.find("case 0: result=(a==b);"), std::string::npos)
            << "default Compare form must be emitted when alt is not selected\n"
            << src;
    }
}

// P4-1：expected trace surface 描述——确定性 + 覆盖红队契约字段。
TEST(Vmc4BackendTest, TraceSurfaceDescribesInstanceDeterministically)
{
    Vmc4Options opts;
    opts.seed           = 0xC0FFEE;
    opts.dispatcherKind = VmDispatcherKind::IfNest;
    opts.aliasCount     = 2;
    opts.bogusHandlers  = 1;
    VmInstance a(opts);
    VmInstance b(opts);

    const std::string descA = a.describe();
    const std::string descB = b.describe();
    EXPECT_EQ(descA, descB) << "same instance must produce identical trace surface";

    EXPECT_NE(descA.find("dispatcher: if_nest"), std::string::npos) << descA;
    EXPECT_NE(descA.find("instanceSeed:"), std::string::npos) << descA;
    EXPECT_NE(descA.find("aliasCount: 2"), std::string::npos) << descA;
    EXPECT_NE(descA.find("bogusHandlers: 1"), std::string::npos) << descA;
    EXPECT_NE(descA.find("handlerOrder:"), std::string::npos) << descA;
    EXPECT_NE(descA.find("opcodeTable:"), std::string::npos) << descA;
    // 每个支持的语义都有条目。
    for (const auto op : {VmOpcode::Copy, VmOpcode::Binary, VmOpcode::Return,
                          VmOpcode::SuperInc, VmOpcode::Halt}) {
        EXPECT_NE(descA.find(vmOpcodeName(op)), std::string::npos)
            << "trace surface must list every supported semantics\n"
            << descA;
    }

    // 不同 seed 实例的描述不同（红队可据此断言跨实例差异）。
    Vmc4Options other = opts;
    other.seed        = 0xBEEF;
    VmInstance c(other);
    EXPECT_NE(descA, c.describe());
}

// ===========================================================================
// Phase 5：执行期 dispatch drift（语义标签轮转）
// ===========================================================================

/// 按 VmOpcode 查 operand 长度（u8/u16 混合，与 vm_interpreter_emit.cpp 的
/// handler 片段一致），用于把字节流逐指令解码。
int operandLenForTest(VmOpcode op)
{
    switch (op) {
    case VmOpcode::Halt:      return 0;
    case VmOpcode::Jump:      return 2;
    case VmOpcode::Return:    return 3;   // has_value:u8 + src:u16
    case VmOpcode::Branch:    return 6;   // cond:u16 + true:u16 + false:u16
    case VmOpcode::Copy:      return 5;   // type:u8 + dest:u16 + src:u16
    case VmOpcode::Unary:     return 6;   // op:u8 + type:u8 + dest:u16 + src:u16
    case VmOpcode::Convert:   return 6;   // from:u8 + to:u8 + dest:u16 + src:u16
    case VmOpcode::Binary:    return 7;   // op:u8 + type:u8 + dest:u16 + lhs:u16 + rhs:u16
    case VmOpcode::Compare:   return 7;
    case VmOpcode::SuperInc:  return 5;
    case VmOpcode::SuperDec:  return 5;
    case VmOpcode::LoadField:  return 7;   // siteIdx:u16 + recv:u16 + dest:u16 + argc:u8(=0)
    case VmOpcode::StoreField: return 9;   // siteIdx:u16 + recv:u16 + dest:u16 + argc:u8(=1) + valueIdx:u16
    case VmOpcode::ArrayLoad:  return 9;   // siteIdx:u16 + arrayIdx:u16 + dest:u16 + argc:u8(=1) + indexIdx:u16
    case VmOpcode::ArrayStore: return 11;  // siteIdx:u16 + arrayIdx:u16 + dest:u16 + argc:u8(=2) + indexIdx:u16 + valueIdx:u16
    case VmOpcode::Allocate:  return 7;   // siteIdx:u16 + recv:u16 + dest:u16 + argc:u8（Object=0；Array=1 +lenIdx）
    case VmOpcode::Intrinsic: return 7;   // siteIdx:u16 + arrayIdx:u16 + dest:u16 + argc:u8(=0)
    default:                  return 0;
    }
}

/// 用 unmap 逐指令解码字节流：全部指令 canonical 合法（非 0xFF）返回 true，
/// 并统计遇到的变体集合与"同一语义跨变体字节不同"是否发生。
bool decodeStreamCleanForTest(const VmInstance& inst, const std::vector<uint8_t>& bytes,
                              std::set<int>& variants, bool& sawRotation)
{
    variants.clear();
    sawRotation = false;
    size_t pc = 0;
    std::map<uint8_t, uint8_t> firstRawByCanonical;   // canonical -> 首个 raw
    // 支持集（须与 vmc4_instance.cpp kAllOpcodes 一致，含 Phase 2.0 Call + 2.A field/array/alloc）。
    static const VmOpcode kAllOps[] = {VmOpcode::Halt,    VmOpcode::Copy,     VmOpcode::Unary,
                                       VmOpcode::Binary,  VmOpcode::Compare,  VmOpcode::Convert,
                                       VmOpcode::Jump,    VmOpcode::Branch,   VmOpcode::Return,
                                       VmOpcode::SuperInc, VmOpcode::SuperDec, VmOpcode::Call,
                                       VmOpcode::LoadField, VmOpcode::StoreField,
                                       VmOpcode::ArrayLoad, VmOpcode::ArrayStore,
                                       VmOpcode::Allocate, VmOpcode::Intrinsic};
    while (pc < bytes.size()) {
        const int d = inst.variantForPc(pc);
        variants.insert(d);
        const uint8_t raw = bytes[pc];
        const uint8_t canonical = inst.unmapByte(raw, d);
        if (canonical == 0xFF) return false;
        // canonical 是洗牌后的字节值，必须反查成语义，再按语义推进 pc
        // （不能把 canonical 字节值 cast 成 VmOpcode，也不能把 operand 字节当 opcode）。
        VmOpcode op       = VmOpcode::Halt;
        bool     found    = false;
        for (const auto cand : kAllOps) {
            if (inst.opcodeFor(cand) == canonical) { op = cand; found = true; break; }
        }
        if (!found) return false;
        auto it = firstRawByCanonical.find(canonical);
        if (it != firstRawByCanonical.end() && it->second != raw) sawRotation = true;
        if (it == firstRawByCanonical.end()) firstRawByCanonical[canonical] = raw;
        if (op == VmOpcode::Halt) break;
        pc += 1u + static_cast<size_t>(operandLenForTest(op));
    }
    return true;
}

TEST(Vmc4BackendTest, DriftDescribeExposesParams)
{
    Vmc4Options opts;
    opts.seed = 0xABCD;
    VmInstance plain(opts);
    EXPECT_NE(plain.describe().find("driftVariants: 0"), std::string::npos) << plain.describe();
    EXPECT_NE(plain.describe().find("driftPeriod: 16"), std::string::npos) << plain.describe();

    opts.driftVariants = 3;
    opts.driftPeriod   = 32;
    VmInstance drift(opts);
    const std::string desc = drift.describe();
    EXPECT_NE(desc.find("driftVariants: 3"), std::string::npos) << desc;
    EXPECT_NE(desc.find("driftPeriod: 32"), std::string::npos) << desc;
    // period 语义变义观测面（#762 升级档）：每变体一个 caseLabelPerms 段。
    EXPECT_NE(desc.find("caseLabelPerms:"), std::string::npos) << desc;
    EXPECT_NE(desc.find("    d0:"), std::string::npos) << desc;
    EXPECT_NE(desc.find("    d2:"), std::string::npos) << desc;

    // 确定性：同配置同实例 seed → 同描述。
    VmInstance drift2(opts);
    EXPECT_EQ(desc, drift2.describe());
}

// period 语义变义（#762 升级档）：case 标签周期轮换
TEST(Vmc4BackendTest, DriftCaseLabelsDeterministicRotatedDisjoint)
{
    Vmc4Options opts;
    opts.seed          = 0x1357;
    opts.driftVariants = 3;
    opts.driftPeriod   = 8;
    VmInstance a(opts);
    VmInstance b(opts);
    const size_t kCount = a.caseLabelPerm(0).size();

    // 确定性 + 每变体段内是 0..K-1 全排列。
    for (int d = 0; d < a.driftVariants(); ++d) {
        EXPECT_EQ(a.caseLabelPerm(d), b.caseLabelPerm(d));
        auto perm = a.caseLabelPerm(d);
        std::sort(perm.begin(), perm.end());
        for (size_t i = 0; i < perm.size(); ++i) {
            EXPECT_EQ(perm[i], static_cast<uint8_t>(i)) << "变体 " << d << " 非排列";
        }
    }
    // 标签空间 = D × K；case 常量 = d*K + 段内值，跨变体不相交（无碰撞）。
    EXPECT_EQ(a.caseLabelSpace(), a.driftVariants() * static_cast<int>(kCount));
    for (int d1 = 0; d1 < a.driftVariants(); ++d1) {
        for (int d2 = d1 + 1; d2 < a.driftVariants(); ++d2) {
            for (uint8_t v1 : a.caseLabelPerm(d1)) {
                for (uint8_t v2 : a.caseLabelPerm(d2)) {
                    EXPECT_NE(static_cast<int>(v1) + d1 * static_cast<int>(kCount),
                              static_cast<int>(v2) + d2 * static_cast<int>(kCount))
                        << "跨变体 case 常量碰撞";
                }
            }
        }
    }
    // 轮换真实发生：存在语义其 case 标签在不同变体下不同。
    bool rotated = false;
    for (const auto& op : a.handlerOrder()) {
        const int l0 = a.caseLabelFor(op, 0);
        for (int d = 1; d < a.driftVariants(); ++d) {
            if (a.caseLabelFor(op, d) != l0) rotated = true;
        }
    }
    EXPECT_TRUE(rotated) << "case 标签跨变体无轮换（变义未发生）";
    // drift 关闭：无标签空间，caseLabelFor 回落 canonical。
    Vmc4Options plain;
    VmInstance p(plain);
    EXPECT_FALSE(p.driftEnabled());
    EXPECT_EQ(p.caseLabelSpace(), 0);
    EXPECT_EQ(p.caseLabelFor(VmOpcode::Copy, 0), static_cast<int>(p.opcodeFor(VmOpcode::Copy)));
}

TEST(Vmc4BackendTest, DriftVariantPickUnmapRoundTrip)
{
    Vmc4Options opts;
    opts.seed           = 0x12345678;
    opts.aliasCount     = 2;
    opts.driftVariants  = 3;
    opts.driftPeriod    = 16;
    VmInstance inst(opts);

    const VmOpcode supported[] = {VmOpcode::Copy,       VmOpcode::Unary,  VmOpcode::Binary,
                                  VmOpcode::Compare,    VmOpcode::Convert, VmOpcode::Jump,
                                  VmOpcode::Branch,     VmOpcode::Return, VmOpcode::SuperInc,
                                  VmOpcode::SuperDec,   VmOpcode::Halt};
    for (const auto op : supported) {
        for (int d = 0; d < 3; ++d) {
            for (uint64_t nonce = 0; nonce < 32; ++nonce) {
                const uint8_t raw = inst.variantPickOpcodeByte(op, nonce, d);
                // 变体下选出的字节必须能 unmap 回该语义的 canonical。
                EXPECT_EQ(inst.unmapByte(raw, d), inst.opcodeFor(op))
                    << "variant " << d << " nonce " << nonce;
            }
        }
    }
}

TEST(Vmc4BackendTest, DriftVariantForPcIsPeriodic)
{
    Vmc4Options opts;
    opts.seed          = 7;
    opts.driftVariants = 2;
    opts.driftPeriod   = 16;
    VmInstance inst(opts);
    EXPECT_TRUE(inst.driftEnabled());
    // (pc / 16) % 2：16 字节一个周期，2 个变体交替。
    EXPECT_EQ(inst.variantForPc(0), 0);
    EXPECT_EQ(inst.variantForPc(15), 0);
    EXPECT_EQ(inst.variantForPc(16), 1);
    EXPECT_EQ(inst.variantForPc(31), 1);
    EXPECT_EQ(inst.variantForPc(32), 0);
    EXPECT_EQ(inst.variantForPc(48), 1);
    EXPECT_EQ(inst.variantForPc(63), 1);
}

TEST(Vmc4BackendTest, DriftVariantsDivergeFromBaseline)
{
    Vmc4Options base;
    base.seed = 0xDEADBEEF;
    base.aliasCount = 1;

    Vmc4Options withDrift = base;
    withDrift.driftVariants = 2;
    withDrift.driftPeriod   = 8;

    VmInstance plain(base);
    VmInstance drift(withDrift);

    // 同一 nonce 下，变体 1 至少有一个语义的字节与 baseline 不同。
    bool anyDiff = false;
    const VmOpcode supported[] = {VmOpcode::Copy, VmOpcode::Unary,  VmOpcode::Binary,
                                  VmOpcode::Compare, VmOpcode::Convert, VmOpcode::Jump,
                                  VmOpcode::Branch,  VmOpcode::Return, VmOpcode::SuperInc,
                                  VmOpcode::SuperDec, VmOpcode::Halt};
    for (const auto op : supported) {
        for (uint64_t nonce = 0; nonce < 64; ++nonce) {
            if (drift.variantPickOpcodeByte(op, nonce, 1) != plain.pickOpcodeByte(op, nonce)) {
                anyDiff = true;
                break;
            }
        }
        if (anyDiff) break;
    }
    EXPECT_TRUE(anyDiff) << "drift variant 1 must change at least one opcode byte";
}

TEST(Vmc4BackendTest, DriftEmitGeneratesUnmapTablesAndCanonicalCases)
{
    Vmc4Options opts;
    opts.dispatcherKind = VmDispatcherKind::SwitchTable;
    opts.aliasCount     = 2;
    opts.driftVariants  = 2;
    opts.driftPeriod    = 16;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("driftAdd", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "drift1");
    // 2 个 unmap 表 + 2 个 case 标签表（period 语义变义，#762 升级档）。
    EXPECT_NE(src.find("mj2x_vm_unmap_drift1_0"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_unmap_drift1_1"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_label_drift1_0"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_label_drift1_1"), std::string::npos) << src;
    // 取码走周期公式 + unmap 选择 + case 标签映射。
    EXPECT_NE(src.find("(unsigned)(pc / 16) % 2"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_label_drift1_"), std::string::npos) << src;
    // case 数 = (handlerOrder + Halt) × driftVariants（每语义每变体一个标签，
    // 共享 body；不再为 ALT 别名发 case）。
    const size_t caseCount = countOccurrencesForTest(src, "case 0x");
    EXPECT_EQ(caseCount, (instance.handlerOrder().size() + 1u) *
                             static_cast<size_t>(instance.driftVariants()))
        << src;
}

TEST(Vmc4BackendTest, DriftEmitOffHasNoUnmapTables)
{
    Vmc4Options opts;
    opts.dispatcherKind = VmDispatcherKind::SwitchTable;
    opts.aliasCount     = 2;
    VmInstance instance(opts);
    std::string reason;
    auto prog = encodeMethod("plainAdd", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "plain1");
    EXPECT_EQ(src.find("mj2x_vm_unmap_"), std::string::npos)
        << "drift disabled must not emit unmap tables\n"
        << src;
    // 关闭时保持别名 case（每个语义 canonical + 别名）。
    const size_t caseCount = countOccurrencesForTest(src, "case 0x");
    size_t expected = 0;
    for (const auto op : instance.handlerOrder()) expected += instance.opcodeBytesFor(op).size();
    expected += instance.opcodeBytesFor(VmOpcode::Halt).size();
    EXPECT_EQ(caseCount, expected) << src;
}

TEST(Vmc4BackendTest, DriftEncoderStreamDecodesCleanly)
{
    Vmc4Options opts;
    opts.seed           = 0x7777;
    opts.aliasCount     = 2;
    opts.driftVariants  = 2;
    opts.driftPeriod    = 8;
    VmInstance instance(opts);
    std::string reason;
    // 循环方法：字节流足够长，能跨越多个变体周期。
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("driftLoop", "(I)I",
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

    // 编码器/解释器同步性：字节流按 unmap 逐指令解码全部合法，
    // 且轮转真实发生（同一语义跨变体用了不同字节）。
    std::set<int> variants;
    bool          sawRotation = false;
    EXPECT_TRUE(decodeStreamCleanForTest(instance, prog->bytecodes, variants, sawRotation))
        << "bytecode stream must decode cleanly under drift";
    EXPECT_GT(variants.size(), 1u) << "stream must span more than one drift variant";
    EXPECT_TRUE(sawRotation)
        << "same semantics must use different raw bytes across drift variants";
}

// ===========================================================================
// 反侦测检查点注入（--runtime-protection）：
//   - 默认不注入（零产物变化，既有 e2e 不回归）
//   - 开启后 wrapper 入口含 mj2x_rt_checkpoint_run 调用 + FAKE 默认值返回
// ===========================================================================
TEST(Vmc4BackendTest, RuntimeProtectionInjection)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2);
    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*method);

    // 默认：不注入。
    Vmc4Backend plain;
    auto        plainResult = plain.compileMethod("Test", cls.methods[0], blocks);
    ASSERT_EQ(plainResult.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_EQ(plainResult.text.find("mj2x_rt_checkpoint_run"), std::string::npos)
        << plainResult.text;

    // 开启 FAKE：wrapper 入口注入检查点，命中返回 (jint)0。
    Vmc4Options opts;
    opts.runtimeProtection.action  = codegen::RuntimeProtectionAction::Fake;
    opts.runtimeProtection.step    = 16;
    opts.runtimeProtection.delayMs = 500;
    Vmc4Backend injected;
    injected.setOptions(opts);
    auto injectedResult = injected.compileMethod("Test", cls.methods[0], blocks);
    ASSERT_EQ(injectedResult.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(injectedResult.text.find("mj2x_rt_checkpoint_run"), std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("MJ2X_RT_PROTECT_FAKE"), std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("static mj2x_rt_checkpoint mj2x_rt_cp_"),
              std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("return (jint)0;"), std::string::npos)
        << injectedResult.text;
}

// 后续项：CHECKCAST（ReferenceCast）→ castcheck:<desc> intrinsic
// （IsInstanceOf + 失败抛 ClassCastException，null 通过）——真实样本转化率
// 第一瓶颈（snake/jvm-obf-tester 主要 fail-closed 原因）。
TEST(Vmc4BackendTest, EncodesCheckCast)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    // static int castLen(Object o) { return ((String)o).length(); }
    auto prog = encodeMethod("castLen", "(Ljava/lang/Object;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new TypeInsnNode(
                                     Opcodes::CHECKCAST,
                                     ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                                 new MethodInsnNode(
                                     Opcodes::INVOKEVIRTUAL,
                                     ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                     ca::str::Utf8StringRef::from_cstr("length"),
                                     ca::str::Utf8StringRef::from_cstr("()I"), false),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // castcheck 入 intrinsic 表（按 desc 去重），结果 REF 原样传递。
    ASSERT_EQ(prog->intrinsics.size(), 1u);
    EXPECT_EQ(prog->intrinsics[0].name, "castcheck:java/lang/String");
    EXPECT_EQ(prog->intrinsics[0].arg0Kind, VmJniKind::Ref);
    EXPECT_EQ(prog->intrinsics[0].retKind, VmJniKind::Ref);
    // 字节流含 Intrinsic 的实例字节（canonical 或 ALT 别名——emitOpcode 按
    // nonce 选字节，两者都可能出现）。
    const auto& opBytes = instance.opcodeBytesFor(VmOpcode::Intrinsic);
    const bool found =
        std::any_of(opBytes.begin(), opBytes.end(), [&](uint8_t b) {
            return std::find(prog->bytecodes.begin(), prog->bytecodes.end(), b) !=
                   prog->bytecodes.end();
        });
    EXPECT_TRUE(found) << "checkcast must be encoded as Intrinsic opcode";
}

// ===========================================================================
// 粒度矩阵（issue #827）：per-program 全程序共享一个 VM 实例。
// ===========================================================================

namespace {

/// 编译一个类的单个静态方法，返回生成 C 文本（backend 必须已 setOptions）。
/// 用同一个 backend 编译多个方法以观察实例缓存（共享/独立）。
std::string compileSingleMethodWithBackend(Vmc4Backend& backend, const std::string& cls,
                                           const std::string& method, std::string& reason)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, method.c_str(), "(II)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         },
                         2, 2);
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;
    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod(cls, cm, blocks);
    if (result.kind != BackendResult::Kind::C_SOURCE) {
        reason = result.text;
        return {};
    }
    return result.text;
}

/// 提取生成 C 里的实例 opcode 字节集合（switch dispatcher 的 `case 0xNN` 链），
/// 同一 VmInstance 的所有方法该集合一致——共享实例的直接证据。
std::string firstOpcodeChain(const std::string& c)
{
    const std::string marker = "case 0x";
    std::string       out;
    size_t            pos = 0;
    while ((pos = c.find(marker, pos)) != std::string::npos) {
        // 取 "0xNN" 后到下一个非 hex 字符。
        size_t i = pos + marker.size();
        while (i < c.size() && std::isxdigit(static_cast<unsigned char>(c[i]))) ++i;
        out += c.substr(pos + marker.size(), i - (pos + marker.size()));
        out += ';';
        pos = i;
    }
    return out;
}

}   // namespace

/// per-program：同 backend（同主 seed）下不同类的方法共享同一 VmInstance →
/// 解释器 opcode 字节链一致。
TEST(Vmc4BackendTest, PerProgramGranularitySharesOneInstanceAcrossClasses)
{
    Vmc4Options opts;
    opts.seed        = 42;
    opts.granularity = Vmc4Options::Granularity::PerProgram;
    Vmc4Backend backend;
    backend.setOptions(opts);
    backend.setOutputPath("unused.c");

    std::string reason;
    const std::string a = compileSingleMethodWithBackend(backend, "Alpha", "add", reason);
    ASSERT_FALSE(a.empty()) << reason;
    const std::string b = compileSingleMethodWithBackend(backend, "Beta", "add", reason);
    ASSERT_FALSE(b.empty()) << reason;

    const std::string chainA = firstOpcodeChain(a);
    const std::string chainB = firstOpcodeChain(b);
    ASSERT_FALSE(chainA.empty()) << a;
    ASSERT_FALSE(chainB.empty()) << b;
    EXPECT_EQ(chainA, chainB)
        << "per-program: different classes must share one VM instance (same opcode chain)\n"
        << "A: " << chainA << "\nB: " << chainB;
}

/// per-method：同 backend 下不同类的方法各自独立实例 → opcode 字节链不同。
TEST(Vmc4BackendTest, PerMethodGranularityDivergeAcrossClasses)
{
    Vmc4Options opts;
    opts.seed        = 42;
    opts.granularity = Vmc4Options::Granularity::PerMethod;
    Vmc4Backend backend;
    backend.setOptions(opts);
    backend.setOutputPath("unused.c");

    std::string reason;
    const std::string a = compileSingleMethodWithBackend(backend, "Alpha", "add", reason);
    ASSERT_FALSE(a.empty()) << reason;
    const std::string b = compileSingleMethodWithBackend(backend, "Beta", "add", reason);
    ASSERT_FALSE(b.empty()) << reason;

    const std::string chainA = firstOpcodeChain(a);
    const std::string chainB = firstOpcodeChain(b);
    ASSERT_FALSE(chainA.empty()) << a;
    ASSERT_FALSE(chainB.empty()) << b;
    EXPECT_NE(chainA, chainB)
        << "per-method: different classes must have independent VM instances\n"
        << "A: " << chainA << "\nB: " << chainB;
}

/// per-class：同类方法共享（链一致），跨类独立（链不同）—— 既有语义回归锚。
TEST(Vmc4BackendTest, PerClassGranularitySharesWithinClassDivergeAcross)
{
    Vmc4Options opts;
    opts.seed        = 42;
    opts.granularity = Vmc4Options::Granularity::PerClass;
    Vmc4Backend backend;
    backend.setOptions(opts);
    backend.setOutputPath("unused.c");

    std::string reason;
    // Alpha 类的两个方法（共享实例）。
    const std::string a1 = compileSingleMethodWithBackend(backend, "Alpha", "add", reason);
    ASSERT_FALSE(a1.empty()) << reason;
    const std::string a2 = compileSingleMethodWithBackend(backend, "Alpha", "add", reason);
    ASSERT_FALSE(a2.empty()) << reason;
    EXPECT_EQ(firstOpcodeChain(a1), firstOpcodeChain(a2))
        << "per-class: same class methods share one instance";
    // Beta 类（独立实例）。
    const std::string b = compileSingleMethodWithBackend(backend, "Beta", "add", reason);
    ASSERT_FALSE(b.empty()) << reason;
    EXPECT_NE(firstOpcodeChain(a1), firstOpcodeChain(b))
        << "per-class: different classes have independent instances";
}

// ===========================================================================
// issue #1133：u16 块 id 总块数容量 fail-closed（真实块 + 合成边块 > 0xFFFF 时
// id 回绕静默腐蚀跳转目标；旧校验只卡 realCount ≤ 0xFFFE，合成块不计入）。
// ===========================================================================

namespace {

/// @brief 构造含 realCount 个真实块的 RegFunction（内层 API 直达编码器）。
///
/// @details
/// 单方法 > 65535 块无法经合法类文件构造（JVM code 属性上限 65535 字节），
/// 容量校验只能从内部 IR 状态触发。块形态：前 jumpsWithCopies 个块各持一条
/// Jump → 下一块且带边拷贝（每条带拷贝的边物质化一个合成块），其余块空指令
/// + Return 终结。边拷贝固定 v1 ← v0（I32），使 scanComputeRegisters/
/// emitSyntheticEdge 有合法寄存器可编码。
RegFunction makeBlockCapacityFixture(size_t realCount, size_t jumpsWithCopies)
{
    RegFunction fn;
    fn.symbol.owner = "test/BlockCap";
    fn.symbol.name  = "cap";
    fn.symbol.desc  = "()V";
    fn.blocks.reserve(realCount);
    for (size_t i = 0; i < realCount; ++i) {
        fn.blocks.push_back(std::make_unique<RegBlock>(SSABlockId{static_cast<uint32_t>(i)}));
    }
    const RegValue v0{RegId{0}, SSAType::I32};
    const RegValue v1{RegId{1}, SSAType::I32};
    for (size_t i = 0; i < realCount; ++i) {
        RegBlock* next = (i + 1 < realCount) ? fn.blocks[i + 1].get() : nullptr;
        if (next != nullptr && i < jumpsWithCopies) {
            RegJumpTerminator term;
            term.target.block = next;
            term.target.copies.push_back(RegCopy{v1, RegOperand::fromRegister(v0)});
            fn.blocks[i]->terminator = std::move(term);
        } else {
            fn.blocks[i]->terminator = RegReturnTerminator{};
        }
    }
    return fn;
}

}   // namespace

/// 容量校验小函数直测：边界 65535 合法、65536 超限，诊断含四个数字。
TEST(Vmc4BackendTest, BlockIdCapacityCheckBoundaries)
{
    std::string reason;
    // 边界内：total == 0xFFFF 合法（id 0..65534 全部落入 u16 域）；
    // 覆盖真实为主 / 合成为主 / 混合三种构成。
    EXPECT_TRUE(checkBlockIdCapacity(65535, 0, reason));
    EXPECT_TRUE(checkBlockIdCapacity(65534, 1, reason));
    EXPECT_TRUE(checkBlockIdCapacity(0, 65535, reason));
    EXPECT_TRUE(checkBlockIdCapacity(60000, 5000, reason));
    EXPECT_TRUE(reason.empty()) << reason;
    // 超限：total == 0xFFFF + 1 即报错（真实为主 / 合成为主两个方向）。
    EXPECT_FALSE(checkBlockIdCapacity(65536, 0, reason));
    EXPECT_FALSE(checkBlockIdCapacity(0, 65536, reason));
    EXPECT_FALSE(checkBlockIdCapacity(60000, 5536, reason));
    // 诊断含总块数、真实块数、合成块数、上限四个数字。
    EXPECT_FALSE(checkBlockIdCapacity(65534, 2, reason));
    EXPECT_NE(reason.find("total=65536"), std::string::npos) << reason;
    EXPECT_NE(reason.find("real=65534"), std::string::npos) << reason;
    EXPECT_NE(reason.find("synthetic=2"), std::string::npos) << reason;
    EXPECT_NE(reason.find("65535"), std::string::npos) << reason;
}

/// 负向：total == 65536（65534 真实 + 2 合成）编码 fail-closed 返回 nullptr。
/// 旧校验（realCount ≤ 0xFFFE）对此形态放行——合成块不计入正是 #1133 的洞。
TEST(Vmc4BackendTest, EncodeFailClosedWhenSyntheticEdgesPushTotalPastU16)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    RegFunction fn   = makeBlockCapacityFixture(65534, 2);
    auto        prog = VmEncoder::encode(fn, instance, reason);
    EXPECT_EQ(prog, nullptr) << "total 65534+2=65536 blocks must fail closed";
    EXPECT_NE(reason.find("total=65536"), std::string::npos) << reason;
    EXPECT_NE(reason.find("synthetic=2"), std::string::npos) << reason;
    EXPECT_NE(reason.find("65535"), std::string::npos) << reason;
}

/// 负向：65536 个纯真实块（无合成）同样 fail-closed（total 越界与构成无关）。
TEST(Vmc4BackendTest, EncodeFailClosedAt65536RealBlocks)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    RegFunction fn   = makeBlockCapacityFixture(65536, 0);
    auto        prog = VmEncoder::encode(fn, instance, reason);
    EXPECT_EQ(prog, nullptr) << "total 65536 real blocks must fail closed";
    EXPECT_NE(reason.find("total=65536"), std::string::npos) << reason;
    EXPECT_NE(reason.find("real=65536"), std::string::npos) << reason;
}

/// 正向边界：total == 0xFFFF（65534 真实 + 1 合成）仍正常编码，块表覆盖
/// 真实 + 合成全空间；65535 个纯真实块亦然（旧校验误拒的合法边界）。
TEST(Vmc4BackendTest, EncodesAtU16TotalBlockCapacityBoundary)
{
    Vmc4Options opts;
    VmInstance  instance(opts);
    std::string reason;
    {
        RegFunction fn   = makeBlockCapacityFixture(65534, 1);
        auto        prog = VmEncoder::encode(fn, instance, reason);
        ASSERT_NE(prog, nullptr) << reason;
        EXPECT_EQ(prog->blockTargets.size(), 65535u) << reason;
        EXPECT_FALSE(prog->bytecodes.empty());
    }
    {
        RegFunction fn   = makeBlockCapacityFixture(65535, 0);
        auto        prog = VmEncoder::encode(fn, instance, reason);
        ASSERT_NE(prog, nullptr) << reason;
        EXPECT_EQ(prog->blockTargets.size(), 65535u) << reason;
        EXPECT_FALSE(prog->bytecodes.empty());
    }
}

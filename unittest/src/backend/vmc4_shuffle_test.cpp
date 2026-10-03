// VMC4 编码期块乱序单测（issue #1116 切片 A，--vmc4-shuffle-blocks）。
//
// 本测试不调用 JNI/DLL（执行级 round-trip 在 vmc4_backend_e2e_test.cpp），
// 只在 VmProgram / 生成 C 源层面验证：
//   1. shuffle 关闭：发射形态与历史一致（const targets 表、无表解密调用）。
//   2. shuffle 开启：块物理布局 ≠ 发射序（Sattolo 单环、恒无不动点），
//      跳转操作数闭合（所有目标 id < 块数）、blockTargets 恒升序（异常检查点
//      区间反查不变量）、入口块 id 0 恒为物理首块。
//   3. 同 seed 两次生成逐字节一致（确定性纪律）；不同 seed 布局不同（统计性）。
//   4. 异常区域随块重映射：条目内容（copies/caughtType）不变、handlerBlock/
//      blockIdx 落在打乱后的 id 空间且与 blockExcRegions 自洽。
//   5. 间接跳转表加密落盘（non-const + JNI_OnLoad 解密调用）+ 与 drift/
//      encrypt-program 叠加的产物形态。

#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/vmc4/vm_encoder.hpp"
#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// B8b：测试侧 Label 统一归宿（进程寿命 scratch arena）。
garena::Arena& shuffleLabelArena()
{
    static garena::Arena arena;
    return arena;
}

/// @brief 构造 MethodNode（name/desc 必须是静态存储期字面量）。
std::unique_ptr<MethodNode> makeShuffleMethod(int access, const char* name, const char* desc,
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

/// 完整编码管线：MethodNode -> VmProgram（与 vmc4_backend_test 同构）。
std::unique_ptr<VmProgram> encodeForShuffle(std::unique_ptr<MethodNode> mn,
                                            const VmInstance& instance, std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmc4Shuffle", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    return VmEncoder::encode(*reg, instance, reason);
}

/// 三路分支 + 循环的方法（>=5 CFG 块、多前驱汇合块会触发合成边块），
/// 指令集限定在 Copy/Unary/Binary/Compare/Jump/Branch/Return（解码器覆盖面）。
std::unique_ptr<MethodNode> makeBranchyLoopMethod()
{
    auto* lElseIf = new LabelNode(shuffleLabelArena().create<Label>());
    auto* lNeg    = new LabelNode(shuffleLabelArena().create<Label>());
    auto* lJoin   = new LabelNode(shuffleLabelArena().create<Label>());
    auto* lExit   = new LabelNode(shuffleLabelArena().create<Label>());
    // static int clip(int x) {
    //   int r;
    //   if (x > 100) r = x - 100;
    //   else if (x < 0) r = -x;
    //   else r = x * 2;
    //   while (r > 50) r -= 50;
    //   return r; }
    return makeShuffleMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "clip", "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new LdcInsnNode(100),
                                 new JumpInsnNode(Opcodes::IF_ICMPLE, lElseIf->label),
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new LdcInsnNode(100),
                                 new InsnNode(Opcodes::ISUB),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 new JumpInsnNode(Opcodes::GOTO, lJoin->label),
                                 lElseIf,
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new JumpInsnNode(Opcodes::IFLT, lNeg->label),
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::ICONST_2),
                                 new InsnNode(Opcodes::IMUL),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 new JumpInsnNode(Opcodes::GOTO, lJoin->label),
                                 lNeg,
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::INEG),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 lJoin,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new LdcInsnNode(50),
                                 new JumpInsnNode(Opcodes::IF_ICMPLE, lExit->label),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new LdcInsnNode(50),
                                 new InsnNode(Opcodes::ISUB),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 new JumpInsnNode(Opcodes::GOTO, lJoin->label),
                                 lExit,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             3, 2);
}

/// shuffle 开启的工具实例（opts.seed 即实例 seed）。
VmInstance shuffledInstance(uint64_t seed)
{
    Vmc4Options opts;
    opts.seed          = seed;
    opts.shuffleBlocks = true;
    return VmInstance(opts);
}

/// shuffle 关闭的工具实例。
VmInstance plainInstance(uint64_t seed)
{
    Vmc4Options opts;
    opts.seed = seed;
    return VmInstance(opts);
}

// ---------------------------------------------------------------------------
// 迷你指令解码器：字节 → 语义 → 块 terminator 与跳转目标。
// 仅覆盖纯计算 + 控制流子集（本文件测试方法不产生其他 family）；
// drift 关闭（字节集合互不相交，buildOpcodeBytes 双射保证唯一匹配）。
// ---------------------------------------------------------------------------

/// 统一变长指令长度（含 opcode 字节；与 vm_encoder.cpp 的 emit 序列一致）。
size_t vmInstrLength(VmOpcode op)
{
    switch (op) {
    case VmOpcode::Copy: return 6;      // op + type + dest + src
    case VmOpcode::Unary: return 7;     // op + opk + type + dest + src
    case VmOpcode::Binary: return 9;    // op + binop + type + dest + lhs + rhs
    case VmOpcode::Compare: return 9;
    case VmOpcode::Convert: return 7;
    case VmOpcode::Jump: return 3;      // op + target
    case VmOpcode::Branch: return 7;    // op + cond + true + false
    case VmOpcode::Return: return 4;    // op + has_value + src
    case VmOpcode::SuperInc: return 6;
    case VmOpcode::SuperDec: return 6;
    default: return 0;                  // 解码器不认识的 family
    }
}

struct DecodedTerminator
{
    VmOpcode                op = VmOpcode::Halt;
    std::vector<uint16_t>   targets;   ///< Jump 1 个 / Branch 2 个 / Return 0 个
};

/// @brief 从块起点走指令流到块尾，解码 terminator。
///
/// @details
/// [start, end) 是块字节区间（blockTargets 相邻差分）。非 terminator 指令按
/// 长度推进且不得越过块尾；terminator 必须恰好落在块尾（VM 块以显式 terminator
/// 闭合，无 fall-through）。失败返回 false（字节无法解释 = 闭合性被破坏）。
bool decodeBlockTerminator(const VmProgram& prog, const VmInstance& inst, uint32_t start,
                           uint32_t end, DecodedTerminator& out)
{
    static const std::vector<VmOpcode> kKnown = {
        VmOpcode::Copy,   VmOpcode::Unary,   VmOpcode::Binary, VmOpcode::Compare,
        VmOpcode::Convert, VmOpcode::Jump,  VmOpcode::Branch, VmOpcode::Return,
        VmOpcode::SuperInc, VmOpcode::SuperDec,
    };
    std::map<uint8_t, VmOpcode> byteToOp;
    for (VmOpcode op : kKnown) {
        for (uint8_t b : inst.opcodeBytesFor(op)) byteToOp[b] = op;
    }
    auto readU16 = [&](uint32_t at) -> uint16_t {
        return static_cast<uint16_t>(prog.bytecodes[at] |
                                     (static_cast<unsigned>(prog.bytecodes[at + 1]) << 8));
    };
    uint32_t pc = start;
    while (pc < end) {
        const auto it = byteToOp.find(prog.bytecodes[pc]);
        if (it == byteToOp.end()) return false;
        const VmOpcode op   = it->second;
        const size_t   len  = vmInstrLength(op);
        if (len == 0) return false;
        if (op == VmOpcode::Jump || op == VmOpcode::Branch || op == VmOpcode::Return) {
            if (pc + len != end) return false;   // terminator 必须收口块尾
            if (op == VmOpcode::Jump) {
                out.targets = {readU16(pc + 1)};
            } else if (op == VmOpcode::Branch) {
                out.targets = {readU16(pc + 3), readU16(pc + 5)};
            } else {
                out.targets.clear();
            }
            out.op = op;
            return true;
        }
        if (pc + len > end) return false;
        pc += len;
    }
    return false;   // 走到块尾都没遇到 terminator
}

/// 解码全部块的 terminator（按 id 序）；任一块解码失败返回 false。
/// 末块区间扣除末尾 Halt 哨兵（1 字节）。
bool decodeAllTerminators(const VmProgram& prog, const VmInstance& inst,
                          std::vector<DecodedTerminator>& out)
{
    out.clear();
    if (prog.blockTargets.empty() || prog.bytecodes.empty()) return false;
    const size_t n = prog.blockTargets.size();
    out.resize(n);
    for (size_t id = 0; id < n; ++id) {
        const uint32_t start = prog.blockTargets[id];
        const uint32_t end = (id + 1 < n) ? prog.blockTargets[id + 1]
                                          : static_cast<uint32_t>(prog.bytecodes.size() - 1);
        if (!decodeBlockTerminator(prog, inst, start, end, out[id])) return false;
    }
    return true;
}

/// 块字节大小序列（按 id 序；末块扣除 Halt 哨兵 1 字节）。
std::vector<uint32_t> blockSizes(const VmProgram& prog)
{
    std::vector<uint32_t> sizes;
    if (prog.blockTargets.empty() || prog.bytecodes.empty()) return sizes;
    const size_t n = prog.blockTargets.size();
    sizes.reserve(n);
    for (size_t id = 0; id < n; ++id) {
        const uint32_t end = (id + 1 < n) ? prog.blockTargets[id + 1]
                                          : static_cast<uint32_t>(prog.bytecodes.size() - 1);
        sizes.push_back(end - prog.blockTargets[id]);
    }
    return sizes;
}

/// 子串出现次数（断言 JNI_OnLoad 解密调用行数用）。
size_t countNeedles(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

}   // namespace

// ===========================================================================
// shuffle 关闭：历史形态不变（fail-closed 默认）。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffleOffKeepsHistoricalEmission)
{
    VmInstance  instance = plainInstance(0x12345678ull);
    std::string reason;
    auto        prog = encodeForShuffle(makeBranchyLoopMethod(), instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // blockTargets 升序（物理序 == id 序 == 发射序）；入口 id 0 恒 offset 0。
    EXPECT_EQ(prog->blockTargets[0], 0u);
    for (size_t i = 1; i < prog->blockTargets.size(); ++i) {
        EXPECT_GT(prog->blockTargets[i], prog->blockTargets[i - 1]) << "id " << i;
    }
    // 生成 C：const 明文 targets 表；无表解密调用（关闭路径产物形态不变）。
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "soff");
    EXPECT_NE(src.find("static const unsigned int mj2x_vm_targets_soff"), std::string::npos);
    EXPECT_EQ(src.find("static unsigned int mj2x_vm_targets_"), std::string::npos);
    EXPECT_EQ(src.find("mj2x_vm_blob_decrypt((unsigned char*)mj2x_vm_targets_"),
              std::string::npos);
    EXPECT_EQ(src.find("shuffled block ids"), std::string::npos);
}

// ===========================================================================
// shuffle 开启：块物理布局 ≠ 发射序（Sattolo 单环，恒无不动点）。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffleOnReordersBlocksAgainstEmissionOrder)
{
    VmInstance  off = plainInstance(0xA5A5A5A5ull);
    VmInstance  on  = shuffledInstance(0xA5A5A5A5ull);
    std::string reason;
    auto        plainProg = encodeForShuffle(makeBranchyLoopMethod(), off, reason);
    ASSERT_NE(plainProg, nullptr) << reason;
    auto shufProg = encodeForShuffle(makeBranchyLoopMethod(), on, reason);
    ASSERT_NE(shufProg, nullptr) << reason;

    ASSERT_EQ(plainProg->blockTargets.size(), shufProg->blockTargets.size());
    ASSERT_GE(shufProg->blockTargets.size(), 5u)
        << "branchy+loop method must produce enough blocks";

    // 块字节大小多重集相等（同一批块、只重排）；序列本身不同（足够块数 +
    // Sattolo 无不动点，固定 seed 下确定性成立）。
    auto plainSizes = blockSizes(*plainProg);
    auto shufSizes  = blockSizes(*shufProg);
    std::sort(plainSizes.begin(), plainSizes.end());
    std::sort(shufSizes.begin(), shufSizes.end());
    EXPECT_EQ(plainSizes, shufSizes) << "same blocks must be re-laid-out, not rewritten";
    EXPECT_NE(plainSizes, blockSizes(*shufProg))
        << "Sattolo must move non-entry blocks (no fixed points)";

    // 指令字节流整体不同（不同指令顺序 → 不同 nonce → 不同别名选取）。
    EXPECT_NE(plainProg->bytecodes, shufProg->bytecodes);
}

// ===========================================================================
// 跳转目标闭合 + blockTargets 升序不变量（异常检查点区间反查依赖）。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffledLayoutIsClosedAndAscending)
{
    for (uint64_t seed : {0x1ull, 0xDEADBEEFull, 0x7C4EA531ull}) {
        VmInstance  instance = shuffledInstance(seed);
        std::string reason;
        auto        prog = encodeForShuffle(makeBranchyLoopMethod(), instance, reason);
        ASSERT_NE(prog, nullptr) << reason;
        const size_t n = prog->blockTargets.size();

        // 升序 + 入口物理首块（id 0 恒 offset 0）。
        EXPECT_EQ(prog->blockTargets[0], 0u) << "seed " << seed;
        for (size_t i = 1; i < n; ++i) {
            EXPECT_GT(prog->blockTargets[i], prog->blockTargets[i - 1]) << "seed " << seed;
        }
        // 全部块起点都是指令起点（instructionOffsets 与新布局同步）。
        for (uint32_t t : prog->blockTargets) {
            EXPECT_NE(std::find(prog->instructionOffsets.begin(),
                                prog->instructionOffsets.end(), t),
                      prog->instructionOffsets.end())
                << "block start must be an instruction start (seed " << seed << ")";
        }
        // 跳转闭合：每个块 terminator 的目标 id 都 < 块数；Branch 双目标都闭合。
        std::vector<DecodedTerminator> terms;
        ASSERT_TRUE(decodeAllTerminators(*prog, instance, terms)) << "seed " << seed;
        for (size_t id = 0; id < n; ++id) {
            for (uint16_t t : terms[id].targets) {
                EXPECT_LT(t, n) << "dangling jump target (seed " << seed << ", id " << id << ")";
            }
        }
        // 入口块 terminator 存在（入口块也参与闭合）。
        EXPECT_NE(terms[0].op, VmOpcode::Halt);
    }
}

// ===========================================================================
// 同 seed 两次生成逐字节一致（确定性纪律）。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffleSameSeedByteIdentical)
{
    VmInstance  a = shuffledInstance(0x9E3779B97F4A7C15ull);
    VmInstance  b = shuffledInstance(0x9E3779B97F4A7C15ull);
    std::string reason;
    auto        pa = encodeForShuffle(makeBranchyLoopMethod(), a, reason);
    ASSERT_NE(pa, nullptr) << reason;
    auto pb = encodeForShuffle(makeBranchyLoopMethod(), b, reason);
    ASSERT_NE(pb, nullptr) << reason;
    EXPECT_EQ(pa->bytecodes, pb->bytecodes);
    EXPECT_EQ(pa->blockTargets, pb->blockTargets);
    EXPECT_EQ(pa->instructionOffsets, pb->instructionOffsets);
    EXPECT_EQ(pa->excRegions.size(), pb->excRegions.size());
    // 生成 C 源逐字节一致（同 tag）。
    EXPECT_EQ(VmInterpreterEmitter::emit(*pa, a, "det"),
              VmInterpreterEmitter::emit(*pb, b, "det"));
}

// ===========================================================================
// 不同 seed 布局不同（统计性断言，不逐字节比对）。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffleDifferentSeedsDiverge)
{
    std::string                 reason;
    std::vector<VmProgram*>     progs;
    std::vector<VmInstance>     instances;
    std::vector<std::unique_ptr<VmProgram>> owned;
    for (uint64_t seed : {0x11ull, 0x22ull, 0x33ull, 0x44ull, 0x55ull, 0x66ull, 0x77ull,
                          0x88ull}) {
        instances.push_back(shuffledInstance(seed));
        auto prog = encodeForShuffle(makeBranchyLoopMethod(), instances.back(), reason);
        ASSERT_NE(prog, nullptr) << reason;
        owned.push_back(std::move(prog));
    }
    // 与 seed[0] 的 blockTargets 相同的实例数：Sattolo 排列空间 (m-1)!，
    // 8 块方法碰撞概率极低；允许至多 2 个碰撞（统计余量），其余必须互异。
    int sameAsFirst = 0;
    for (size_t i = 1; i < owned.size(); ++i) {
        if (owned[i]->blockTargets == owned[0]->blockTargets) ++sameAsFirst;
    }
    EXPECT_LE(sameAsFirst, 2) << "different seeds must produce different layouts";
    // 两两比较至少存在一对互异（防全同退化）。
    bool anyDifferent = false;
    for (size_t i = 1; i < owned.size(); ++i) {
        if (owned[i]->blockTargets != owned[0]->blockTargets) anyDifferent = true;
    }
    EXPECT_TRUE(anyDifferent);
}

// ===========================================================================
// 异常区域随块重映射：条目内容不变、索引自洽、落在打乱后的 id 空间。
// ===========================================================================
TEST(Vmc4ShuffleTest, ExceptionRegionsRemappedConsistently)
{
    auto makeTryCatch = [] {
        auto* start   = new LabelNode(shuffleLabelArena().create<Label>());
        auto* end     = new LabelNode(shuffleLabelArena().create<Label>());
        auto* handler = new LabelNode(shuffleLabelArena().create<Label>());
        // static int divCatch(int a, int b) {
        //   try { return a / b; } catch (ArithmeticException e) { return 1; } }
        auto mn = makeShuffleMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "divCatch",
                                    "(II)I",
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
        mn->tryCatchBlocks.push_back(mn->create<TryCatchBlockNode>(
            start->label, end->label, handler->label,
            ca::str::Utf8StringRef::from_cstr("java/lang/ArithmeticException")));
        return mn;
    };

    VmInstance  off = plainInstance(0x42ull);
    VmInstance  on  = shuffledInstance(0x42ull);
    std::string reason;
    auto plainProg = encodeForShuffle(makeTryCatch(), off, reason);
    ASSERT_NE(plainProg, nullptr) << reason;
    auto shufProg = encodeForShuffle(makeTryCatch(), on, reason);
    ASSERT_NE(shufProg, nullptr) << reason;

    const size_t n = shufProg->blockTargets.size();
    // 区域条目数不变；catch 类型表不变。
    ASSERT_FALSE(shufProg->excRegions.empty());
    ASSERT_EQ(shufProg->excRegions.size(), plainProg->excRegions.size());
    EXPECT_EQ(shufProg->catchTypes, plainProg->catchTypes);
    // 条目内容（与块索引无关的部分）逐字段不变；索引部分落在合法 id 空间。
    for (size_t i = 0; i < shufProg->excRegions.size(); ++i) {
        const auto& s = shufProg->excRegions[i];
        const auto& p = plainProg->excRegions[i];
        EXPECT_EQ(s.caughtTypeIdx, p.caughtTypeIdx);
        EXPECT_EQ(s.exceptionReg, p.exceptionReg);
        EXPECT_EQ(s.copies, p.copies) << "edge copies are register-level, layout independent";
        EXPECT_LT(s.handlerBlock, n) << "handler id must land in the shuffled id space";
        EXPECT_LT(s.blockIdx, n);
    }
    // blockExcRegions 自洽：每条活跃条目反查 blockIdx 一致；合成块（若有）
    // 无活跃条目。
    ASSERT_EQ(shufProg->blockExcRegions.size(), n);
    for (size_t b = 0; b < n; ++b) {
        for (uint16_t entryIdx : shufProg->blockExcRegions[b]) {
            ASSERT_LT(entryIdx, shufProg->excRegions.size());
            EXPECT_EQ(shufProg->excRegions[entryIdx].blockIdx, b)
                << "blockExcRegions must index the shuffled id space";
        }
    }
    // handler 块本身是可解码块（乱序后仍是良构块）。
    std::vector<DecodedTerminator> terms;
    ASSERT_TRUE(decodeAllTerminators(*shufProg, on, terms));
    // 生成 C：异常检查点存在（块反查逻辑对乱序布局透明——targets 恒升序）。
    const std::string src = VmInterpreterEmitter::emit(*shufProg, on, "shufx");
    EXPECT_NE(src.find("mj2x_vm_exception_matches"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_exc:"), std::string::npos) << src;
}

// ===========================================================================
// 语义保真（单测级模拟执行）：乱序程序按解释器语义逐指令模拟，输出与参考
// 实现一致。shuffle 的跳转目标全部经 blockIndex/blockTargets 解析——本测试
// 守住「语义不变」不变量（纯结构性断言抓不住跳转指错块，曾由 e2e 死循环
// 实证，见 resolveEdgeTarget 的 id 缓存缺陷修复）。
// ===========================================================================
namespace {

/// 迷你模拟器：int 寄存器文件 + 纯计算/控制流子集（与解码器同一覆盖面）。
/// 失败（未知字节/失控/越界）返回 false。
bool simulateProgram(const VmProgram& prog, const VmInstance& inst,
                     const std::vector<long long>& args, long long& outValue)
{
    static const std::vector<VmOpcode> kKnown = {
        VmOpcode::Copy,     VmOpcode::Unary,  VmOpcode::Binary, VmOpcode::Compare,
        VmOpcode::Convert,  VmOpcode::Jump,   VmOpcode::Branch, VmOpcode::Return,
        VmOpcode::SuperInc, VmOpcode::SuperDec,
    };
    std::map<uint8_t, VmOpcode> byteToOp;
    for (VmOpcode op : kKnown) {
        for (uint8_t b : inst.opcodeBytesFor(op)) byteToOp[b] = op;
    }
    auto u16 = [&](uint32_t p) {
        return static_cast<uint16_t>(prog.bytecodes[p] |
                                     (static_cast<unsigned>(prog.bytecodes[p + 1]) << 8));
    };
    std::vector<long long> regs(prog.numRegs ? prog.numRegs : 1, 0);
    for (size_t i = 0; i < args.size() && i < prog.numInputs; ++i) regs[i] = args[i];
    // 常量槽预加载（prologue 等价；本方法只含整型常量）。
    for (const auto& c : prog.constantPool) {
        if (c.destSlot >= regs.size()) return false;
        regs[c.destSlot] = c.intBits;
    }
    if (prog.blockTargets.empty()) return false;
    uint32_t pc = prog.blockTargets[0];
    for (long long steps = 0; steps < 1000000; ++steps) {
        if (pc >= prog.bytecodes.size()) return false;
        const auto it = byteToOp.find(prog.bytecodes[pc]);
        if (it == byteToOp.end()) return false;   // Halt 或未知字节：终止模拟
        const VmOpcode op = it->second;
        switch (op) {
        case VmOpcode::Copy: {
            const uint16_t d = u16(pc + 2), s = u16(pc + 4);
            regs[d] = regs[s];
            break;
        }
        case VmOpcode::Unary: {
            const long long v = regs[u16(pc + 5)];
            regs[u16(pc + 3)] = (prog.bytecodes[pc + 1] == 0) ? -v : ~v;
            break;
        }
        case VmOpcode::Binary: {
            const uint8_t binop = prog.bytecodes[pc + 1];
            const long long l = regs[u16(pc + 5)], r = regs[u16(pc + 7)];
            long long v = 0;
            switch (binop) {
            case 0: v = l + r; break;
            case 1: v = l - r; break;
            case 2: v = l * r; break;
            default: return false;   // 除法等不在覆盖面
            }
            regs[u16(pc + 3)] = v;
            break;
        }
        case VmOpcode::Compare: {
            const uint8_t pred = prog.bytecodes[pc + 1];
            const long long l = regs[u16(pc + 5)], r = regs[u16(pc + 7)];
            bool res = false;
            switch (pred) {
            case 0: res = l == r; break;
            case 1: res = l != r; break;
            case 2: res = l < r; break;
            case 3: res = l <= r; break;
            case 4: res = l > r; break;
            case 5: res = l >= r; break;
            default: return false;
            }
            regs[u16(pc + 3)] = res ? 1 : 0;
            break;
        }
        case VmOpcode::Jump:
            pc = prog.blockTargets[u16(pc + 1)];
            continue;
        case VmOpcode::Branch:
            // [Branch][cond:2][true:2][false:2]：cond 在 pc+1，true pc+3，false pc+5。
            pc = prog.blockTargets[regs[u16(pc + 1)] != 0 ? u16(pc + 3) : u16(pc + 5)];
            continue;
        case VmOpcode::Return:
            outValue = prog.bytecodes[pc + 1] != 0 ? regs[u16(pc + 2)] : 0;
            return true;
        case VmOpcode::SuperInc:
            regs[u16(pc + 3)] = regs[u16(pc + 5)] + 1;
            break;
        case VmOpcode::SuperDec:
            regs[u16(pc + 3)] = regs[u16(pc + 5)] - 1;
            break;
        default:
            return false;
        }
        pc += vmInstrLength(op);
    }
    return false;
}

}   // namespace

TEST(Vmc4ShuffleTest, ShuffleSimulatedExecutionMatchesReference)
{
    // int clip(int x) 的参考语义（与 makeBranchyLoopMethod 逐句对应）。
    auto reference = [](long long x) {
        long long r;
        if (x > 100) {
            r = x - 100;
        } else if (x < 0) {
            r = -x;
        } else {
            r = x * 2;
        }
        while (r > 50) r -= 50;
        return r;
    };
    const std::vector<long long> inputs = {0, 1, 2, 3, 7, 30, 44, 84, 120, 200, -3, -30, -100, 51};

    VmInstance  off = plainInstance(0xABCDull);
    VmInstance  on  = shuffledInstance(0xABCDull);
    std::string reason;
    auto        plainProg = encodeForShuffle(makeBranchyLoopMethod(), off, reason);
    ASSERT_NE(plainProg, nullptr) << reason;
    auto shufProg = encodeForShuffle(makeBranchyLoopMethod(), on, reason);
    ASSERT_NE(shufProg, nullptr) << reason;

    for (long long x : inputs) {
        long long a = 0, b = 0;
        ASSERT_TRUE(simulateProgram(*plainProg, off, {x}, a)) << "plain x=" << x;
        ASSERT_TRUE(simulateProgram(*shufProg, on, {x}, b)) << "shuffled x=" << x;
        EXPECT_EQ(a, reference(x)) << "plain x=" << x;
        EXPECT_EQ(b, reference(x)) << "shuffled x=" << x;
    }
}

// ===========================================================================
// 间接跳转表加密落盘（shuffle 开启）：non-const 数组 + JNI_OnLoad 解密调用。
// ===========================================================================
TEST(Vmc4ShuffleTest, TargetsTableEncryptedAtRestWhenShuffleOn)
{
    VmInstance  instance = shuffledInstance(0xC0FFEEull);
    std::string reason;
    auto        prog = encodeForShuffle(makeBranchyLoopMethod(), instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "enc");
    // 密文 non-const 数组形态；明文 const 形态不得残留。
    EXPECT_NE(src.find("static unsigned int mj2x_vm_targets_enc[]"), std::string::npos);
    EXPECT_EQ(src.find("static const unsigned int mj2x_vm_targets_enc"), std::string::npos);
    EXPECT_NE(src.find("encrypted at rest (issue #1116 slice A)"), std::string::npos) << src;
}

/// backend 级单方法编译（加法方法），返回生成 C 文本。
std::string compileOneShuffleMethod(Vmc4Backend& backend)
{
    auto mn = makeShuffleMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                                {
                                    new VarInsnNode(Opcodes::ILOAD, 0),
                                    new VarInsnNode(Opcodes::ILOAD, 1),
                                    new InsnNode(Opcodes::IADD),
                                    new InsnNode(Opcodes::IRETURN),
                                },
                                2, 2);
    CompilerMethod cm;
    cm.methodNode = mn.get();
    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("ShufProd", cm, blocks);
    return result.kind == BackendResult::Kind::C_SOURCE ? result.text : std::string{};
}

TEST(Vmc4ShuffleTest, BackendEmitsTargetsDecryptOnlyWhenShuffleOn)
{
    const std::string tag = codegen::hex64(codegen::fnv1a64("ShufProd.add(II)I"));

    // --- 关闭态：const 明文 targets 表、无 targets 解密调用（历史形态不变）。
    Vmc4Options offOpts;
    offOpts.seed = 0x2B;
    Vmc4Backend offBackend;
    offBackend.setOptions(offOpts);
    const std::string offText = compileOneShuffleMethod(offBackend);
    ASSERT_FALSE(offText.empty());
    EXPECT_NE(offText.find("static const unsigned int mj2x_vm_targets_" + tag),
              std::string::npos)
        << offText;
    offBackend.finalize(CompilerIndex{});
    const std::string offSrc = offBackend.generatedSource();
    EXPECT_EQ(offSrc.find("(unsigned char*)mj2x_vm_targets_"), std::string::npos)
        << "shuffle-off product must not reference targets decryption\n" << offSrc;

    // --- 开启态：JNI_OnLoad 发射每方法一条 targets 解密调用（数组名 + 字节数 + 密钥）。
    Vmc4Options onOpts = offOpts;
    onOpts.shuffleBlocks = true;
    Vmc4Backend onBackend;
    onBackend.setOptions(onOpts);
    ASSERT_FALSE(compileOneShuffleMethod(onBackend).empty());
    onBackend.finalize(CompilerIndex{});
    const std::string full = onBackend.generatedSource();
    const std::string needle = "mj2x_vm_blob_decrypt((unsigned char*)mj2x_vm_targets_" + tag;
    EXPECT_EQ(countNeedles(full, needle), 1u) << full;
    const size_t pos = full.find(needle);
    ASSERT_NE(pos, std::string::npos);
    const std::string line = full.substr(pos, full.find('\n', pos) - pos);
    EXPECT_NE(line.find("u, 0x"), std::string::npos) << line;
    // 解密调用置于 JNI_OnLoad 体内（加载期一次性解密，先于任何 native 调用）。
    const size_t onLoad = full.find("JNIEXPORT jint JNICALL JNI_OnLoad");
    ASSERT_NE(onLoad, std::string::npos);
    EXPECT_GT(pos, onLoad) << "targets decrypt calls must live inside JNI_OnLoad";

    // 确定性：同 seed 同输入 → finalize 产物逐字节一致。
    Vmc4Backend again;
    again.setOptions(onOpts);
    ASSERT_FALSE(compileOneShuffleMethod(again).empty());
    again.finalize(CompilerIndex{});
    EXPECT_EQ(again.generatedSource(), full) << "same seed must reproduce the product";
}

// ===========================================================================
// 正交叠加：shuffle + drift + encrypt-program 三开产物形态。
// ===========================================================================
TEST(Vmc4ShuffleTest, ShuffleComposesWithDriftAndEncrypt)
{
    Vmc4Options opts;
    opts.seed               = 0x5EED5EEDull;
    opts.shuffleBlocks      = true;
    opts.driftVariants      = 2;
    opts.driftPeriod        = 16;
    opts.programBlobEncrypt = true;
    VmInstance  instance(opts);
    std::string reason;
    auto        prog = encodeForShuffle(makeBranchyLoopMethod(), instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // drift 语义已烘焙进字节流（unmap 表消费形态），块乱序与 drift 编码同源于
    // 最终布局——指令边界表仍与块起点对齐（懒解密分块前提）。
    for (uint32_t t : prog->blockTargets) {
        EXPECT_NE(std::find(prog->instructionOffsets.begin(),
                            prog->instructionOffsets.end(), t),
                  prog->instructionOffsets.end());
    }
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "all3");
    // 字节码 blob 加密（encrypt-program）+ targets 表加密（shuffle）+ drift
    // unmap 表三者共存于同一产物。
    EXPECT_NE(src.find("static unsigned char mj2x_vm_code_all3[]"), std::string::npos);
    EXPECT_EQ(src.find("static const unsigned char mj2x_vm_code_all3"), std::string::npos);
    EXPECT_NE(src.find("static unsigned int mj2x_vm_targets_all3[]"), std::string::npos);
    EXPECT_NE(src.find("mj2x_vm_unmap_all3_0"), std::string::npos);
    EXPECT_NE(src.find("mj2x_vm_unmap_all3_1"), std::string::npos);
}


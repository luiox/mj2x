// vmx64 流级解释器测试：把 Vmx64Encoder 产物（Vmx64Program）按 .asm handler
// 的字段偏移与 pc 推进语义逐位镜像执行——不经 ml64/JNI/DLL，跨平台跑。
//
// 动机（CI 实证）：emit 结构与直算方法全通，但带循环方法首次真执行即死循环
//（windows 作业静默 88 分钟无输出）——asm 层盲审自洽，需要仪器把"编码器数据
// vs handler 语义"的契约错位暴露成可读轨迹。本测试三层：
//   1. 明文流解释执行（plaintext stream：tEnc 字在 program.stream 中是明文
//      站点号，加密只发生在发射器副本上）——语义/布局/边朝向验证。
//   2. 两种分支朝向都覆盖：javac 形态（if_icmplt body：true 边=循环体前跳、
//      false 边=出口直落）与手工形态（if_icmpge done：true 边=出口、false
//      边=循环体直落）——编码器对两条边的规划不对称，只测一边会漏。
//   3. 发射产物回读：db 行字节逐位核对（明文字段不变、tEnc 密文可解回站点
//      号）、dq 行 handler 名与 addend 与 ks64 单源对账。
//
// 解释器带步数预算：死循环 = 带执行轨迹与站点表转储的快速失败，而不是挂死。

#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "test_helper.hpp"
#include "libca/random/split_mix64.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/backend/vmx64/vmx64_encoder.hpp"
#include "mj2x/backend/vmx64/vmx64_opcode.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

// 测试侧 Label 统一归宿（进程寿命 scratch arena）。
static garena::Arena& labelScratchArena()
{
    static garena::Arena arena;
    return arena;
}

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// ===========================================================================
// 编码管线（与 vmx64_backend_test 同源）：MethodNode -> RegIR -> Vmx64Program。
// ===========================================================================

std::unique_ptr<MethodNode> makeMethod(const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ca::str::Utf8StringRef::from_cstr(name),
                                           ca::str::Utf8StringRef::from_cstr(desc),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

std::unique_ptr<Vmx64Program> encodeMethodNode(std::unique_ptr<MethodNode> mn,
                                               std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = test::convertMethodNode(converter, *mn, arena);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmx64I", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    return Vmx64Encoder::encode(*reg, reason);
}

// ===========================================================================
// 流级解释器：逐位镜像 .asm handler（vmx64_asm_emit.cpp）的字段偏移、宽度
// 语义与 pc 推进。i32 写只落低 4 字节（dword store，高位留陈旧值）。
// ===========================================================================

uint16_t rd16(const std::vector<uint8_t>& s, uint32_t off)
{
    return static_cast<uint16_t>(s[off] | (s[off + 1] << 8));
}

uint32_t rd32(const std::vector<uint8_t>& s, uint32_t off)
{
    return static_cast<uint32_t>(rd16(s, off)) | (static_cast<uint32_t>(rd16(s, off + 2)) << 16);
}

uint64_t rd64(const std::vector<uint8_t>& s, uint32_t off)
{
    return static_cast<uint64_t>(rd32(s, off)) | (static_cast<uint64_t>(rd32(s, off + 4)) << 32);
}

struct InterpOutcome
{
    bool        ok    = false;
    uint64_t    value = 0;
    std::string diag;   // 失败诊断：站点表 + 执行轨迹 + slot 现场（失败即全文转储）
};

/// 站点表全量转储（解释器失败诊断的固定前置段）。
std::string dumpSites(const Vmx64Program& p)
{
    std::string out = "sites:\n";
    for (size_t t = 0; t < p.sites.size(); ++t) {
        char line[128];
        std::snprintf(line, sizeof(line), "  [%2zu] off=%-4u %s\n", t,
                      p.sites[t].streamOffset, vmx64OpHandlerName(p.sites[t].op));
        out += line;
    }
    return out;
}

InterpOutcome interpretStream(const Vmx64Program& p, std::vector<uint64_t> slots,
                              uint64_t stepBudget = 100000)
{
    // 与 asm 的已知差异：此处 slot 零初始化，asm 的池是未初始化栈内存
    //（类型化程序里每个槽先写后读，不可观测）。
    InterpOutcome out;
    const auto    fail = [&](const std::string& what, const std::vector<std::string>& trace,
                             uint32_t pc) {
        out.diag = what + " (pc=" + std::to_string(pc) + ")\n" + dumpSites(p) + "trace (last 48):\n";
        for (const auto& l : trace) out.diag += "  " + l + "\n";
        out.diag += "slots:";
        for (size_t i = 0; i < slots.size(); ++i) {
            char b[48];
            std::snprintf(b, sizeof(b), " [%zu]=%llx", i,
                          static_cast<unsigned long long>(slots[i]));
            out.diag += b;
        }
        out.diag += "\n";
        return out;
    };

    uint32_t                pc    = 0;
    uint64_t                steps = 0;
    std::vector<std::string> trace;
    while (true) {
        if (++steps > stepBudget) {
            trace.push_back("<step budget exhausted: stream does not terminate>");
            return fail("step budget exhausted (possible dispatch loop)", trace, pc);
        }
        if (pc + 1 >= p.stream.size()) return fail("pc out of stream", trace, pc);
        const uint16_t t = rd16(p.stream, pc);
        if (t >= p.sites.size()) return fail("t out of site-table range", trace, pc);
        if (p.sites[t].streamOffset != pc) {
            return fail("site desync: stream tEnc word at pc does not match table offset",
                        trace, pc);
        }
        const auto op = p.sites[t].op;

        const auto slotAt = [&](uint32_t off) -> uint64_t& {
            const uint16_t idx = rd16(p.stream, off);
            return slots[idx];   // 越界由调用前的检查拦截
        };
        // i32 写镜像 asm 的 dword store：只落低 4 字节、高 4 字节保留陈旧值
        //（E2E 崩溃根因恰是"高位残留拼接垃圾指针"——解释器必须暴露而非
        // 掩盖该位级效应；类型化程序里 i64 槽不会被 i32 写触碰，不可观测）。
        const auto w32 = [](uint64_t& slot, uint32_t v) {
            slot = (slot & 0xFFFFFFFF00000000ULL) | v;
        };
        const auto checkSlot = [&](uint32_t off) -> bool {
            const uint16_t idx = rd16(p.stream, off);
            return idx < slots.size();
        };

        switch (op) {
        case Vmx64Op::IConst:
            if (!checkSlot(pc + 2)) return fail("iconst dst out of slots", trace, pc);
            w32(slotAt(pc + 2), rd32(p.stream, pc + 4));
            trace.push_back("iconst");
            pc += 8;
            break;
        case Vmx64Op::LConst:
            if (!checkSlot(pc + 2)) return fail("lconst dst out of slots", trace, pc);
            slotAt(pc + 2) = rd64(p.stream, pc + 4);
            trace.push_back("lconst");
            pc += 12;
            break;
        case Vmx64Op::ILoad:
        case Vmx64Op::IStore:
            if (!checkSlot(pc + 2) || !checkSlot(pc + 4))
                return fail("i32 copy slot out of range", trace, pc);
            w32(slotAt(pc + 2), static_cast<uint32_t>(slotAt(pc + 4)));
            trace.push_back("i32copy");
            pc += 6;
            break;
        case Vmx64Op::LLoad:
        case Vmx64Op::LStore:
            if (!checkSlot(pc + 2) || !checkSlot(pc + 4))
                return fail("i64 copy slot out of range", trace, pc);
            slotAt(pc + 2) = slotAt(pc + 4);
            trace.push_back("i64copy");
            pc += 6;
            break;
        case Vmx64Op::IAdd:
        case Vmx64Op::ISub:
        case Vmx64Op::IMul: {
            if (!checkSlot(pc + 2) || !checkSlot(pc + 4) || !checkSlot(pc + 6))
                return fail("i32 arith slot out of range", trace, pc);
            const uint32_t a = static_cast<uint32_t>(slotAt(pc + 4));
            const uint32_t b = static_cast<uint32_t>(slotAt(pc + 6));
            uint32_t      r  = 0;
            if (op == Vmx64Op::IAdd) r = a + b;
            else if (op == Vmx64Op::ISub) r = a - b;
            else r = a * b;
            w32(slotAt(pc + 2), r);
            trace.push_back("i32arith");
            pc += 8;
            break;
        }
        case Vmx64Op::LAdd:
        case Vmx64Op::LSub:
        case Vmx64Op::LMul: {
            if (!checkSlot(pc + 2) || !checkSlot(pc + 4) || !checkSlot(pc + 6))
                return fail("i64 arith slot out of range", trace, pc);
            const uint64_t a = slotAt(pc + 4);
            const uint64_t b = slotAt(pc + 6);
            uint64_t      r  = 0;
            if (op == Vmx64Op::LAdd) r = a + b;
            else if (op == Vmx64Op::LSub) r = a - b;
            else r = a * b;
            slotAt(pc + 2) = r;
            trace.push_back("i64arith");
            pc += 8;
            break;
        }
        case Vmx64Op::IfIcmpEq:
        case Vmx64Op::IfIcmpNe:
        case Vmx64Op::IfIcmpLt:
        case Vmx64Op::IfIcmpLe:
        case Vmx64Op::IfIcmpGt:
        case Vmx64Op::IfIcmpGe: {
            if (!checkSlot(pc + 2) || !checkSlot(pc + 4))
                return fail("icmp slot out of range", trace, pc);
            const int32_t a = static_cast<int32_t>(static_cast<uint32_t>(slotAt(pc + 2)));
            const int32_t b = static_cast<int32_t>(static_cast<uint32_t>(slotAt(pc + 4)));
            bool          cond = false;
            switch (op) {
            case Vmx64Op::IfIcmpEq: cond = a == b; break;
            case Vmx64Op::IfIcmpNe: cond = a != b; break;
            case Vmx64Op::IfIcmpLt: cond = a < b; break;
            case Vmx64Op::IfIcmpLe: cond = a <= b; break;
            case Vmx64Op::IfIcmpGt: cond = a > b; break;
            case Vmx64Op::IfIcmpGe: cond = a >= b; break;
            default: break;
            }
            const uint32_t next = cond ? rd32(p.stream, pc + 6) : rd32(p.stream, pc + 10);
            char line[160];
            std::snprintf(line, sizeof(line), "icmp a=%d b=%d -> %s off=%u", a, b,
                          cond ? "true" : "false", next);
            trace.push_back(line);
            pc = next;
            break;
        }
        case Vmx64Op::Goto: {
            const uint32_t next = rd32(p.stream, pc + 2);
            char           line[96];
            std::snprintf(line, sizeof(line), "goto off=%u", next);
            trace.push_back(line);
            pc = next;
            break;
        }
        case Vmx64Op::IReturn:
            if (!checkSlot(pc + 2)) return fail("iret slot out of range", trace, pc);
            out.value = static_cast<uint32_t>(slotAt(pc + 2));
            out.ok    = true;
            return out;
        case Vmx64Op::LReturn:
            if (!checkSlot(pc + 2)) return fail("lret slot out of range", trace, pc);
            out.value = slotAt(pc + 2);
            out.ok    = true;
            return out;
        case Vmx64Op::VReturn:
        case Vmx64Op::Halt:
            out.ok    = true;
            out.value = 0;
            return out;
        case Vmx64Op::Count:
            return fail("sentinel opcode in stream", trace, pc);
        }
        if (trace.size() > 48) trace.erase(trace.begin());
    }
}

/// 参数落槽后解释执行。
InterpOutcome interpretWithArgs(const Vmx64Program& p,
                                std::initializer_list<uint64_t> args,
                                uint64_t stepBudget = 100000)
{
    std::vector<uint64_t> slots(p.numSlots, 0);
    size_t                i = 0;
    for (const uint64_t v : args) {
        if (i >= p.args.size()) {
            InterpOutcome bad;
            bad.diag = "arg count exceeds program args";
            return bad;
        }
        slots[p.args[i].slot] = v;
        ++i;
    }
    return interpretStream(p, std::move(slots), stepBudget);
}

// ===========================================================================
// 样本构造：两种分支朝向的 sum 循环 + 直算方法。
// ===========================================================================

/// javac 形态：`int s=0; for(int i=0;i<n;i++) s+=i; return s;`
///（if_icmplt body：true 边=循环体前跳，false 边=出口直落）。
std::unique_ptr<Vmx64Program> encodeSumJavacShape(std::string& reason)
{
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* body = new LabelNode(labelScratchArena().create<Label>());
    return encodeMethodNode(
        makeMethod("sum", "(I)I",
                   {
                       new InsnNode(Opcodes::ICONST_0),
                       new VarInsnNode(Opcodes::ISTORE, 1),
                       new InsnNode(Opcodes::ICONST_0),
                       new VarInsnNode(Opcodes::ISTORE, 2),
                       loop,
                       new VarInsnNode(Opcodes::ILOAD, 2),
                       new VarInsnNode(Opcodes::ILOAD, 0),
                       new JumpInsnNode(Opcodes::IF_ICMPLT, body->label),
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new InsnNode(Opcodes::IRETURN),
                       body,
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new VarInsnNode(Opcodes::ILOAD, 2),
                       new InsnNode(Opcodes::IADD),
                       new VarInsnNode(Opcodes::ISTORE, 1),
                       new IincInsnNode(2, 1),
                       new JumpInsnNode(Opcodes::GOTO, loop->label),
                   },
                   2, 3),
        reason);
}

/// 手工形态：同语义但条件取反（if_icmpge done：true 边=出口，false 边=循环
/// 体直落）——与 vmx64_backend_test 的 EncodesLoopMethodWithFusedBranch 同形。
std::unique_ptr<Vmx64Program> encodeSumInvertedShape(std::string& reason)
{
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    return encodeMethodNode(
        makeMethod("sum", "(I)I",
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
                   2, 3),
        reason);
}

}   // namespace

// ===========================================================================
// 1. javac 形态循环：流级执行与 JVM 语义对拍（含 0/1/边界 n 与步数上限）。
// ===========================================================================
TEST(Vmx64StreamInterpTest, JavacShapeLoopTerminatesWithCorrectSum)
{
    std::string reason;
    auto        prog = encodeSumJavacShape(reason);
    ASSERT_NE(prog, nullptr) << reason;

    const struct
    {
        uint64_t n;
        uint64_t expect;
    } cases[] = {{0, 0}, {1, 0}, {2, 1}, {5, 10}, {100, 4950}};
    for (const auto& c : cases) {
        const auto r = interpretWithArgs(*prog, {c.n});
        EXPECT_TRUE(r.ok) << "n=" << c.n << " " << r.diag;
        if (r.ok) EXPECT_EQ(r.value, c.expect) << "n=" << c.n << " " << r.diag;
    }
}

// ===========================================================================
// 2. 手工反相形态循环：同一编码器另一条边朝向的执行对拍。
// ===========================================================================
TEST(Vmx64StreamInterpTest, InvertedShapeLoopTerminatesWithCorrectSum)
{
    std::string reason;
    auto        prog = encodeSumInvertedShape(reason);
    ASSERT_NE(prog, nullptr) << reason;

    const struct
    {
        uint64_t n;
        uint64_t expect;
    } cases[] = {{0, 0}, {1, 0}, {5, 10}, {100, 4950}};
    for (const auto& c : cases) {
        const auto r = interpretWithArgs(*prog, {c.n});
        EXPECT_TRUE(r.ok) << "n=" << c.n << " " << r.diag;
        if (r.ok) EXPECT_EQ(r.value, c.expect) << "n=" << c.n << " " << r.diag;
    }
}

// ===========================================================================
// 3. 直算方法（i32/i64）：与 E2E Driver 相同输入对拍。
// ===========================================================================
TEST(Vmx64StreamInterpTest, StraightLineArithmeticMatchesJvm)
{
    std::string reason;
    {
        auto prog = encodeMethodNode(
            makeMethod("add", "(II)I",
                       {
                           new VarInsnNode(Opcodes::ILOAD, 0),
                           new VarInsnNode(Opcodes::ILOAD, 1),
                           new InsnNode(Opcodes::IADD),
                           new InsnNode(Opcodes::IRETURN),
                       },
                       2, 2),
            reason);
        ASSERT_NE(prog, nullptr) << reason;
        const auto r = interpretWithArgs(*prog, {19u, 23u});
        ASSERT_TRUE(r.ok) << r.diag;
        EXPECT_EQ(r.value, 42u);
        const auto rn = interpretWithArgs(*prog, {~7u + 1u, 7u});
        ASSERT_TRUE(rn.ok) << rn.diag;
        EXPECT_EQ(rn.value, 0u);
    }
    {
        auto prog = encodeMethodNode(
            makeMethod("lmul", "(JJ)J",
                       {
                           new VarInsnNode(Opcodes::LLOAD, 0),
                           new VarInsnNode(Opcodes::LLOAD, 2),
                           new InsnNode(Opcodes::LMUL),
                           new InsnNode(Opcodes::LRETURN),
                       },
                       4, 4),
            reason);
        ASSERT_NE(prog, nullptr) << reason;
        const auto r = interpretWithArgs(*prog, {123456789ULL, 987654321ULL});
        ASSERT_TRUE(r.ok) << r.diag;
        EXPECT_EQ(r.value, 123456789ULL * 987654321ULL);
    }
}

// ===========================================================================
// 4. 帧布局不变量：vreg 池与帧头必须整体位于被保存寄存器区（7 个非易失
//    寄存器，0x38 字节）之下。历史缺陷（E2E 实证）：按 rbp-0x20 起算时
//    slot 9/10/11 叠在 saved r15/r14/r13 上，epilogue pop 出「高位残留 +
//    slot 值」拼接的垃圾寄存器，JVM native 存根解引用即崩（sum 循环必踩，
//    直算方法水位低而侥幸存活）。以文本级断言锁死偏移关系。
// ===========================================================================
TEST(Vmx64StreamInterpTest, FrameLayoutKeepsVregsBelowSavedRegisters)
{
    std::string reason;
    auto        prog = encodeSumJavacShape(reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string asmText = Vmx64AsmEmitter::emitMethod(*prog, 0xABCD, "frm");
    ASSERT_EQ(asmText.find(".err"), std::string::npos) << asmText;

    const auto hexAfter = [&](const std::string& prefix) -> uint32_t {
        const auto pos = asmText.find(prefix);
        EXPECT_NE(pos, std::string::npos) << asmText;
        if (pos == std::string::npos) return 0;
        const auto end = asmText.find('h', pos);
        EXPECT_NE(end, std::string::npos) << asmText;
        return static_cast<uint32_t>(
            std::stoul(asmText.substr(pos + prefix.size(), end - pos - prefix.size()),
                       nullptr, 16));
    };

    const uint32_t subRsp    = hexAfter("    sub rsp, ");
    const uint32_t leaRsi    = hexAfter("    lea rsi, [rbp-");
    const uint32_t envDisp   = hexAfter("    mov [rbp-");
    constexpr uint32_t kSavedRegs = 7u * 8u;   // rbx,rsi,rdi,r12,r13,r14,r15

    // rsi 基址 = sub rsp 量 + 保存区（vreg 池贴着 rsp 之下、保存区之上）。
    EXPECT_EQ(leaRsi, subRsp + kSavedRegs) << asmText;
    // 帧头紧跟保存区之下：env@0x38+8、this@0x38+0x10。
    EXPECT_EQ(envDisp, kSavedRegs + 8) << asmText;
    EXPECT_NE(asmText.find("    mov [rbp-48h], rdx"), std::string::npos) << asmText;
    // vreg 池完整落在 [rbp-0x58, rbp-0x38) 内：不小于 slot 总量。
    EXPECT_GE(leaRsi - kSavedRegs - 0x20, prog->numSlots * 8u) << asmText;
    // 旧缺陷形态（帧头砸在保存区上）必须绝迹。
    EXPECT_EQ(asmText.find("    mov [rbp-8], rcx"), std::string::npos) << asmText;
    EXPECT_EQ(asmText.find("    mov [rbp-16], rdx"), std::string::npos) << asmText;
}

// ===========================================================================
// 5. 发射产物回读：db 行字节与明文流逐位对账（操作数字段不变、tEnc 密文可
//    用 ks64 解回站点号）、dq 行 handler 名与 addend 与 ks64 单源对账。
//    （发射器的 K1 派生在此按同源公式重演——测的是发射器落盘契约本身。）
// ===========================================================================
TEST(Vmx64StreamInterpTest, EmittedAsmCarriesFaithfulStreamAndTable)
{
    std::string reason;
    auto        prog = encodeSumJavacShape(reason);
    ASSERT_NE(prog, nullptr) << reason;

    const uint64_t seed = 0x5EED;
    const uint64_t k1   = ca::random::SplitMix64(seed ^ 0x766D783634ULL).next();
    const std::string asmText = Vmx64AsmEmitter::emitMethod(*prog, seed, "tag0");
    ASSERT_EQ(asmText.find(".err"), std::string::npos) << asmText;

    // --- 解析 db 行（mj2x_vmx64_code_tag0 label byte 之后到 align 8 之前） ---
    std::vector<uint8_t> emitted;
    {
        const auto codePos = asmText.find("mj2x_vmx64_code_tag0 label byte");
        ASSERT_NE(codePos, std::string::npos) << asmText;
        const auto endPos = asmText.find("align 8", codePos);
        ASSERT_NE(endPos, std::string::npos);
        size_t pos = codePos;
        while (true) {
            const auto dbPos = asmText.find("    db ", pos);
            if (dbPos == std::string::npos || dbPos >= endPos) break;
            const auto lineEnd = asmText.find('\n', dbPos);
            ASSERT_NE(lineEnd, std::string::npos);
            std::string line = asmText.substr(dbPos + 7, lineEnd - dbPos - 7);
            size_t t = 0;
            while (t < line.size()) {
                const auto comma = line.find(',', t);
                const std::string tok = line.substr(t, comma == std::string::npos
                                                             ? std::string::npos
                                                             : comma - t);
                ASSERT_EQ(tok.size(), 4u) << "db tokens must be %03xh (3 hex digits + h): " << tok;
                ASSERT_EQ(tok.substr(3), "h") << tok;
                emitted.push_back(static_cast<uint8_t>(std::stoul(tok.substr(0, 3), nullptr, 16)));
                if (comma == std::string::npos) break;
                t = comma + 1;
            }
            pos = lineEnd;
        }
    }
    ASSERT_EQ(emitted.size(), prog->stream.size())
        << "emitted db byte count must equal plaintext stream size";

    // 逐字节对账：非 tEnc 字节必须与明文流相同；tEnc 字解密后必须还原站点号。
    for (size_t t = 0; t < prog->sites.size(); ++t) {
        const uint32_t off = prog->sites[t].streamOffset;
        // tEnc 密文字 = 明文站点号 ^ (u16)ks64(off)。
        const uint16_t ks     = static_cast<uint16_t>(vmx64Ks64(off, k1) & 0xFFFFULL);
        const uint16_t expect = static_cast<uint16_t>(t) ^ ks;
        EXPECT_EQ(emitted[off], expect & 0xFF) << "tEnc lo byte mismatch at site " << t;
        EXPECT_EQ(emitted[off + 1], (expect >> 8) & 0xFF) << "tEnc hi byte mismatch at site " << t;
    }
    for (size_t i = 0; i < prog->stream.size(); ++i) {
        bool isTEnc = false;
        for (const auto& site : prog->sites)
            if (site.streamOffset == i || site.streamOffset + 1 == i) isTEnc = true;
        if (isTEnc) continue;
        EXPECT_EQ(emitted[i], prog->stream[i])
            << "operand byte " << i << " must be identical to plaintext stream";
    }

    // --- dq 行：条数/顺序 handler 名/addend 与 ks64(t) 对账。 ---
    {
        const auto tablePos = asmText.find("mj2x_vmx64_sites_tag0 label qword");
        ASSERT_NE(tablePos, std::string::npos) << asmText;
        size_t pos = tablePos;
        for (size_t t = 0; t < prog->sites.size(); ++t) {
            const auto dqPos = asmText.find("    dq mj2x_vmx64_h_", pos);
            ASSERT_NE(dqPos, std::string::npos) << "missing dq entry " << t;
            const auto lineEnd = asmText.find('\n', dqPos);
            const std::string line = asmText.substr(dqPos + 4 + 3, lineEnd - dqPos - 7);
            const std::string expectHandler =
                std::string("mj2x_vmx64_h_") + vmx64OpHandlerName(prog->sites[t].op);
            const auto minus = line.rfind('-');
            ASSERT_NE(minus, std::string::npos) << line;
            EXPECT_EQ(line.substr(0, minus), expectHandler)
                << "dq entry " << t << " handler mismatch: " << line;
            const std::string addendHex = line.substr(minus + 1);
            ASSERT_EQ(addendHex.back(), 'h') << addendHex;
            const uint64_t addend = std::stoull(addendHex.substr(0, addendHex.size() - 1),
                                                nullptr, 16);
            EXPECT_EQ(addend, vmx64Ks64(t, k1)) << "dq entry " << t << " addend mismatch";
            pos = lineEnd;
        }
    }
}

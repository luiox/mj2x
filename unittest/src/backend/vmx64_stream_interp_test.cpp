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
#include "mj2x/backend/vmx64/vmx64_le_bytes.hpp"
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
// LE 读原语用生产侧单源（mj2x::leRead16/32/64，vmx64_le_bytes.hpp）。
// ===========================================================================

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
        const uint16_t t = leRead16(p.stream, pc);
        if (t >= p.sites.size()) return fail("t out of site-table range", trace, pc);
        if (p.sites[t].streamOffset != pc) {
            return fail("site desync: stream tEnc word at pc does not match table offset",
                        trace, pc);
        }
        const auto op = p.sites[t].op;
        // 操作数区间越界防御（review 遗留）：此前只查 tEnc 字在流内，操作数
        // 字段（pc+2..pc+13）无界读依赖站点对齐检查间接兜住——对编码器产物
        // 安全，对手工构造的坏流是 UB。按 opcode 操作数宽度一次性封顶。
        if (pc + 2u + vmx64OpOperandSize(op) > p.stream.size())
            return fail("operand span runs past end of stream", trace, pc);

        const auto slotAt = [&](uint32_t off) -> uint64_t& {
            const uint16_t idx = leRead16(p.stream, off);
            return slots[idx];   // 越界由调用前的检查拦截
        };
        // i32 写镜像 asm 的 dword store：只落低 4 字节、高 4 字节保留陈旧值
        //（E2E 崩溃根因恰是"高位残留拼接垃圾指针"——解释器必须暴露而非
        // 掩盖该位级效应；类型化程序里 i64 槽不会被 i32 写触碰，不可观测）。
        const auto w32 = [](uint64_t& slot, uint32_t v) {
            slot = (slot & 0xFFFFFFFF00000000ULL) | v;
        };
        const auto checkSlot = [&](uint32_t off) -> bool {
            const uint16_t idx = leRead16(p.stream, off);
            return idx < slots.size();
        };

        switch (op) {
        case Vmx64Op::IConst:
            if (!checkSlot(pc + 2)) return fail("iconst dst out of slots", trace, pc);
            w32(slotAt(pc + 2), leRead32(p.stream, pc + 4));
            trace.push_back("iconst");
            pc += 8;
            break;
        case Vmx64Op::LConst:
            if (!checkSlot(pc + 2)) return fail("lconst dst out of slots", trace, pc);
            slotAt(pc + 2) = leRead64(p.stream, pc + 4);
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
            const uint32_t next = cond ? leRead32(p.stream, pc + 6) : leRead32(p.stream, pc + 10);
            char line[160];
            std::snprintf(line, sizeof(line), "icmp a=%d b=%d -> %s off=%u", a, b,
                          cond ? "true" : "false", next);
            trace.push_back(line);
            pc = next;
            break;
        }
        case Vmx64Op::Goto: {
            const uint32_t next = leRead32(p.stream, pc + 2);
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

/// do-while 形态：`int i=0; do { i++; } while (i < n); return i;`
/// javac 产物中循环底部的 if_icmplt true 边直接回到循环头——循环头的 phi
///（i 的 entry/回边两个前驱）把边拷贝放在了分支的 true 边上（sum 循环覆盖
/// 的是 false 边链，两条边都需直接覆盖——T3）。
std::unique_ptr<Vmx64Program> encodeDoWhileTrueEdgeCopy(std::string& reason)
{
    auto* body = new LabelNode(labelScratchArena().create<Label>());
    return encodeMethodNode(
        makeMethod("dw", "(I)I",
                   {
                       new InsnNode(Opcodes::ICONST_0),
                       new VarInsnNode(Opcodes::ISTORE, 1),
                       body,
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new InsnNode(Opcodes::ICONST_1),
                       new InsnNode(Opcodes::IADD),
                       new VarInsnNode(Opcodes::ISTORE, 1),
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new VarInsnNode(Opcodes::ILOAD, 0),
                       new JumpInsnNode(Opcodes::IF_ICMPLT, body->label),
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new InsnNode(Opcodes::IRETURN),
                   },
                   2, 2),
        reason);
}

/// 菱形：`int x; if (a < b) x = a + 1; else x = b - 1; return x;`
/// 汇合点的 phi 在两条出边（then/else 块各自的 jump 边）各生成一份拷贝。
std::unique_ptr<Vmx64Program> encodeDiamondPhi(std::string& reason)
{
    auto* elseLbl = new LabelNode(labelScratchArena().create<Label>());
    auto* join    = new LabelNode(labelScratchArena().create<Label>());
    return encodeMethodNode(
        makeMethod("dia", "(II)I",
                   {
                       new VarInsnNode(Opcodes::ILOAD, 0),
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new JumpInsnNode(Opcodes::IF_ICMPGE, elseLbl->label),
                       new VarInsnNode(Opcodes::ILOAD, 0),
                       new InsnNode(Opcodes::ICONST_1),
                       new InsnNode(Opcodes::IADD),
                       new VarInsnNode(Opcodes::ISTORE, 2),
                       new JumpInsnNode(Opcodes::GOTO, join->label),
                       elseLbl,
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new InsnNode(Opcodes::ICONST_1),
                       new InsnNode(Opcodes::ISUB),
                       new VarInsnNode(Opcodes::ISTORE, 2),
                       join,
                       new VarInsnNode(Opcodes::ILOAD, 2),
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

// ===========================================================================
// 6. ks64 双侧重放回归：NEXT 宏文本与 C++ 侧 vmx64Ks64 的单源契约。
//
//    vmx64_NEXT 宏里的常数（61C88647h/045D9F3Bh）是**硬编码文本**，与
//    kVmx64P1/kVmx64P2 constexpr 是同一公式的两份表达——发射侧 C++ 与
//    .asm handler 侧各算一份，头文件契约要求「改公式必须双侧同步」。这条
//    契约的运行时意义：tEnc 字与站点表项的解密都靠 handler 现场重算
//    ks64——两侧公式哪怕一位漂移，dispatch 直接跳飞。本测试两层锁死：
//    a) 文本层：宏体指令序列逐行比对（imul/rol/xor/imul 次序 + 立即数
//       解析后与 kVmx64P1/P2 相等，大小写不敏感）；宏体固定 19 行，行数
//       漂移即宏被改动，先在此炸出；
//    b) 数值层：按宏语义独立重放（imul → rol13 → xor → imul）与
//       vmx64Ks64 在多组 (x, k1) 采样上逐位相等——即使文本比对被绕过，
//       公式漂移也会在数值上炸出来。
// ===========================================================================
TEST(Vmx64StreamInterpTest, Ks64AsmMacroReplaysCppSide)
{
    const std::string shared = Vmx64AsmEmitter::emitSharedHandlers();
    const auto macroBegin = shared.find("vmx64_NEXT macro\n");
    ASSERT_NE(macroBegin, std::string::npos) << shared;
    const auto macroEnd = shared.find("endm\n", macroBegin);
    ASSERT_NE(macroEnd, std::string::npos) << shared;
    std::string body = shared.substr(macroBegin, macroEnd - macroBegin);

    // --- a) 文本层：行序列 + 常数对账 ---
    std::vector<std::string> lines;
    {
        std::string cur;
        for (const char c : body) {
            if (c == '\n') {
                if (!cur.empty()) lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    ASSERT_EQ(lines.size(), 19u)
        << "vmx64_NEXT macro line count drifted: " << body;

    const auto parseHexLit = [](const std::string& text) -> uint64_t {
        const auto h = text.rfind('h');
        EXPECT_NE(h, std::string::npos) << "not an h-suffix literal: " << text;
        if (h == std::string::npos) return 0;
        return std::stoull(text.substr(0, h), nullptr, 16);
    };
    const auto expectLine = [&](size_t i, const std::string& prefix,
                                const uint64_t* imm, const char* what) {
        ASSERT_LT(i, lines.size()) << what << ": line missing";
        ASSERT_EQ(lines[i].compare(0, prefix.size(), prefix), 0)
            << what << ": expected prefix [" << prefix << "] got [" << lines[i] << "]";
        if (imm != nullptr) {
            const std::string tail = lines[i].substr(prefix.size());
            EXPECT_EQ(parseHexLit(tail), *imm)
                << what << ": constant drift in macro text (" << tail << ")";
        }
    };

    uint64_t p1 = kVmx64P1, p2 = kVmx64P2;
    expectLine(0, "vmx64_NEXT macro", nullptr, "macro head");
    expectLine(1, "    movzx eax, word ptr [r14+rbx]", nullptr, "tEnc fetch");
    expectLine(2, "    imul rcx, rbx, ", &p1, "chain key P1");
    expectLine(3, "    rol rcx, 13", nullptr, "chain rol");
    expectLine(4, "    xor rcx, r15", nullptr, "chain xor K1");
    expectLine(5, "    imul rcx, rcx, ", &p2, "chain key P2");
    expectLine(6, "    xor ax, cx", nullptr, "tEnc decrypt");
    expectLine(7, "    imul r11, rax, ", &p1, "table key P1");
    expectLine(8, "    rol r11, 13", nullptr, "table rol");
    expectLine(9, "    xor r11, r15", nullptr, "table xor K1");
    expectLine(10, "    imul r11, r11, ", &p2, "table key P2");
    expectLine(11, "    mov rdx, [rdi+rax*8]", nullptr, "table load");
    expectLine(12, "    test r12, r12", nullptr, "form branch test");
    expectLine(13, "    jz @F", nullptr, "plain-form artifact branch");
    expectLine(14, "    xor rdx, r11", nullptr, "entry un-XOR (post-link form)");
    expectLine(15, "    @@:", nullptr, "plain-form join (MASM anonymous label)");
    expectLine(16, "    add rdx, r11", nullptr, "ks64 addend");
    expectLine(17, "    sub rdx, r12", nullptr, "image delta compensation");
    expectLine(18, "    jmp rdx", nullptr, "indirect dispatch");
    ASSERT_EQ(lines.size(), 19u) << "trailing lines in vmx64_NEXT macro: " << body;

    // --- b) 数值层：按宏语义独立重放 vs vmx64Ks64 ---
    const auto replayKs64 = [](uint64_t x, uint64_t k1) {
        uint64_t v = x * kVmx64P1;
        v          = (v << 13) | (v >> 51);
        v ^= k1;
        return v * kVmx64P2;
    };
    const uint64_t xs[]   = {0, 1, 6, 114, 0xFFFF, 0x12345, 0xFFFFFFFFu};
    const uint64_t k1s[]  = {0x2C3A78E936A5A840ULL, 0, ~0ULL};
    for (const uint64_t k1 : k1s) {
        for (const uint64_t x : xs) {
            EXPECT_EQ(replayKs64(x, k1), vmx64Ks64(x, k1))
                << "ks64 drift at x=" << x << " k1=" << k1;
        }
    }
}

// ===========================================================================
// 7. T3 分支边拷贝直接覆盖：true 边（do-while 回边）与双出边（菱形 phi）。
//    编码器的 dest∈{比较操作数/暂存槽} fail-closed 防御在真实 SSA 下不可达
//    （SSA 值寄存器唯一），该路径由编码器内联防御承担，不进本测试面。
// ===========================================================================
TEST(Vmx64StreamInterpTest, TrueEdgeCarriesPhiCopiesOnDoWhileBackEdge)
{
    std::string reason;
    auto        prog = encodeDoWhileTrueEdgeCopy(reason);
    ASSERT_NE(prog, nullptr) << reason;

    // do-while 至少执行一轮：n=0 也返回 1。
    const struct
    {
        uint64_t n;
        uint64_t expect;
    } cases[] = {{0, 1}, {1, 1}, {2, 2}, {5, 5}};
    for (const auto& c : cases) {
        const auto r = interpretWithArgs(*prog, {c.n});
        EXPECT_TRUE(r.ok) << "n=" << c.n << " " << r.diag;
        if (r.ok) EXPECT_EQ(r.value, c.expect) << "n=" << c.n << " " << r.diag;
    }
}

TEST(Vmx64StreamInterpTest, DiamondPhiCopiesOnBothOutgoingEdges)
{
    std::string reason;
    auto        prog = encodeDiamondPhi(reason);
    ASSERT_NE(prog, nullptr) << reason;

    const struct
    {
        uint64_t a, b, expect;
    } cases[] = {{1, 5, 2}, {5, 1, 0}, {3, 3, 2}, {0, 0, -1ull & 0xFFFFFFFFu}};
    for (const auto& c : cases) {
        const auto r = interpretWithArgs(*prog, {c.a, c.b});
        EXPECT_TRUE(r.ok) << "a=" << c.a << " b=" << c.b << " " << r.diag;
        if (r.ok) EXPECT_EQ(r.value, c.expect) << "a=" << c.a << " b=" << c.b << " " << r.diag;
    }
}

// ===========================================================================
// 8. T5 回绕语义：i32/i64 加减乘的 2^n 回绕位型（JVM 有符号回绕）。
// ===========================================================================
TEST(Vmx64StreamInterpTest, WraparoundBitPatterns)
{
    std::string reason;
    {
        auto prog = encodeMethodNode(
            makeMethod("w32", "(II)I",
                       {
                           new VarInsnNode(Opcodes::ILOAD, 0),
                           new VarInsnNode(Opcodes::ILOAD, 1),
                           new InsnNode(Opcodes::IADD),
                           new VarInsnNode(Opcodes::ILOAD, 0),
                           new VarInsnNode(Opcodes::ILOAD, 1),
                           new InsnNode(Opcodes::ISUB),
                           new InsnNode(Opcodes::IMUL),
                           new InsnNode(Opcodes::IRETURN),
                       },
                       4, 3),
            reason);
        // (a+b)*(a-b) = a^2-b^2（i32 回绕位型：a^2-b^2 mod 2^32）。
        ASSERT_NE(prog, nullptr) << reason;
        const struct
        {
            uint32_t a, b;
            uint32_t expect;   // (a+b)*(a-b) mod 2^32
        } cases[] = {
            {0x7FFFFFFFu, 1u, 0u},                    // INT_MAX ±1：a^2-b^2 回绕到 0
            {0x80000000u, 1u, 0xFFFFFFFFu},           // INT_MIN ±1：(2^31+1)(2^31-1) 回绕到 -1
            {0u, 1u, 0xFFFFFFFFu},                    // (0+1)*(0-1) = -1
            {0x10000u, 0x10000u, 0u},                 // a^2-b^2 = 0
        };
        for (const auto& c : cases) {
            const auto r = interpretWithArgs(*prog, {c.a, c.b});
            ASSERT_TRUE(r.ok) << r.diag;
            EXPECT_EQ(static_cast<uint32_t>(r.value), c.expect)
                << "a=" << c.a << " b=" << c.b;
        }
    }
    {
        auto prog = encodeMethodNode(
            makeMethod("w64", "(JJ)J",
                       {
                           new VarInsnNode(Opcodes::LLOAD, 0),
                           new InsnNode(Opcodes::LCONST_1),
                           new InsnNode(Opcodes::LADD),
                           new VarInsnNode(Opcodes::LLOAD, 0),
                           new InsnNode(Opcodes::LCONST_1),
                           new InsnNode(Opcodes::LSUB),
                           new InsnNode(Opcodes::LMUL),
                           new InsnNode(Opcodes::LRETURN),
                       },
                       4, 3),
            reason);
        // (a+1)*(a-1) = a^2-1（i64 回绕位型：a^2-1 mod 2^64）。
        ASSERT_NE(prog, nullptr) << reason;
        const struct
        {
            uint64_t a;
            uint64_t expect;
        } cases[] = {
            {0x7FFFFFFFFFFFFFFFULL, 0u},                      // LONG_MAX：a^2-1 回绕到 0
            {0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL},   // LONG_MIN：a^2 回绕到 0 → -1
            {0x100000000ULL, 0xFFFFFFFFFFFFFFFFULL},          // a^2 = 2^64 回绕到 0 → -1
        };
        for (const auto& c : cases) {
            const auto r = interpretWithArgs(*prog, {c.a});
            ASSERT_TRUE(r.ok) << r.diag;
            EXPECT_EQ(r.value, c.expect) << "a=" << c.a;
        }
    }
}

// ===========================================================================
// 9. T4 栈传参（≥3 参数）入口 homing 文本契约：p0/p1 走 r8/r9，p2+ 走
//    [rbp+0x30+8*(i-2)]（call 落返回地址 + 调用方 shadow space 之后的栈参
//    区）。此前的 E2E 只有 1-2 参数方法，栈参 homing 路径零覆盖。
// ===========================================================================
TEST(Vmx64StreamInterpTest, StackArgsHomeFromCallerShadowSpace)
{
    std::string reason;
    // int add3(int a, int b, int c) { return a + b + c; }
    auto prog = encodeMethodNode(
        makeMethod("add3", "(III)I",
                   {
                       new VarInsnNode(Opcodes::ILOAD, 0),
                       new VarInsnNode(Opcodes::ILOAD, 1),
                       new InsnNode(Opcodes::IADD),
                       new VarInsnNode(Opcodes::ILOAD, 2),
                       new InsnNode(Opcodes::IADD),
                       new InsnNode(Opcodes::IRETURN),
                   },
                   2, 3),
        reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->args.size(), 3u) << reason;

    const std::string asmText = Vmx64AsmEmitter::emitMethod(*prog, 0x5EED, "stk");
    ASSERT_EQ(asmText.find(".err"), std::string::npos) << asmText;
    // p0=r8、p1=r9、p2=首个栈参（返回地址 8 + shadow space 0x20 之后）。
    EXPECT_NE(asmText.find("    mov eax, r8d\n"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("    mov eax, r9d\n"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("    mov eax, dword ptr [rbp+30h]\n"), std::string::npos)
        << asmText;
    // 槽落位顺序与编码器 args 契约一致（slot 0/1/2）。
    EXPECT_NE(asmText.find("    mov dword ptr [rsi+0h], eax\n"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("    mov dword ptr [rsi+8h], eax\n"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("    mov dword ptr [rsi+10h], eax\n"), std::string::npos) << asmText;

    // 解释器走 args 契约（slot 直接落参）验证语义：与 homing 路径解耦。
    const auto r = interpretWithArgs(*prog, {10u, 20u, 30u});
    ASSERT_TRUE(r.ok) << r.diag;
    EXPECT_EQ(r.value, 60u);
}

// ===========================================================================
// 10. 数据表契约（post-link 加密器的定位假设，布局总览见
//     doc:vm-site-table-encryption）：表头两 qword（magic/站点数）、表尾两
//     qword（k1/base 自引用指针）与入口 prologue 的 r12 算定序列。
//
//     加密器（vmx64_postlink）不改汇编、不读符号——它在链接产物里按
//     8 字节步扫描 magic，并按「magic → 站点数 → 表项区 → k1 → base」的
//     固定顺序推算表尾位置。发射器是这份布局的唯一生产者，加密器是唯一
//     消费者，两侧没有共享类型可以把守：任何字段的顺序或内容漂移都会让
//     加密器定位失败（fail-closed）或定位到错误字节。发射面与加密面的
//     对账单就是这里的文本级断言。
// ===========================================================================
TEST(Vmx64StreamInterpTest, DataTableCarriesEncryptorContract)
{
    std::string reason;
    auto        prog = encodeSumJavacShape(reason);
    ASSERT_NE(prog, nullptr) << reason;

    const uint64_t seed = 0x5EED;
    const uint64_t k1   = ca::random::SplitMix64(seed ^ 0x766D783634ULL).next();
    const std::string asmText = Vmx64AsmEmitter::emitMethod(*prog, seed, "tag0");
    ASSERT_EQ(asmText.find(".err"), std::string::npos) << asmText;

    // 表头两 qword：magic（ASCII "VMX64TBL" 小端，kVmx64TableMagic）+ 站点
    // 数。二者不落 MASM label——消费方只有加密器的字节级扫描，符号名帮不
    // 上忙（release DLL 无 COFF 符号表）。
    const auto masmHex = [](uint64_t v) {
        std::string h;
        char        buf[32];
        std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
        h = buf;
        if (h[0] >= 'a' && h[0] <= 'f') h.insert(h.begin(), '0');
        return h + "h";
    };
    const size_t magicPos =
        asmText.find(std::string("    dq ") + masmHex(kVmx64TableMagic) + "\n");
    ASSERT_NE(magicPos, std::string::npos) << asmText;
    const size_t countPos =
        asmText.find("    dq " + masmHex(static_cast<uint64_t>(prog->sites.size())) + "\n");
    ASSERT_GT(countPos, magicPos) << asmText;
    // 表尾：k1（加密器的密钥材料，与 prologue 的 mov r15 imm 同一 C++ 局
    // 部变量，无漂移面）+ base 自引用指针（链接值 = 表首选 VA；未加密
    // 产物装载器把它改写成表运行时地址，加密产物重定位熄灭、保持首选
    // VA）。布局顺序即加密器的定位假设：magic → count →（表项区）→ k1 →
    // base，相对顺序一并锁死（表项区各行由站点表发射点另行逐行断言）。
    const size_t k1Pos =
        asmText.find("mj2x_vmx64_k1_tag0 label qword\n    dq " + masmHex(k1) + "\n");
    ASSERT_GT(k1Pos, countPos) << asmText;
    const size_t basePos =
        asmText.find("mj2x_vmx64_base_tag0 label qword\n    dq mj2x_vmx64_sites_tag0\n");
    ASSERT_GT(basePos, k1Pos) << asmText;
    // prologue 的形态/基差算定序列：r12 = base qword 落盘值 - 表运行时地址
    // （rdi）。未加密产物装载器已改写 base → r12 = 0（明文 dispatch）；
    // 加密产物 base 保持首选 VA → r12 = -delta（NEXT 的 sub rdx, r12 补基
    // 差）。这三行是 NEXT 形态分支的前提，逐字符锁定。
    EXPECT_NE(asmText.find("    lea r12, [mj2x_vmx64_base_tag0]\n"
                           "    mov r12, [r12]\n"
                           "    sub r12, rdi\n"),
              std::string::npos)
        << asmText;
}

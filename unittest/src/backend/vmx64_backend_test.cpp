// vmx64 后端单元测试（Phase 1 emit 级）：Java 字节码 -> L1 -> SSA -> RegIR ->
// Vmx64Encoder -> Vmx64AsmEmitter -> MASM x64 .asm 文本。
//
// 本测试不汇编/不调 JNI/DLL（exec 级 E2E 是已知 Phase 1 缺口——模板侧 ml64
// 步骤 + RegisterNatives 注册 C TU 未接线，见 vmx64_backend.hpp），只验证：
//   1. 纯 i32/i64 算术 + 控制流方法能编码成 Vmx64Program 并发射 .asm。
//   2. 确定性：同 seed 两次 emit 逐字节相同；不同 seed 产物不同。
//   3. fail-closed：不支持的形态（除法/字段/浮点/实例方法 this）编码失败且
//      不落半成品（成功方法与失败方法混编时产物只含成功方法段）。
//   4. .asm 结构 sanity：jmp rdx、加密站点表（label - addend 形态）、
//      PROC FRAME 入口、流/表数据段、vmx64_NEXT 宏。

#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "test_helper.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/backend.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/backend/vmx64/vmx64_backend.hpp"
#include "mj2x/backend/vmx64/vmx64_encoder.hpp"
#include "mj2x/backend/vmx64/vmx64_opcode.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

// B8b：测试侧 Label 统一归宿（进程寿命 scratch arena）。
static garena::Arena& labelScratchArena()
{
    static garena::Arena arena;
    return arena;
}


using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

/// @brief 构造 MethodNode（name/desc 必须是静态存储期 C 字符串）。
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

/// 完整编码管线：MethodNode -> RegIR -> Vmx64Program（结构与 vmc4_backend_test
/// 的 encodeMethodNode 同源，末段换 Vmx64Encoder）。
std::unique_ptr<Vmx64Program> encodeMethodNode(std::unique_ptr<MethodNode> mn,
                                               std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = test::convertMethodNode(converter, *mn, arena);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmx64", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    return Vmx64Encoder::encode(*reg, reason);
}

/// 便捷重载：从指令列表构造 MethodNode 再编码（静态方法）。
std::unique_ptr<Vmx64Program> encodeMethod(const std::string& name, const std::string& desc,
                                           std::initializer_list<AbstractInsnNode*> insns,
                                           int maxStack, int maxLocals, std::string& reason)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name.c_str(), desc.c_str(),
                         insns, maxStack, maxLocals);
    return encodeMethodNode(std::move(mn), reason);
}

/// 走 Backend 契约的完整 emit：compileMethod + finalize → generatedSource。
std::string emitViaBackend(const char* name, const char* desc,
                           std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                           int maxLocals, uint64_t seed, std::string* methodTagOut = nullptr)
{
    auto        method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns,
                                    maxStack, maxLocals);
    CompilerMethod cm  = test::snapshotMethod(*method);
    CompilerClass  cls;
    cls.methods.push_back(cm);

    Vmx64Backend backend;
    Vmx64Options opts;
    opts.seed = seed;
    backend.setOptions(opts);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto    blocks = test::convertMethodNode(converter, *method, arena);
    BackendResult result = backend.compileMethod("Vmx64T", cls.methods[0], blocks);
    if (methodTagOut != nullptr) {
        const std::string methodId = std::string("Vmx64T.") + name + desc;
        *methodTagOut              = codegen::hex64(codegen::fnv1a64(methodId));
    }
    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    return result.kind == BackendResult::Kind::ASM_SOURCE
               ? backend.generatedSource()
               : std::string();
}

/// 统计子串出现次数。
size_t countOccurrences(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

}   // namespace

// ===========================================================================
// 1. 纯 i32 算术方法成功编码（含字面量物化与常量折叠路径的 add/mul）。
// ===========================================================================
TEST(Vmx64BackendTest, EncodesPureArithmeticMethod)
{
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
                             3, 2, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_FALSE(prog->stream.empty()) << "stream must be non-empty";
    EXPECT_GE(prog->sites.size(), 2u);
    EXPECT_GE(prog->numRegs, prog->numInputs);
    EXPECT_EQ(prog->numSlots, prog->numRegs + 2) << "two literal scratch slots";
    EXPECT_TRUE(prog->returnsValue);
    EXPECT_FALSE(prog->retIs64);
}

// ===========================================================================
// 2. i64 方法成功编码（lconst/ladd/lret 路径）。
// ===========================================================================
TEST(Vmx64BackendTest, EncodesLongArithmeticMethod)
{
    std::string reason;
    // long inc(long a) { return a + 1L; }
    auto prog = encodeMethod("inc", "(J)J",
                             {
                                 new VarInsnNode(Opcodes::LLOAD, 0),
                                 new InsnNode(Opcodes::LCONST_1),
                                 new InsnNode(Opcodes::LADD),
                                 new InsnNode(Opcodes::LRETURN),
                             },
                             4, 3, reason);
    ASSERT_NE(prog, nullptr) << reason;
    EXPECT_TRUE(prog->returnsValue);
    EXPECT_TRUE(prog->retIs64);
    EXPECT_EQ(prog->args.size(), 1u);
    EXPECT_TRUE(prog->args[0].is64) << "long parameter takes the 64-bit path";
    // 站点里必须有 LAdd 与 LReturn。
    bool sawLAdd = false, sawLRet = false, sawLConst = false;
    for (const auto& site : prog->sites) {
        if (site.op == Vmx64Op::LAdd) sawLAdd = true;
        if (site.op == Vmx64Op::LReturn) sawLRet = true;
        if (site.op == Vmx64Op::LConst) sawLConst = true;
    }
    EXPECT_TRUE(sawLAdd) << "ladd site expected";
    EXPECT_TRUE(sawLRet) << "lreturn site expected";
    EXPECT_TRUE(sawLConst) << "1L materializes via lconst";
}

// ===========================================================================
// 3. 循环方法成功编码（Compare+Branch 融合为 if_icmp 族 + goto 回边）。
// ===========================================================================
TEST(Vmx64BackendTest, EncodesLoopMethodWithFusedBranch)
{
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
                             2, 3, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // 融合检查：流里有 if_icmp 族站点与 goto 站点，且没有独立站点承载
    // Compare（Compare 只能以融合形态存在）。
    int ifIcmp = 0, gotos = 0, rets = 0;
    for (const auto& site : prog->sites) {
        switch (site.op) {
        case Vmx64Op::IfIcmpEq:
        case Vmx64Op::IfIcmpNe:
        case Vmx64Op::IfIcmpLt:
        case Vmx64Op::IfIcmpLe:
        case Vmx64Op::IfIcmpGt:
        case Vmx64Op::IfIcmpGe: ifIcmp++; break;
        case Vmx64Op::Goto: gotos++; break;
        case Vmx64Op::IReturn:
        case Vmx64Op::LReturn:
        case Vmx64Op::VReturn: rets++; break;
        default: break;
        }
    }
    EXPECT_GE(ifIcmp, 1) << "loop condition must fuse into an if_icmp site";
    EXPECT_GE(gotos, 1) << "loop back-edge must encode as goto";
    EXPECT_GE(rets, 1);
}

// ===========================================================================
// 4. 确定性：同 seed 两次 emit 逐字节相同。
// ===========================================================================
TEST(Vmx64BackendTest, DeterministicEmitForSameSeed)
{
    const auto mkInsns = []() {
        return std::initializer_list<AbstractInsnNode*>{
            new VarInsnNode(Opcodes::ILOAD, 0), new InsnNode(Opcodes::ICONST_3),
            new InsnNode(Opcodes::IMUL), new InsnNode(Opcodes::IRETURN)};
    };
    const std::string a = emitViaBackend("det", "(I)I", mkInsns(), 2, 1, 0xBEEF);
    const std::string b = emitViaBackend("det", "(I)I", mkInsns(), 2, 1, 0xBEEF);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b) << "same seed + same input must be byte-identical";
    // 头注释不含时间戳/绝对路径类内容（确定性文本抽查）。
    EXPECT_EQ(a.find("Generated at"), std::string::npos);
    EXPECT_EQ(a.find("C:"), std::string::npos);
}

// ===========================================================================
// 5. 不同 seed 产物不同（密文流/表随 seed 漂移）。
// ===========================================================================
TEST(Vmx64BackendTest, DifferentSeedProducesDifferentArtifact)
{
    const auto mkInsns = []() {
        return std::initializer_list<AbstractInsnNode*>{
            new VarInsnNode(Opcodes::ILOAD, 0), new InsnNode(Opcodes::ICONST_3),
            new InsnNode(Opcodes::IMUL), new InsnNode(Opcodes::IRETURN)};
    };
    const std::string a = emitViaBackend("sd", "(I)I", mkInsns(), 2, 1, 0x1111);
    const std::string b = emitViaBackend("sd", "(I)I", mkInsns(), 2, 1, 0x2222);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_NE(a, b) << "different seeds must produce different .asm artifacts";
    // 具体到密文：指令流 DB 行与站点表 DQ addend 都应不同。
    const auto dbLineA = a.find("    db ");
    const auto dbLineB = b.find("    db ");
    ASSERT_NE(dbLineA, std::string::npos);
    ASSERT_NE(dbLineB, std::string::npos);
    EXPECT_NE(a.substr(dbLineA, a.find('\n', dbLineA) - dbLineA),
              b.substr(dbLineB, b.find('\n', dbLineB) - dbLineB))
        << "encrypted stream bytes must differ across seeds";
}

// ===========================================================================
// 6. fail-closed：不支持的形态编码失败并给原因。
// ===========================================================================

// 除法（Phase 1 只支持 add/sub/mul）。
TEST(Vmx64BackendTest, FailClosedOnIntegerDivision)
{
    std::string reason;
    auto prog = encodeMethod("dv", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IDIV),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, reason);
    EXPECT_EQ(prog, nullptr);
    EXPECT_NE(reason.find("binary op unsupported"), std::string::npos) << reason;
}

// 字段访问。
TEST(Vmx64BackendTest, FailClosedOnFieldAccess)
{
    std::string reason;
    auto prog = encodeMethod("gf", "(Ltest/MyObj;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new FieldInsnNode(Opcodes::GETFIELD,
                                                   ca::str::Utf8StringRef::from_cstr("test/MyObj"),
                                                   ca::str::Utf8StringRef::from_cstr("x"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1, 1, reason);
    EXPECT_EQ(prog, nullptr);
}

// 浮点。
TEST(Vmx64BackendTest, FailClosedOnFloatArithmetic)
{
    std::string reason;
    auto prog = encodeMethod("fadd", "(FF)F",
                             {
                                 new VarInsnNode(Opcodes::FLOAD, 0),
                                 new VarInsnNode(Opcodes::FLOAD, 1),
                                 new InsnNode(Opcodes::FADD),
                                 new InsnNode(Opcodes::FRETURN),
                             },
                             3, 2, reason);
    EXPECT_EQ(prog, nullptr);
}

// 实例方法（this 为 REF 参数）——经 Backend 契约验证 fail-closed 保留字节码。
TEST(Vmx64BackendTest, FailClosedOnInstanceMethodViaBackend)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC, "bump", "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2);
    CompilerMethod cm = test::snapshotMethod(*method);
    CompilerClass  cls;
    cls.methods.push_back(cm);

    Vmx64Backend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = test::convertMethodNode(converter, *method, arena);
    auto          result = backend.compileMethod("Vmx64T", cls.methods[0], blocks);
    EXPECT_EQ(result.kind, BackendResult::Kind::NONE)
        << "this (REF) parameter must fail-closed in Phase 1";
}

// ===========================================================================
// 7. fail-closed 不落半成品：成功方法与失败方法混编时产物只含成功方法段。
// ===========================================================================
TEST(Vmx64BackendTest, FailClosedLeavesNoPartialArtifact)
{
    Vmx64Backend backend;
    Vmx64Options opts;
    opts.seed = 7;
    backend.setOptions(opts);
    CompilerClass cls;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);

    auto okMethod = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "ok", "(I)I",
                               {
                                   new VarInsnNode(Opcodes::ILOAD, 0),
                                   new InsnNode(Opcodes::ICONST_1),
                                   new InsnNode(Opcodes::IADD),
                                   new InsnNode(Opcodes::IRETURN),
                               },
                               2, 1);
    auto badMethod = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "bad", "(II)I",
                                {
                                    new VarInsnNode(Opcodes::ILOAD, 0),
                                    new VarInsnNode(Opcodes::ILOAD, 1),
                                    new InsnNode(Opcodes::IDIV),
                                    new InsnNode(Opcodes::IRETURN),
                                },
                                2, 2);
    auto okCm  = test::snapshotMethod(*okMethod);
    auto badCm = test::snapshotMethod(*badMethod);
    cls.methods.push_back(okCm);
    cls.methods.push_back(badCm);

    auto okBlocks  = test::convertMethodNode(converter, *okMethod, arena);
    auto badBlocks = test::convertMethodNode(converter, *badMethod, arena);
    auto okResult  = backend.compileMethod("Mix", cls.methods[0], okBlocks);
    auto badResult = backend.compileMethod("Mix", cls.methods[1], badBlocks);
    EXPECT_EQ(okResult.kind, BackendResult::Kind::ASM_SOURCE);
    EXPECT_EQ(badResult.kind, BackendResult::Kind::NONE);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const std::string& src = backend.generatedSource();
    const std::string okTag =
        codegen::hex64(codegen::fnv1a64("Mix.ok(I)I"));
    const std::string badTag =
        codegen::hex64(codegen::fnv1a64("Mix.bad(II)I"));
    EXPECT_NE(src.find("mj2x_vmx64_exec_" + okTag), std::string::npos)
        << "successful method section must be present";
    EXPECT_EQ(src.find("mj2x_vmx64_exec_" + badTag), std::string::npos)
        << "failed method must leave no partial artifact";
}

// ===========================================================================
// 8. .asm 结构 sanity：jmp rdx / 加密表 / PROC FRAME / 数据段。
// ===========================================================================
TEST(Vmx64BackendTest, AsmStructureSanity)
{
    std::string tag;
    const std::string src = emitViaBackend(
        "sanity", "(II)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new VarInsnNode(Opcodes::ILOAD, 1),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        2, 2, 0x1234, &tag);
    ASSERT_FALSE(src.empty()) << "backend emit must succeed";
    ASSERT_FALSE(tag.empty());

    // dispatcher 间接跳转链核心特征。
    EXPECT_NE(src.find("jmp rdx"), std::string::npos)
        << "register-indirect dispatch (jmp rdx) missing";
    EXPECT_GE(countOccurrences(src, "jmp rdx"), 1);
    // 每条共享 handler 的尾部分发都经 vmx64_NEXT 宏实例化。
    EXPECT_NE(src.find("vmx64_NEXT macro"), std::string::npos);
    EXPECT_GE(countOccurrences(src, "    vmx64_NEXT\n"), 20u)
        << "every handler tail must dispatch through vmx64_NEXT";

    // 加密站点表：DQ label-addend 形态（编译期 seed 派生加密）。
    EXPECT_NE(src.find("mj2x_vmx64_sites_" + tag + " label qword"), std::string::npos);
    EXPECT_NE(src.find("dq mj2x_vmx64_h_"), std::string::npos);
    EXPECT_NE(src.find("h\n"), std::string::npos)
        << "site table entries must carry seed-derived addends (h-suffix)";

    // 指令流数据段。
    EXPECT_NE(src.find("mj2x_vmx64_code_" + tag + " label byte"), std::string::npos);
    EXPECT_NE(src.find("    db "), std::string::npos);

    // ml64 拒绝 0x 前缀（A2206 missing operator in expression，本机 14.44 实证）：
    // 产物数值必须全是 h 后缀形态，任何 0x 字面量都是发射器回归。
    EXPECT_EQ(src.find("0x"), std::string::npos)
        << "ml64 rejects 0x-prefixed literals; all numbers must be h-suffix";

    // JNI 入口 PROC（SEH unwind 注记齐全）。
    EXPECT_NE(src.find("mj2x_vmx64_exec_" + tag + " proc frame"), std::string::npos);
    EXPECT_NE(src.find(".endprolog"), std::string::npos);
    EXPECT_NE(src.find("mj2x_vmx64_exec_" + tag + " endp"), std::string::npos);
    EXPECT_NE(src.find("public mj2x_vmx64_exec_" + tag), std::string::npos);

    // 共享 handler 集存在（iconst/算术/返回族）。
    EXPECT_NE(src.find("mj2x_vmx64_h_iadd:"), std::string::npos);
    EXPECT_NE(src.find("mj2x_vmx64_h_iret:"), std::string::npos);
    EXPECT_NE(src.find("mj2x_vmx64_h_goto:"), std::string::npos);
}

// ===========================================================================
// 9. Backend 产物元数据：MasmX64 语言 + vmx64.asm 建议名。
// ===========================================================================
TEST(Vmx64BackendTest, ArtifactLanguageIsMasmX64)
{
    Vmx64Backend backend;
    const auto   artifact = backend.generatedSourceArtifact();
    EXPECT_EQ(artifact.language, NativeSourceLanguage::MasmX64);
    EXPECT_EQ(std::string(artifact.suggestedFileName), std::string("vmx64.asm"));
    EXPECT_EQ(backend.name(), std::string_view("vmx64"));
}

// ===========================================================================
// 10. seed 派生接口：确定性 + 身份混合（后续接 VMC4 式 seed_derivation 的锚点）。
// ===========================================================================
TEST(Vmx64BackendTest, InstanceSeedDerivationIsDeterministic)
{
    EXPECT_EQ(vmx64DeriveInstanceSeed(0x11, 0xAA), vmx64DeriveInstanceSeed(0x11, 0xAA));
    EXPECT_NE(vmx64DeriveInstanceSeed(0x11, 0xAA), vmx64DeriveInstanceSeed(0x11, 0xBB));
    EXPECT_NE(vmx64DeriveInstanceSeed(0x11, 0xAA), vmx64DeriveInstanceSeed(0x22, 0xAA));
}

// ===========================================================================
// 11. 链字加密 sanity：流加密确实由 instance seed 派生的密钥驱动。
// ===========================================================================
TEST(Vmx64BackendTest, StreamCarriesEncryptedHandlerNumbers)
{
    std::string reason;
    auto prog = encodeMethod("enc", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_GE(prog->sites.size(), 3u);

    // 行为级断言（不重复实现发射器内部 K1 派生）：同 program + 同 seed → 同
    // 密文 db 行；不同 instance seed → 密文 db 行不同（密钥参与流加密）。
    const std::string firstDb = "    db ";
    const auto        firstDbLine = [&firstDb](const std::string& text) {
        const auto pos = text.find(firstDb);
        return pos == std::string::npos ? std::string{}
                                        : text.substr(pos, text.find('\n', pos) - pos);
    };
    const uint64_t seedA = vmx64DeriveInstanceSeed(0xABCD, 0x1234);
    const uint64_t seedB = vmx64DeriveInstanceSeed(0xABCE, 0x1234);
    const std::string asmA1 = Vmx64AsmEmitter::emitMethod(*prog, seedA, "enc0");
    const std::string asmA2 = Vmx64AsmEmitter::emitMethod(*prog, seedA, "enc0");
    const std::string asmB  = Vmx64AsmEmitter::emitMethod(*prog, seedB, "enc0");
    ASSERT_NE(firstDbLine(asmA1), std::string()) << asmA1;
    EXPECT_EQ(firstDbLine(asmA1), firstDbLine(asmA2))
        << "same instance seed must encrypt the stream identically";
    EXPECT_NE(firstDbLine(asmA1), firstDbLine(asmB))
        << "instance seed must key the stream cipher";
}

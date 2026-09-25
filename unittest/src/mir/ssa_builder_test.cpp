// R1a 端到端验收：Java 方法 → CodeConverter → SSABuilder → SSAFunction。
//
// 验收标准（mj2x/doc_ai/ssa_algorithm.md §7）：
//   1. add: 线性方法，SSA 无栈槽引用，dump 不含 cstack/sp。
//   2. loopSum/loopCount: 循环方法，循环头块有 block arg（phi 语义）。
//   3. dump 文本过 c2j CSTACK_RE（cstack(\d+)\.(\w)）→ 零匹配（消除锚点 2）。
//
// 样本：test_resource/ssa/ssa/SsaSamples.class（javac 编译）
#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jdk_fixture.hpp"
#include "test_helper.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/mir/ssa/ssa_dump.hpp"
#include "mj2x/mj2x.hpp"

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
using namespace ca::str;

namespace {

namespace fs = std::filesystem;

std::string toStr(Utf8StringRef ref)
{
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

// 读 SsaSamples.class，返回 classNode（arena 由调用方持有以保活 Utf8StringRef）。
ClassNode* loadSamples(Utf8StringArena& arena, std::vector<ca::u8>& bytesOut)
{
    bytesOut       = test::getResource("ssa/ssa/SsaSamples.class");
    auto        cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytesOut.data(), static_cast<ca::i32>(bytesOut.size()));
    reader.accept(cn.get(), 0);
    // 返回裸指针；调用方持 arena + cn 容器。
    // 为简化，这里用一个全局持有 cn（测试用）。
    static std::unique_ptr<ClassNode> g_cn;
    g_cn = std::move(cn);
    return g_cn.get();
}

MethodNode* findMethod(ClassNode* cn, const std::string& name, const std::string& desc)
{
    for (auto* m : cn->methods) {
        if (toStr(m->name) == name && toStr(m->desc) == desc) return m;
    }
    return nullptr;
}

// 跑 CodeConverter + SSABuilder，返回 SSAFunction（或 nullptr + reason）。
// arena 由调用方持有——SSAFunction 节点字段是 arena-backed Utf8StringRef（spec §6.1），
// arena 必须活到调用方消费完 SSA（dump/lower/emit）为止（参见 #698）。
std::unique_ptr<SSAFunction> buildSSAFor(ClassNode* cn, MethodNode* m,
                                         ca::str::Utf8StringArena& arena, std::string& reason)
{
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*m);
    SSABuilder    builder;
    bool          isStatic = (m->access & Opcodes::ACC_STATIC) != 0;
    return builder.build(blocks, toStr(cn->name), toStr(m->name), toStr(m->desc), isStatic,
                         m->maxLocals, arena, reason);
}

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

TEST(SSAFunctionTest, InternFloatPreservesIeee754Identity)
{
    SSAFunction function;

    auto* negativeZero = function.internFloat(-0.0, SSAType::F64);
    auto* positiveZero = function.internFloat(0.0, SSAType::F64);
    auto* firstNan = function.internFloat(std::numeric_limits<double>::quiet_NaN(), SSAType::F64);
    auto* secondNan = function.internFloat(std::numeric_limits<double>::quiet_NaN(), SSAType::F64);

    EXPECT_NE(negativeZero, positiveZero);
    EXPECT_EQ(firstNan, secondNan);
    EXPECT_EQ(function.floatLiterals.size(), 3u);
}

// buildInlineSSA 的返回包：把 arena 与 fn 绑在一起，保证 arena 与 SSAFunction 同生共死。
// SSAFunction 节点字段是 arena-backed Utf8StringRef（spec §6.1），arena 析构会让 fn 内的
// ref 全部悬垂——故不能用局部 arena + 返回 fn 的旧写法（参见 #698）。
struct InlineSsaBundle
{
    std::unique_ptr<SSAFunction>      fn;
    std::unique_ptr<Utf8StringArena>  arena;   // 先声明 fn 后声明 arena：fn 先析构，再释放 arena
    operator bool() const { return fn != nullptr; }
    bool          operator==(std::nullptr_t) const { return fn == nullptr; }
    bool          operator!=(std::nullptr_t) const { return fn != nullptr; }
    SSAFunction&  operator*() const { return *fn; }
    SSAFunction*  operator->() const { return fn.get(); }
};

InlineSsaBundle buildInlineSSA(const std::string& name, const std::string& desc,
                               std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                               int maxLocals, std::string& reason)
{
    InlineSsaBundle bundle;
    bundle.arena = std::make_unique<Utf8StringArena>();
    ClassNode     cn;
    cn.name = ca::str::Utf8StringRef::from_cstr("InlineSsa");
    auto         mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns, maxStack, maxLocals);
    bundle.fn = buildSSAFor(&cn, mn.get(), *bundle.arena, reason);
    return bundle;
}

struct ExecResult
{
    int         exitCode = -1;
    std::string out;
};

std::string quotePath(const fs::path& path)
{
    return "\"" + path.string() + "\"";
}

bool fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

ExecResult runScript(const std::vector<std::string>& lines)
{
    ExecResult      result;
    fs::path        tmpDir = test::getTempOutPath("mj2x_ssa_javac.keep").parent_path();
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    fs::path script =
        tmpDir / ("mj2x_ssa_javac_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
#ifdef _WIN32
                  ".bat");
#else
                  ".sh");
#endif

    {
        std::ofstream out(script);
#ifdef _WIN32
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
#else
        out << "#!/bin/sh\n";
#endif
        for (const auto& line : lines) out << line << "\n";
#ifdef _WIN32
        out << "exit /b %errorlevel%\n";
#endif
    }

#ifdef _WIN32
    std::string cmd = quotePath(script) + " 2>&1";
#else
    std::string cmd = "sh " + quotePath(script) + " 2>&1";
#endif
    FILE* pipe =
#ifdef _WIN32
        _popen(cmd.c_str(), "rb");
#else
        popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        result.out = "failed to run script: " + script.string();
        return result;
    }
    char buffer[4096];
    while (size_t n = fread(buffer, 1, sizeof(buffer), pipe)) {
        result.out.append(buffer, n);
    }
#ifdef _WIN32
    result.exitCode = _pclose(pipe);
#else
    result.exitCode = pclose(pipe);
#endif
    fs::remove(script, ec);
    return result;
}

fs::path findJavac()
{
    // 复用共享 JDK 探测（底层 libjdk_finder：JAVA_HOME→MORPHER_JDK_HOME→fallback），
    // 替代原先硬编码作者机器路径的回落。
    return test::jdk::probeJdk().javac;
}

fs::path compileJavaSample(const fs::path& javac, const std::string& caseName,
                           const std::string& sourceResource, const std::string& className)
{
    fs::path outDir = test::getOutPath("mj2x/ssa_cases/" + caseName + "/.keep").parent_path();
    std::error_code ec;
    fs::create_directories(outDir, ec);
    fs::path sourcePath = test::getResourcePath(sourceResource);
    auto     result     = runScript({
        quotePath(javac) + " -g -d " + quotePath(outDir) + " " + quotePath(sourcePath),
    });
    EXPECT_EQ(result.exitCode, 0) << "javac failed:\n" << result.out;
    return outDir / "ssa_cases" / (className + ".class");
}

std::unique_ptr<ClassNode> loadClassFile(Utf8StringArena& arena, const fs::path& classFile,
                                         std::vector<ca::u8>& bytesOut)
{
    std::ifstream in(classFile, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) return nullptr;
    bytesOut.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytesOut.data()),
            static_cast<std::streamsize>(bytesOut.size()));

    auto        cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytesOut.data(), static_cast<ca::i32>(bytesOut.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

}   // namespace

// ============================================================================
// add(I)I —— 线性方法，基本 IR 生成 + 无栈槽
// ============================================================================
TEST(SSABuilderE2E, AddProducesStacklessSSA)
{
    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    ClassNode*          cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "add", "(II)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto        fn = buildSSAFor(cn, m, arena, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);

    // 验收 1：dump 不含 c2j 栈锚点子串。
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << "dump must not contain cstack";
    EXPECT_EQ(dump.find("sp"), std::string::npos) << "dump must not contain sp";

    // 验收 2：c2j CSTACK_RE 正则（cstack(\d+)\.(\w)）零匹配。
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe))
        << "CSTACK_RE must not match SSA dump (锚点 2 已打破)";

    // 验收 3：至少有一条 add 指令（param0 + param1）。
    EXPECT_NE(dump.find("+"), std::string::npos) << "add method must contain an add operation:\n"
                                                 << dump;

    // 打印 dump 供人工检查（gtest 在失败或 --gtest_also_run_disabled_tests 时可见）
    if (::testing::Test::HasFailure()) {
        printf("=== dump ===\n%s\n", dump.c_str());
    }
}

// ============================================================================
// loopSum(I)I —— 循环，验证 block-arg phi
// ============================================================================
TEST(SSABuilderE2E, LoopSumHasPhiBlockArg)
{
    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    ClassNode*          cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto        fn = buildSSAFor(cn, m, arena, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);

    // 验收 1：dump 无栈锚点 + CSTACK_RE 零匹配。
    EXPECT_EQ(dump.find("cstack"), std::string::npos);
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe));

    // 验收 2：存在带 args 的块（= block-arg phi）。循环头块应有 args。
    // loopSum 的循环头对 s(local1) 和 i(local2) 各有 ≥2 def 源 → ≥2 args。
    bool foundBlockWithArgs = false;
    int  maxArgsOnAnyBlock  = 0;
    for (auto& b : fn->blocks) {
        int argCount = static_cast<int>(b->args.size());
        if (argCount > maxArgsOnAnyBlock) maxArgsOnAnyBlock = argCount;
        if (argCount >= 2) foundBlockWithArgs = true;
    }
    EXPECT_TRUE(foundBlockWithArgs)
        << "loopSum must have a block with >=2 args (phi for s and i):\n"
        << dump;

    if (::testing::Test::HasFailure()) {
        printf("=== loopSum dump ===\n%s\n", dump.c_str());
    }
}

// ============================================================================
// loopCount(I)I —— while 循环，验证回边 phi
// ============================================================================
TEST(SSABuilderE2E, LoopCountHasPhiBlockArg)
{
    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    ClassNode*          cn = loadSamples(arena, bytes);
    ASSERT_NE(cn, nullptr);
    auto* m = findMethod(cn, "loopCount", "(I)I");
    ASSERT_NE(m, nullptr);

    std::string reason;
    auto        fn = buildSSAFor(cn, m, arena, reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_EQ(dump.find("cstack"), std::string::npos);
    std::regex cstackRe("cstack(\\d+)\\.(\\w)");
    EXPECT_FALSE(std::regex_search(dump, cstackRe));

    // loopCount 的 c(local1) 在回边目标有 2 个 def（iconst_0 + iinc）→ ≥1 arg。
    bool foundBlockWithArgs = false;
    for (auto& b : fn->blocks) {
        if (!b->args.empty()) {
            foundBlockWithArgs = true;
            break;
        }
    }
    EXPECT_TRUE(foundBlockWithArgs) << "loopCount must have a block with args (phi for c):\n"
                                    << dump;

    if (::testing::Test::HasFailure()) {
        printf("=== loopCount dump ===\n%s\n", dump.c_str());
    }
}

TEST(SSABuilderE2E, ParsesWideMethodDescriptorAndBindsEntryParameters)
{
    std::string reason;
    auto        fn = buildInlineSSA("retDouble",
                             "(JD)D",
                                    {
                                 new VarInsnNode(Opcodes::DLOAD, 2),
                                 new InsnNode(Opcodes::DRETURN),
                             },
                             2,
                             4,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    ASSERT_EQ(fn->symbol.parameterTypes.size(), 2u);
    EXPECT_EQ(fn->symbol.parameterTypes[0], SSAType::I64);
    EXPECT_EQ(fn->symbol.parameterTypes[1], SSAType::F64);
    EXPECT_EQ(fn->symbol.returnType, SSAType::F64);
    ASSERT_EQ(fn->parameters.size(), 2u);
    EXPECT_EQ(fn->parameters[0]->index, 0u);
    EXPECT_EQ(fn->parameters[0]->type, SSAType::I64);
    EXPECT_EQ(fn->parameters[1]->index, 2u);
    EXPECT_EQ(fn->parameters[1]->type, SSAType::F64);

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("ret param2"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("<opaque:undef_local_2>"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersCastAndCompareForRegCInput)
{
    auto*       elseLabel = new LabelNode(labelScratchArena().create<Label>());
    std::string reason;
    auto        fn = buildInlineSSA("castCmp",
                             "(D)I",
                                    {
                                 new VarInsnNode(Opcodes::DLOAD, 0),
                                 new InsnNode(Opcodes::D2I),
                                 new InsnNode(Opcodes::ICONST_5),
                                 new JumpInsnNode(Opcodes::IF_ICMPGE, elseLabel->label),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IRETURN),
                                 elseLabel,
                                 new InsnNode(Opcodes::ICONST_0),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             4,
                             2,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("convert param0 -> i32"), std::string::npos) << dump;
    EXPECT_NE(dump.find("cmp "), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("sp"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersSwitchTerminatorForRegCInput)
{
    auto* case0        = new LabelNode(labelScratchArena().create<Label>());
    auto* case1        = new LabelNode(labelScratchArena().create<Label>());
    auto* defaultLabel = new LabelNode(labelScratchArena().create<Label>());

    std::string reason;
    auto        fn = buildInlineSSA(
        "pick",
        "(I)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new TableSwitchInsnNode(0, 1, defaultLabel->label, {case0->label, case1->label}),
            case0,
            new IntInsnNode(Opcodes::BIPUSH, 7),
            new InsnNode(Opcodes::IRETURN),
            case1,
            new IntInsnNode(Opcodes::BIPUSH, 9),
            new InsnNode(Opcodes::IRETURN),
            defaultLabel,
            new InsnNode(Opcodes::ICONST_M1),
            new InsnNode(Opcodes::IRETURN),
        },
        2,
        1,
        reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("switch param0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("case 0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("case 1"), std::string::npos) << dump;
    EXPECT_NE(dump.find("default"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("sp"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersStringLdcAsOpaqueReference)
{
    std::string reason;
    auto        fn = buildInlineSSA("literal",
                             "()Ljava/lang/String;",
                                    {
                                 new LdcInsnNode(std::string("hello")),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             1,
                             0,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    // The String LDC opaque now carries the literal content after the
    // "ldc_string:" prefix so String intrinsics (parseInt/literal/toString)
    // can recover it.  The dump must still read as a stackless opaque form.
    EXPECT_NE(dump.find("<opaque:ldc_string:hello>"), std::string::npos) << dump;
    EXPECT_NE(dump.find("ret <opaque:ldc_string:hello>"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("cstack"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersFieldsAndStaticFieldsForRegCInput)
{
    std::string reason;
    auto        fn = buildInlineSSA("fields",
                             "(LFoo;)J",
                                    {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new FieldInsnNode(Opcodes::GETFIELD,
                                                   ca::str::Utf8StringRef::from_cstr("Foo"),
                                                   ca::str::Utf8StringRef::from_cstr("x"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new FieldInsnNode(Opcodes::PUTSTATIC,
                                                   ca::str::Utf8StringRef::from_cstr("Foo"),
                                                   ca::str::Utf8StringRef::from_cstr("sink"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new FieldInsnNode(Opcodes::GETSTATIC,
                                                   ca::str::Utf8StringRef::from_cstr("Foo"),
                                                   ca::str::Utf8StringRef::from_cstr("wide"),
                                                   ca::str::Utf8StringRef::from_cstr("J")),
                                 new InsnNode(Opcodes::LRETURN),
                             },
                             2,
                             1,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("getfield Foo.x:I from param0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("putstatic Foo.sink:I"), std::string::npos) << dump;
    EXPECT_NE(dump.find("getstatic Foo.wide:J"), std::string::npos) << dump;
    EXPECT_EQ(fn->symbol.returnType, SSAType::I64);
}

TEST(SSABuilderE2E, LowersInvokeObjectAllocationAndConstructorPattern)
{
    std::string reason;
    auto        fn = buildInlineSSA(
        "create",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        },
        2,
        0,
        reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("alloc object java/lang/Object()"), std::string::npos) << dump;
    EXPECT_NE(dump.find("call java/lang/Object.<init>()V("), std::string::npos) << dump;
    EXPECT_NE(dump.find("ret v"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersStaticInvokeWithWideArguments)
{
    std::string reason;
    auto        fn = buildInlineSSA("callWide",
                             "(ID)J",
                                    {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::DLOAD, 1),
                                 new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                    ca::str::Utf8StringRef::from_cstr("Other"),
                                                    ca::str::Utf8StringRef::from_cstr("mix"),
                                                    ca::str::Utf8StringRef::from_cstr("(ID)J"),
                                                    false),
                                 new InsnNode(Opcodes::LRETURN),
                             },
                             4,
                             3,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("call Other.mix(ID)J(param0, param1)"), std::string::npos) << dump;
    EXPECT_NE(dump.find("ret v"), std::string::npos) << dump;
    ASSERT_FALSE(fn->blocks.empty());
}

TEST(SSABuilderE2E, LowersArrayOperationsForRegCInput)
{
    std::string reason;
    auto        fn = buildInlineSSA("arrayOps",
                             "()I",
                                    {
                                 new InsnNode(Opcodes::ICONST_2),
                                 new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_INT),
                                 new InsnNode(Opcodes::DUP),
                                 new InsnNode(Opcodes::ICONST_0),
                                 new InsnNode(Opcodes::ICONST_5),
                                 new InsnNode(Opcodes::IASTORE),
                                 new InsnNode(Opcodes::DUP),
                                 new InsnNode(Opcodes::ICONST_0),
                                 new InsnNode(Opcodes::IALOAD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             4,
                             0,
                             reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("alloc array [I(2)"), std::string::npos) << dump;
    EXPECT_NE(dump.find("array.store i32"), std::string::npos) << dump;
    EXPECT_NE(dump.find("array.load i32"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersMultiArrayAllocationForRegCInput)
{
    std::string reason;
    auto        fn =
        buildInlineSSA("multi",
                       "()[[I",
                       {
                           new InsnNode(Opcodes::ICONST_2),
                           new InsnNode(Opcodes::ICONST_3),
                           new MultiANewArrayInsnNode(ca::str::Utf8StringRef::from_cstr("[[I"), 2),
                           new InsnNode(Opcodes::ARETURN),
                       },
                       3,
                       0,
                       reason);
    ASSERT_NE(fn, nullptr) << "build failed: " << reason;

    std::string dump = dumpSSA(*fn);
    EXPECT_NE(dump.find("alloc multiarray [[I(2, 3)"), std::string::npos) << dump;
}

TEST(SSABuilderE2E, LowersObjectArrayDescriptorForRegCInput)
{
    std::string reason;
    auto        objectArray =
        buildInlineSSA("strings",
                       "()[Ljava/lang/String;",
                       {
                           new InsnNode(Opcodes::ICONST_2),
                           new TypeInsnNode(Opcodes::ANEWARRAY,
                                            ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                           new InsnNode(Opcodes::ARETURN),
                       },
                       2,
                       0,
                       reason);
    ASSERT_NE(objectArray, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*objectArray).find("alloc array [Ljava/lang/String;(2)"), std::string::npos);

    reason.clear();
    auto nestedArray = buildInlineSSA(
        "intArrays",
        "()[[I",
        {
            new InsnNode(Opcodes::ICONST_2),
            new TypeInsnNode(Opcodes::ANEWARRAY, ca::str::Utf8StringRef::from_cstr("[I")),
            new InsnNode(Opcodes::ARETURN),
        },
        2,
        0,
        reason);
    ASSERT_NE(nestedArray, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*nestedArray).find("alloc array [[I(2)"), std::string::npos);
}

TEST(SSABuilderE2E, HandlesPopAndWideDupStackShapes)
{
    std::string reason;
    auto        pops = buildInlineSSA("pops",
                               "()I",
                                      {
                                   new InsnNode(Opcodes::ICONST_1),
                                   new InsnNode(Opcodes::POP),
                                   new InsnNode(Opcodes::ICONST_2),
                                   new InsnNode(Opcodes::ICONST_3),
                                   new InsnNode(Opcodes::POP2),
                                   new InsnNode(Opcodes::ICONST_5),
                                   new InsnNode(Opcodes::IRETURN),
                               },
                               3,
                               0,
                               reason);
    ASSERT_NE(pops, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*pops).find("ret 5"), std::string::npos);

    reason.clear();
    auto dup2Wide = buildInlineSSA("dup2Wide",
                                   "()J",
                                   {
                                       new InsnNode(Opcodes::LCONST_1),
                                       new InsnNode(Opcodes::DUP2),
                                       new InsnNode(Opcodes::POP2),
                                       new InsnNode(Opcodes::LRETURN),
                                   },
                                   4,
                                   0,
                                   reason);
    ASSERT_NE(dup2Wide, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*dup2Wide).find("ret 1"), std::string::npos);
}

TEST(SSABuilderE2E, HandlesDupInsertionStackShapes)
{
    std::string reason;
    auto        dupX2 = buildInlineSSA("dupX2",
                                "()I",
                                       {
                                    new InsnNode(Opcodes::LCONST_0),
                                    new InsnNode(Opcodes::ICONST_5),
                                    new InsnNode(Opcodes::DUP_X2),
                                    new InsnNode(Opcodes::IRETURN),
                                },
                                4,
                                0,
                                reason);
    ASSERT_NE(dupX2, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*dupX2).find("ret 5"), std::string::npos);

    reason.clear();
    auto dup2X1 = buildInlineSSA("dup2X1",
                                 "()I",
                                 {
                                     new InsnNode(Opcodes::ICONST_5),
                                     new InsnNode(Opcodes::LCONST_1),
                                     new InsnNode(Opcodes::DUP2_X1),
                                     new InsnNode(Opcodes::POP2),
                                     new InsnNode(Opcodes::IRETURN),
                                 },
                                 5,
                                 0,
                                 reason);
    ASSERT_NE(dup2X1, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*dup2X1).find("ret 5"), std::string::npos);

    reason.clear();
    auto dup2X2 = buildInlineSSA("dup2X2",
                                 "()I",
                                 {
                                     new InsnNode(Opcodes::ICONST_1),
                                     new InsnNode(Opcodes::ICONST_2),
                                     new InsnNode(Opcodes::ICONST_3),
                                     new InsnNode(Opcodes::ICONST_4),
                                     new InsnNode(Opcodes::DUP2_X2),
                                     new InsnNode(Opcodes::IRETURN),
                                 },
                                 6,
                                 0,
                                 reason);
    ASSERT_NE(dup2X2, nullptr) << "build failed: " << reason;
    EXPECT_NE(dumpSSA(*dup2X2).find("ret 4"), std::string::npos);
}

TEST(SSABuilderE2E, LowersReferenceChecksAndMonitorIntrinsics)
{
    std::string reason;
    auto        checked = buildInlineSSA(
        "checks",
        "(Ljava/lang/Object;)I",
        {
            new VarInsnNode(Opcodes::ALOAD, 0),
            new TypeInsnNode(Opcodes::CHECKCAST,
                             ca::str::Utf8StringRef::from_cstr("java/lang/String")),
            new TypeInsnNode(Opcodes::INSTANCEOF,
                             ca::str::Utf8StringRef::from_cstr("java/lang/CharSequence")),
            new InsnNode(Opcodes::IRETURN),
        },
        1,
        1,
        reason);
    ASSERT_NE(checked, nullptr) << "build failed: " << reason;

    std::string checkDump = dumpSSA(*checked);
    EXPECT_NE(checkDump.find("checkcast java/lang/String"), std::string::npos) << checkDump;
    EXPECT_NE(checkDump.find("intrinsic instanceof:java/lang/CharSequence"), std::string::npos)
        << checkDump;

    reason.clear();
    auto monitored = buildInlineSSA("monitor",
                                    "(Ljava/lang/Object;)V",
                                    {
                                        new VarInsnNode(Opcodes::ALOAD, 0),
                                        new InsnNode(Opcodes::MONITORENTER),
                                        new VarInsnNode(Opcodes::ALOAD, 0),
                                        new InsnNode(Opcodes::MONITOREXIT),
                                        new InsnNode(Opcodes::RETURN),
                                    },
                                    1,
                                    1,
                                    reason);
    ASSERT_NE(monitored, nullptr) << "build failed: " << reason;

    std::string monitorDump = dumpSSA(*monitored);
    EXPECT_NE(monitorDump.find("intrinsic monitor.enter(param0)"), std::string::npos)
        << monitorDump;
    EXPECT_NE(monitorDump.find("intrinsic monitor.exit(param0)"), std::string::npos) << monitorDump;
}

TEST(SSABuilderJavaSamples, TinyArithmeticControlFlowFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "tiny_arithmetic", "ssa_cases/SsaTinyArithmetic.java", "SsaTinyArithmetic");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       addMul = findMethod(cn.get(), "addMul", "(II)I");
    ASSERT_NE(addMul, nullptr);
    auto addFn = buildSSAFor(cn.get(), addMul, arena, reason);
    ASSERT_NE(addFn, nullptr) << reason;
    std::string addDump = dumpSSA(*addFn);
    EXPECT_NE(addDump.find("+"), std::string::npos) << addDump;
    EXPECT_NE(addDump.find("*"), std::string::npos) << addDump;
    EXPECT_NE(addDump.find("&"), std::string::npos) << addDump;
    EXPECT_EQ(addDump.find("cstack"), std::string::npos) << addDump;

    reason.clear();
    auto* wideMix = findMethod(cn.get(), "wideMix", "(JI)J");
    ASSERT_NE(wideMix, nullptr);
    auto wideFn = buildSSAFor(cn.get(), wideMix, arena, reason);
    ASSERT_NE(wideFn, nullptr) << reason;
    std::string wideDump = dumpSSA(*wideFn);
    EXPECT_NE(wideDump.find("<<"), std::string::npos) << wideDump;
    EXPECT_EQ(wideFn->symbol.returnType, SSAType::I64);

    reason.clear();
    auto* castCompare = findMethod(cn.get(), "castCompare", "(D)I");
    ASSERT_NE(castCompare, nullptr);
    auto castFn = buildSSAFor(cn.get(), castCompare, arena, reason);
    ASSERT_NE(castFn, nullptr) << reason;
    std::string castDump = dumpSSA(*castFn);
    EXPECT_NE(castDump.find("convert param0 -> i32"), std::string::npos) << castDump;
    EXPECT_NE(castDump.find("cmp "), std::string::npos) << castDump;

    reason.clear();
    auto* loopSum = findMethod(cn.get(), "loopSum", "(I)I");
    ASSERT_NE(loopSum, nullptr);
    auto loopFn = buildSSAFor(cn.get(), loopSum, arena, reason);
    ASSERT_NE(loopFn, nullptr) << reason;
    bool foundLoopArgs = false;
    for (const auto& block : loopFn->blocks) {
        if (block->args.size() >= 2) foundLoopArgs = true;
    }
    EXPECT_TRUE(foundLoopArgs) << dumpSSA(*loopFn);

    reason.clear();
    auto* choose = findMethod(cn.get(), "choose", "(I)I");
    ASSERT_NE(choose, nullptr);
    auto switchFn = buildSSAFor(cn.get(), choose, arena, reason);
    ASSERT_NE(switchFn, nullptr) << reason;
    std::string switchDump = dumpSSA(*switchFn);
    EXPECT_NE(switchDump.find("switch"), std::string::npos) << switchDump;
    EXPECT_NE(switchDump.find("default"), std::string::npos) << switchDump;
}

TEST(SSABuilderJavaSamples, ObjectsArraysAndReferenceOpsFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "object_array", "ssa_cases/SsaObjectArrayCases.java", "SsaObjectArrayCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       fieldAndCall = findMethod(cn.get(), "fieldAndCall", "(I)I");
    ASSERT_NE(fieldAndCall, nullptr);
    auto fieldFn = buildSSAFor(cn.get(), fieldAndCall, arena, reason);
    ASSERT_NE(fieldFn, nullptr) << reason;
    std::string fieldDump = dumpSSA(*fieldFn);
    EXPECT_NE(fieldDump.find("alloc object ssa_cases/SsaObjectArrayCases()"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("call ssa_cases/SsaObjectArrayCases.<init>(I)V"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("getfield ssa_cases/SsaObjectArrayCases.value:I"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("putstatic ssa_cases/SsaObjectArrayCases.sink:I"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("getstatic ssa_cases/SsaObjectArrayCases.sink:I"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("call ssa_cases/SsaObjectArrayCases.helper(II)I"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("call ssa_cases/SsaObjectArrayCases.bump(I)I"), std::string::npos)
        << fieldDump;

    reason.clear();
    auto* arrays = findMethod(cn.get(), "arrays", "(I)I");
    ASSERT_NE(arrays, nullptr);
    auto arrayFn = buildSSAFor(cn.get(), arrays, arena, reason);
    ASSERT_NE(arrayFn, nullptr) << reason;
    std::string arrayDump = dumpSSA(*arrayFn);
    EXPECT_NE(arrayDump.find("alloc array [I(3)"), std::string::npos) << arrayDump;
    EXPECT_NE(arrayDump.find("array.store i32"), std::string::npos) << arrayDump;
    EXPECT_NE(arrayDump.find("array.load i32"), std::string::npos) << arrayDump;
    EXPECT_NE(arrayDump.find("intrinsic array.length"), std::string::npos) << arrayDump;

    reason.clear();
    auto* refOps = findMethod(cn.get(), "refOps", "(Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(refOps, nullptr);
    auto refFn = buildSSAFor(cn.get(), refOps, arena, reason);
    ASSERT_NE(refFn, nullptr) << reason;
    std::string refDump = dumpSSA(*refFn);
    EXPECT_NE(refDump.find("intrinsic instanceof:java/lang/String"), std::string::npos) << refDump;
    EXPECT_NE(refDump.find("checkcast java/lang/String"), std::string::npos) << refDump;

    reason.clear();
    auto* literal = findMethod(cn.get(), "literal", "()Ljava/lang/String;");
    ASSERT_NE(literal, nullptr);
    auto literalFn = buildSSAFor(cn.get(), literal, arena, reason);
    ASSERT_NE(literalFn, nullptr) << reason;
    // The opaque tag now embeds the literal content ("ssa-case") so the String
    // intrinsic path can recover it without re-parsing bytecode.
    EXPECT_NE(dumpSSA(*literalFn).find("<opaque:ldc_string:ssa-case>"), std::string::npos);
}

TEST(SSABuilderJavaSamples, NarrowingConversionsFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "narrowing", "ssa_cases/SsaNarrowingCases.java", "SsaNarrowingCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       narrowMath = findMethod(cn.get(), "narrowMath", "(I)I");
    ASSERT_NE(narrowMath, nullptr);
    auto mathFn = buildSSAFor(cn.get(), narrowMath, arena, reason);
    ASSERT_NE(mathFn, nullptr) << reason;
    std::string mathDump = dumpSSA(*mathFn);
    EXPECT_NE(mathDump.find("convert"), std::string::npos) << mathDump;
    EXPECT_NE(mathDump.find("-> i8"), std::string::npos) << mathDump;
    EXPECT_NE(mathDump.find("-> i16"), std::string::npos) << mathDump;
    EXPECT_NE(mathDump.find("-> char"), std::string::npos) << mathDump;

    reason.clear();
    auto* narrowFields = findMethod(cn.get(), "narrowFields", "(Lssa_cases/SsaNarrowingCases;I)I");
    ASSERT_NE(narrowFields, nullptr);
    auto fieldFn = buildSSAFor(cn.get(), narrowFields, arena, reason);
    ASSERT_NE(fieldFn, nullptr) << reason;
    std::string fieldDump = dumpSSA(*fieldFn);
    EXPECT_NE(fieldDump.find("putfield ssa_cases/SsaNarrowingCases.b:B"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("putfield ssa_cases/SsaNarrowingCases.s:S"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("putfield ssa_cases/SsaNarrowingCases.c:C"), std::string::npos)
        << fieldDump;
}

TEST(SSABuilderJavaSamples, StackShapeBytecodeFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "stack_shape", "ssa_cases/SsaStackShapeCases.java", "SsaStackShapeCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       discardCategory1 = findMethod(cn.get(), "discardCategory1", "(I)I");
    ASSERT_NE(discardCategory1, nullptr);
    auto popFn = buildSSAFor(cn.get(), discardCategory1, arena, reason);
    ASSERT_NE(popFn, nullptr) << reason;
    std::string popDump = dumpSSA(*popFn);
    EXPECT_NE(popDump.find("call ssa_cases/SsaStackShapeCases.returnsObject(I)Ljava/lang/String;"),
              std::string::npos)
        << popDump;
    EXPECT_EQ(popDump.find("cstack"), std::string::npos) << popDump;

    reason.clear();
    auto* discardCategory2 = findMethod(cn.get(), "discardCategory2", "(I)I");
    ASSERT_NE(discardCategory2, nullptr);
    auto pop2Fn = buildSSAFor(cn.get(), discardCategory2, arena, reason);
    ASSERT_NE(pop2Fn, nullptr) << reason;
    std::string pop2Dump = dumpSSA(*pop2Fn);
    EXPECT_NE(pop2Dump.find("call ssa_cases/SsaStackShapeCases.returnsLong(J)J"), std::string::npos)
        << pop2Dump;

    reason.clear();
    auto* constructObject = findMethod(cn.get(), "constructObject", "()Ljava/lang/Object;");
    ASSERT_NE(constructObject, nullptr);
    auto constructFn = buildSSAFor(cn.get(), constructObject, arena, reason);
    ASSERT_NE(constructFn, nullptr) << reason;
    std::string constructDump = dumpSSA(*constructFn);
    EXPECT_NE(constructDump.find("alloc object java/lang/Object()"), std::string::npos)
        << constructDump;
    EXPECT_NE(constructDump.find("call java/lang/Object.<init>()V"), std::string::npos)
        << constructDump;
}

TEST(SSABuilderJavaSamples, StackJoinTernaryFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "stack_join", "ssa_cases/SsaStackJoinCases.java", "SsaStackJoinCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       booleanTernary = findMethod(cn.get(), "booleanTernary", "(Z)I");
    ASSERT_NE(booleanTernary, nullptr);
    auto boolFn = buildSSAFor(cn.get(), booleanTernary, arena, reason);
    ASSERT_NE(boolFn, nullptr) << reason;
    std::string boolDump = dumpSSA(*boolFn);
    EXPECT_NE(boolDump.find("@slot-1000"), std::string::npos) << boolDump;
    EXPECT_EQ(boolDump.find("cstack"), std::string::npos) << boolDump;

    reason.clear();
    auto* intTernary = findMethod(cn.get(), "intTernary", "(I)I");
    ASSERT_NE(intTernary, nullptr);
    auto intFn = buildSSAFor(cn.get(), intTernary, arena, reason);
    ASSERT_NE(intFn, nullptr) << reason;
    std::string intDump = dumpSSA(*intFn);
    EXPECT_NE(intDump.find("@slot-1000"), std::string::npos) << intDump;
    EXPECT_EQ(intDump.find("cstack"), std::string::npos) << intDump;

    reason.clear();
    auto* referenceTernary = findMethod(
        cn.get(), "referenceTernary", "(ZLjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    ASSERT_NE(referenceTernary, nullptr);
    auto refFn = buildSSAFor(cn.get(), referenceTernary, arena, reason);
    ASSERT_NE(refFn, nullptr) << reason;
    std::string refDump = dumpSSA(*refFn);
    EXPECT_NE(refDump.find("@slot-1000"), std::string::npos) << refDump;
    EXPECT_EQ(refDump.find("cstack"), std::string::npos) << refDump;
}

TEST(SSABuilderJavaSamples, NullTrapHandlersFromJavaSource)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "null_trap", "ssa_cases/SsaNullTrapCases.java", "SsaNullTrapCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    std::string reason;
    auto*       catchNullField = findMethod(cn.get(), "catchNullField", "()I");
    ASSERT_NE(catchNullField, nullptr);
    auto fieldFn = buildSSAFor(cn.get(), catchNullField, arena, reason);
    ASSERT_NE(fieldFn, nullptr) << reason;
    std::string fieldDump = dumpSSA(*fieldFn);
    EXPECT_NE(fieldDump.find("getfield ssa_cases/SsaNullTrapCases.value:I"), std::string::npos)
        << fieldDump;
    EXPECT_NE(fieldDump.find("@slot-1"), std::string::npos) << fieldDump;

    reason.clear();
    auto* catchNullCall = findMethod(cn.get(), "catchNullCall", "()I");
    ASSERT_NE(catchNullCall, nullptr);
    auto callFn = buildSSAFor(cn.get(), catchNullCall, arena, reason);
    ASSERT_NE(callFn, nullptr) << reason;
    std::string callDump = dumpSSA(*callFn);
    EXPECT_NE(callDump.find("call ssa_cases/SsaNullTrapCases.id(I)I"), std::string::npos)
        << callDump;
    EXPECT_NE(callDump.find("@slot-1"), std::string::npos) << callDump;

    reason.clear();
    auto* catchNullArray = findMethod(cn.get(), "catchNullArray", "()I");
    ASSERT_NE(catchNullArray, nullptr);
    auto arrayFn = buildSSAFor(cn.get(), catchNullArray, arena, reason);
    ASSERT_NE(arrayFn, nullptr) << reason;
    std::string arrayDump = dumpSSA(*arrayFn);
    EXPECT_NE(arrayDump.find("array.load i32 null[0]"), std::string::npos) << arrayDump;
    EXPECT_NE(arrayDump.find("@slot-1"), std::string::npos) << arrayDump;
}

// 异常保护区精确性（稀疏异常加固）：保护区判定改用指令序号区间
// （ExceptionHandler::startInsnIdx/endInsnIdx ∩ Block 指令范围），与 L1
// label 切分解耦，不再依赖"块序 == 字节码序"的 label 区间扫描。断言：
//   1. handler 的 start/end 指令序号已解析且范围非空；
//   2. SSA region 的 protectedBlocks 集合 == { L1 块 | 块指令范围与保护区相交 }
//      —— semiCatch：try 前 x*2 块、try 后 return 块不在保护区；semiAfter：
//      try 后的 x/0 块不在保护区（JVM 语义：try 外异常必须传播）。
TEST(SSABuilderJavaSamples, ExceptionPrecisionRanges)
{
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME to enable Java-source SSA samples";
    }

    fs::path classFile = compileJavaSample(
        javac, "exc_precision", "ssa_cases/SsaExceptionPrecisionCases.java",
        "SsaExceptionPrecisionCases");
    ASSERT_TRUE(fileExists(classFile)) << classFile;

    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    auto                cn = loadClassFile(arena, classFile, bytes);
    ASSERT_NE(cn, nullptr);

    auto checkRegion = [&](const char* name, const char* desc) {
        auto* m = findMethod(cn.get(), name, desc);
        ASSERT_NE(m, nullptr);
        std::string reason;
        CodeConverter converter(arena);
        auto          l1 = converter.convert(*m);
        ASSERT_FALSE(l1.empty());
        ASSERT_FALSE(l1.front()->exceptionTable.empty()) << name << " should have try/catch";
        const auto& h = l1.front()->exceptionTable[0];
        // 1) start/end 指令序号已解析（-1 = 未回填，判定会退化）。
        EXPECT_GE(h.startInsnIdx, 0) << name;
        EXPECT_GT(h.endInsnIdx, h.startInsnIdx) << name;
        // 2) 预期保护区集合：块指令范围与 [start, end) 相交。
        std::vector<bool> expectProtected(l1.size(), false);
        for (size_t i = 0; i < l1.size(); ++i) {
            auto* b = l1[i];
            expectProtected[i] = b->firstInsnIdx >= 0 && b->lastInsnIdx >= 0 &&
                                 b->firstInsnIdx < h.endInsnIdx &&
                                 b->lastInsnIdx >= h.startInsnIdx;
        }
        // 3) SSA region protectedBlocks 与预期逐块对应（L1 与 SSA 块 1:1，
        //    通过 l1LabelId 关联）。
        SSABuilder builder;
        bool       isStatic = (m->access & Opcodes::ACC_STATIC) != 0;
        auto fn = builder.build(l1, toStr(cn->name), name, desc, isStatic, m->maxLocals, arena,
                                reason);
        ASSERT_NE(fn, nullptr) << reason;
        ASSERT_EQ(fn->exceptionRegions.size(), 1u) << name;
        for (size_t i = 0; i < l1.size(); ++i) {
            const int  l1Id    = l1[i]->id;
            bool       inRegion = false;
            for (const auto* pb : fn->exceptionRegions[0].protectedBlocks) {
                if (pb->l1LabelId == l1Id) {
                    inRegion = true;
                    break;
                }
            }
            EXPECT_EQ(inRegion, expectProtected[i])
                << name << " L1 block " << i << " (id=" << l1Id << ", insns ["
                << l1[i]->firstInsnIdx << "," << l1[i]->lastInsnIdx << "]) protected="
                << inRegion << " expected=" << expectProtected[i];
        }
    };
    checkRegion("semiCatch", "(I)I");
    checkRegion("semiAfter", "(I)I");
}

// 临时 dump 查看（人工检查 loopSum 的 phi 形态）。跑一次后可 DISABLED。
TEST(SSABuilderE2E, DISABLED_DumpLoopSumSsa)
{
    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    ClassNode*          cn = loadSamples(arena, bytes);
    auto*               m  = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);
    std::string reason;
    auto        fn = buildSSAFor(cn, m, arena, reason);
    ASSERT_NE(fn, nullptr);
    printf("\n=== loopSum SSA dump ===\n%s\n", dumpSSA(*fn).c_str());
}

TEST(SSABuilderE2E, DISABLED_DumpLoopSumBlocks)
{
    Utf8StringArena     arena;
    std::vector<ca::u8> bytes;
    ClassNode*          cn = loadSamples(arena, bytes);
    auto*               m  = findMethod(cn, "loopSum", "(I)I");
    ASSERT_NE(m, nullptr);
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*m);
    printf("\n=== loopSum L1 blocks (%zu) ===\n", blocks.size());
    printf("%s\n", CodeConverter::dumpMIR(blocks).c_str());
}

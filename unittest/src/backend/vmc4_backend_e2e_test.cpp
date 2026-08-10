// VMC4 native E2E harness: Java source -> javac -> Vmc4Backend -> .c -> DLL
// -> Java loads the DLL and calls the protected native methods.
//
// 与 regc_backend_e2e_test.cpp 的差别：
//   - 后端是 Vmc4Backend（RegIR 之后分叉到 VmEncoder + 解释器发射）。
//   - 生成的 C 完全自包含（runtime prelude 内联），编译只需 jni.h，
//     不需要 mj2x-runtime / mj2x-crt 目标文件。
//   - 在 4 种 MSVC 可编译的 dispatcher 形态下各跑一轮 round-trip
//     （ComputedGoto 依赖 GCC Labels-as-Values，MSVC 编译失败，e2e 排除）。
//
// 本测试验证：生成的 C 真能编译成 DLL、经 RegisterNatives 绑定后
// Java 调用结果与原始字节码基线完全一致（Java -> VM -> native -> Java 闭环）。

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / JdkToolchain / probeJdk

namespace {

// mj2x VMC4 E2E 工具链：JDK（javac/java/jni.h）+ vcvars64（MSVC）。
struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    jdkInclude;
    fs::path    jdkIncludeWin;
    fs::path    vcvars64;
};

struct JavaCheck
{
    std::string label;
    std::string expression;
};

struct DispatcherParam
{
    mj2x::VmDispatcherKind kind;
    const char*            tag;   // 用例目录后缀 + 失败信息
};

Toolchain probeToolchain()
{
    Toolchain tc;
    ProbeOptions opts;
    opts.requireJniHeader = true;
    auto jdk = probeJdk(opts);
    if (!jdk.available) {
        tc.reason = jdk.reason;
        return tc;
    }
    tc.javac         = jdk.javac;
    tc.java          = jdk.java;
    tc.jdkInclude    = jdk.jdkInclude;
    tc.jdkIncludeWin = jdk.jdkIncludeWin;

    tc.vcvars64 = jdk_finder::findVcvarsAll();
    if (tc.vcvars64.empty()) {
        tc.reason = "vcvars64.bat not found; MSVC is required";
        return tc;
    }
    tc.available = true;
    return tc;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string              current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        }
        else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines) {
        out += "  ";
        out += line;
        out += "\n";
    }
    return out;
}

std::string readText(const fs::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

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

class Vmc4BackendE2E : public ::testing::TestWithParam<DispatcherParam>
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/vmc4_e2e/" + name + "/.keep").parent_path();
    }

    /// 跑一个 Java 源文件 + 基线/保护双跑对比。
    ///
    /// @param expectedNativeMethods 转成 VM 的方法数（结构断言：每个方法在生成
    ///        C 里出现 2 次 mj2x_vm_exec_<tag>：解释器定义 + JNI wrapper 调用）。
    void runCase(const std::string& className, const std::string& javaSource,
                 const std::vector<JavaCheck>& checks, int expectedNativeMethods,
                 mj2x::VmDispatcherKind dispatcherKind)
    {
        const std::string suffix = std::string("_") + GetParam().tag;
        fs::path          dir    = caseDir(className + suffix);
        std::error_code   ec;
        fs::create_directories(dir, ec);

        fs::path javaFile = dir / (className + ".java");
        {
            std::ofstream out(javaFile);
            out << javaSource;
        }

        auto javac = runBat({q(tc_.javac) + " -d " + q(dir) + " " + q(javaFile)});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        fs::path driverFile = dir / "Driver.java";
        {
            std::ofstream out(driverFile);
            out << buildDriver(checks);
        }
        auto driverCompile =
            runBat({q(tc_.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
        ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

        auto baselineRun = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
        ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
        auto baseline = splitLines(baselineRun.out);

        fs::path inClass  = dir / (className + ".class");
        fs::path outClass = dir / (className + "_protected.class");
        fs::path cFile    = dir / (className + ".c");

        mj2x::PipelineConfig config;
        config.inputPath  = inClass.string();
        config.outputPath = outClass.string();
        mj2x::Pipeline pipeline(std::move(config));
        auto           backend = std::make_unique<mj2x::Vmc4Backend>();
        backend->setOutputPath(cFile.string());
        mj2x::Vmc4Options opts;
        opts.dispatcherKind = dispatcherKind;
        backend->setOptions(opts);
        pipeline.registerBackend(std::move(backend));
        // VMC4 转 VM 需要方法被标记（MarkAll 跳过 <init>/<clinit>，与 RegC 相同）。
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
        // RegisterNatives 动态绑定需要 mj2x/cloader/Loader 桥接类。
        pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
        ASSERT_EQ(pipeline.run(), 0);
        ASSERT_TRUE(fileExists(cFile));
        std::string generatedC = readText(cFile);
        // 结构断言：prelude + 解释器 + RegisterNatives 都存在。
        EXPECT_NE(generatedC.find("#include <jni.h>"), std::string::npos) << generatedC;
        EXPECT_NE(generatedC.find("mj2x_vm_slot"), std::string::npos) << generatedC;
        EXPECT_NE(generatedC.find("mj2x_vm_div_i32"), std::string::npos) << generatedC;
        EXPECT_NE(generatedC.find("JNI_OnLoad"), std::string::npos) << generatedC;
        // VMC4 不依赖 RegC CRT helper。
        EXPECT_EQ(generatedC.find("_mj2x_regc_"), std::string::npos)
            << "VMC4 generated C must not reference RegC CRT helpers\n"
            << generatedC;
        // 每个转 VM 的方法恰好 2 处 mj2x_vm_exec_<tag>（解释器定义 + wrapper 调用）。
        EXPECT_EQ(countOccurrences(generatedC, "mj2x_vm_exec_"),
                  static_cast<size_t>(expectedNativeMethods) * 2)
            << "expected " << expectedNativeMethods << " virtualized method(s)\n"
            << generatedC;

        fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        // 编译生成的 C：自包含，只需 jni.h（/TC 显式 C 模式）。
        fs::path dll = dir / (className + ".dll");
        fs::path obj = dir / (className + ".obj");
        std::vector<std::string> compileLines;
        compileLines.push_back("call " + q(tc_.vcvars64) + " x64 >nul");
        compileLines.push_back("cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(cFile) +
                               " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin));
        compileLines.push_back("link /nologo /DLL /OUT:" + q(dll) + " " + q(obj));
        auto cl = runBat(compileLines);
        ASSERT_EQ(cl.exitCode, 0) << cl.out;
        ASSERT_TRUE(fileExists(dll));

        auto protectedRun = runBat({
            "set \"MORPHER_DLL=" + dll.string() + "\"",
            q(tc_.java) + " -cp " + q(dir) + " Driver",
        });
        ASSERT_EQ(protectedRun.exitCode, 0) << protectedRun.out;
        auto actual = splitLines(protectedRun.out);

        ASSERT_EQ(baseline.size(), actual.size()) << "baseline:\n"
                                                  << joinLines(baseline) << "protected:\n"
                                                  << joinLines(actual);
        for (size_t i = 0; i < baseline.size(); ++i) {
            EXPECT_EQ(baseline[i], actual[i]) << "line " << i;
        }
    }

private:
    static std::string javaString(const std::string& text)
    {
        std::ostringstream out;
        out << "\"";
        for (char c : text) {
            switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
            }
        }
        out << "\"";
        return out.str();
    }

    static std::string buildDriver(const std::vector<JavaCheck>& checks)
    {
        std::ostringstream out;
        out << "public class Driver {\n";
        out << "  public static void main(String[] args) throws Exception {\n";
        out << "    String dll = System.getenv(\"MORPHER_DLL\");\n";
        out << "    if (dll != null) System.load(dll);\n";
        for (const auto& check : checks) {
            out << "    try {\n";
            out << "      System.out.println(" << javaString(check.label + "=") << " + "
                << check.expression << ");\n";
            out << "    } catch (Throwable t) {\n";
            out << "      System.out.println(" << javaString(check.label + "=THROW:") << " + "
                << "t.getClass().getName());\n";
            out << "    }\n";
        }
        out << "  }\n";
        out << "}\n";
        return out.str();
    }
};

Toolchain Vmc4BackendE2E::tc_;

INSTANTIATE_TEST_SUITE_P(
    Vmc4Dispatchers, Vmc4BackendE2E,
    ::testing::Values(DispatcherParam{mj2x::VmDispatcherKind::SwitchTable, "switch_table"},
                      DispatcherParam{mj2x::VmDispatcherKind::FuncPtrTable, "funcptr_table"},
                      DispatcherParam{mj2x::VmDispatcherKind::IfNest, "if_nest"},
                      DispatcherParam{mj2x::VmDispatcherKind::StateDispatcher, "state_dispatcher"}));

// 纯计算 + 控制流：int/long 算术、环绕、除/模、移位、比较、分支、循环。
// 注意：除零在 MVP 不抛异常（wrapper 忽略 status 直接返回计算值），故只测
// INT_MIN/-1 这类 JVM 不抛的除法路径。
TEST_P(Vmc4BackendE2E, PureArithmeticAndControlFlowRoundTrip)
{
    runCase("Vmc4NativePure",
            "public class Vmc4NativePure {\n"
            "  public static int addMul(int a, int b) { return (a + b) * 3; }\n"
            "  public static int loopSum(int n) {\n"
            "    int acc = 0;\n"
            "    for (int i = 0; i < n; i++) acc += i;\n"
            "    return acc;\n"
            "  }\n"
            "  public static long longChain(long a, long b) { return (a * b) + (a / b) + (a % "
            "b); }\n"
            "  public static int divTraps(int a, int b) { return (a / b) + (a % b); }\n"
            "  public static int ushrDemo(int a) { return a >>> 3; }\n"
            "  public static int shifts(int a) { return (a << 2) | (a >> 2); }\n"
            "  public static boolean cmp(int a, int b) { return a < b; }\n"
            "  public static int pick(int a, int b) { return a > b ? a - b : b - a; }\n"
            "  public static int nested(int a, int b, int c) {\n"
            "    if (a > b) { if (b > c) return a; return b; }\n"
            "    return c;\n"
            "  }\n"
            "}\n",
            {
                {"addMul(3, 4)", "Vmc4NativePure.addMul(3, 4)"},
                {"addMulWrap(2147483647, 1)", "Vmc4NativePure.addMul(2147483647, 1)"},
                {"loopSum(0)", "Vmc4NativePure.loopSum(0)"},
                {"loopSum(7)", "Vmc4NativePure.loopSum(7)"},
                {"loopSum(100)", "Vmc4NativePure.loopSum(100)"},
                {"longChain(10, 3)", "Vmc4NativePure.longChain(10L, 3L)"},
                {"longChain(-7, 2)", "Vmc4NativePure.longChain(-7L, 2L)"},
                {"longChainWrap(9223372036854775807L, 1)",
                 "Vmc4NativePure.longChain(9223372036854775807L, 1L)"},
                {"divTraps(10, 4)", "Vmc4NativePure.divTraps(10, 4)"},
                {"divTrapsMin(INT_MIN, -1)", "Vmc4NativePure.divTraps(-2147483648, -1)"},
                {"ushr(-8)", "Vmc4NativePure.ushrDemo(-8)"},
                {"ushr(7)", "Vmc4NativePure.ushrDemo(7)"},
                {"shifts(3)", "Vmc4NativePure.shifts(3)"},
                {"shifts(-1)", "Vmc4NativePure.shifts(-1)"},
                {"cmp(3, 5)", "Vmc4NativePure.cmp(3, 5)"},
                {"cmp(5, 3)", "Vmc4NativePure.cmp(5, 3)"},
                {"pick(10, 3)", "Vmc4NativePure.pick(10, 3)"},
                {"pick(3, 10)", "Vmc4NativePure.pick(3, 10)"},
                {"nested(5, 3, 4)", "Vmc4NativePure.nested(5, 3, 4)"},
                {"nested(2, 4, 1)", "Vmc4NativePure.nested(2, 4, 1)"},
                {"nested(1, 1, 2)", "Vmc4NativePure.nested(1, 1, 2)"},
            },
            9, GetParam().kind);
}

// 浮点路径：double 全精度一致；float 只用可精确表示的值（MVP 的 F32 handler
// 内部用 double 计算、仅在 JNI 边界转回 float，多步 float 链会与 Java 的
// 每步 binary32 舍入不同——已知偏差，见 vmc4_multi_vm_design.md §11）。
// f2i/f2l 饱和语义（NaN→0、超界→min/max）与 JVM 一致。
TEST_P(Vmc4BackendE2E, FloatDoubleAndConvertRoundTrip)
{
    runCase("Vmc4NativeFloat",
            "public class Vmc4NativeFloat {\n"
            "  public static double dmath(double a, double b) { return a / b + a * b - (a - b); "
            "}\n"
            "  public static float fadd(float a, float b) { return a + b; }\n"
            "  public static float fmul(float a, float b) { return a * b; }\n"
            "  public static float fdiv(float a, float b) { return a / b; }\n"
            "  public static int remDemo(double a, double b) { return (int)(a % b); }\n"
            "  public static int f2i(double v) { return (int)v; }\n"
            "  public static long f2l(double v) { return (long)v; }\n"
            "  public static int l2i(long v) { return (int)v; }\n"
            "  public static double i2d(int v) { return (double)v; }\n"
            "  public static double f2d(float v) { return (double)v; }\n"
            "  public static float i2f(int v) { return (float)v; }\n"
            "}\n",
            {
                {"dmath(5.0, 2.0)", "Vmc4NativeFloat.dmath(5.0, 2.0)"},
                {"dmath(1e300, 1.5)", "Vmc4NativeFloat.dmath(1e300, 1.5)"},
                {"dmath(-3.25, 0.5)", "Vmc4NativeFloat.dmath(-3.25, 0.5)"},
                {"fadd(3.5f, 2.25f)", "Vmc4NativeFloat.fadd(3.5f, 2.25f)"},
                {"fmul(3.5f, 2.0f)", "Vmc4NativeFloat.fmul(3.5f, 2.0f)"},
                {"fdiv(8.0f, 2.0f)", "Vmc4NativeFloat.fdiv(8.0f, 2.0f)"},
                {"remDemo(10.5, 3.0)", "Vmc4NativeFloat.remDemo(10.5, 3.0)"},
                {"remDemo(-10.5, 3.0)", "Vmc4NativeFloat.remDemo(-10.5, 3.0)"},
                {"f2i(3.9)", "Vmc4NativeFloat.f2i(3.9)"},
                {"f2i(-3.9)", "Vmc4NativeFloat.f2i(-3.9)"},
                {"f2i(NaN)", "Vmc4NativeFloat.f2i(Double.NaN)"},
                {"f2i(2^31)", "Vmc4NativeFloat.f2i(2.147483648e9)"},
                {"f2i(-2^31-1)", "Vmc4NativeFloat.f2i(-2.147483649e9)"},
                {"f2l(3.9)", "Vmc4NativeFloat.f2l(3.9)"},
                {"f2l(1e20)", "Vmc4NativeFloat.f2l(1e20)"},
                {"f2l(-1e20)", "Vmc4NativeFloat.f2l(-1e20)"},
                {"f2l(NaN)", "Vmc4NativeFloat.f2l(Double.NaN)"},
                {"l2i(2^32+1)", "Vmc4NativeFloat.l2i(0x100000001L)"},
                {"l2i(Long.MAX)", "Vmc4NativeFloat.l2i(9223372036854775807L)"},
                {"i2d(7)", "Vmc4NativeFloat.i2d(7)"},
                {"f2d(1.5f)", "Vmc4NativeFloat.f2d(1.5f)"},
                {"i2f(3)", "Vmc4NativeFloat.i2f(3)"},
            },
            11, GetParam().kind);
}

// fail-closed 混合：同一类里纯计算方法转 VM，含对象/switch/实例方法的方法
// 保留字节码。验证两类方法在同一类中共存并各自正确。
TEST_P(Vmc4BackendE2E, MixedNativeAndFailClosedRoundTrip)
{
    runCase("Vmc4NativeMixed",
            "public class Vmc4NativeMixed {\n"
            "  public static int pure(int x) { return x * 2 + 1; }\n"
            "  public static String echo(String s) { return s; }\n"
            "  public static int sw(int x) { switch (x) { case 0: return 1; default: return 2; } "
            "}\n"
            "  public static int useInst(int x) { return new Vmc4NativeMixed().inst(x); }\n"
            "  public int inst(int x) { return x + 1; }\n"
            "}\n",
            {
                {"pure(5)", "Vmc4NativeMixed.pure(5)"},
                {"pure(-3)", "Vmc4NativeMixed.pure(-3)"},
                {"echo(ok)", "Vmc4NativeMixed.echo(\"ok\")"},
                {"sw(0)", "Vmc4NativeMixed.sw(0)"},
                {"sw(7)", "Vmc4NativeMixed.sw(7)"},
                {"useInst(5)", "Vmc4NativeMixed.useInst(5)"},
                {"inst(3)", "new Vmc4NativeMixed().inst(3)"},
            },
            1, GetParam().kind);
}

}   // namespace

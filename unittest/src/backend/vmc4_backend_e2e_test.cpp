// VMC4 native E2E harness: Java source -> javac -> Vmc4Backend -> .c -> DLL
// -> Java loads the DLL and calls the protected native methods.
//
// 与 regc_backend_e2e_test.cpp 的差别：
//   - 后端是 Vmc4Backend（RegIR 之后分叉到 VmEncoder + 解释器发射）。
//   - 生成的 C 完全自包含（runtime prelude 内联），编译只需 jni.h，
//     不需要 mj2x-runtime / mj2x-rt 目标文件。
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
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/preprocessor/init_clinit_mover_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
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
        // 执行期 dispatch drift：所有 e2e 在 drift 全开（2 变体 + 16 字节周期）下
        // 验证语义保真——unmap 解码路径与编码器轮转同步。
        opts.driftVariants = 2;
        opts.driftPeriod   = 16;
        backend->setOptions(opts);
        pipeline.registerBackend(std::move(backend));
        // VMC4 转 VM 需要方法被标记（MarkAll 跳过 <init>/<clinit>，与 RegC 相同）。
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
        // RegisterNatives 动态绑定需要 mj2x/cloader/Loader 桥接类。
        pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
        ASSERT_EQ(pipeline.run(), 0);
        ASSERT_TRUE(fileExists(cFile));
        std::string generatedC = readText(cFile);
        // 结构断言：include + 解释器 + RegisterNatives 都存在。
        EXPECT_NE(generatedC.find("#include <jni.h>"), std::string::npos) << generatedC;
        EXPECT_NE(generatedC.find("vmc4/mj2x_vmc4_runtime.h"), std::string::npos)
            << "runtime ABI include missing (#894 运行时/模板分离)\n"
            << generatedC;
        EXPECT_NE(generatedC.find("mj2x_vm_slot"), std::string::npos) << generatedC;
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

        // 编译生成的 C：运行时 ABI 已下沉 mj2x-rt(#894),链接
        // mj2x_vmc4_runtime.c;/I 加 rt src 与 jni.h 路径(/TC 显式 C 模式)。
        fs::path dll = dir / (className + ".dll");
        fs::path obj = dir / (className + ".obj");
        fs::path crtSrc = fs::current_path() / "mj2x" / "mj2x-rt" / "src";
        fs::path runtimeObj = dir / "mj2x_vmc4_runtime.obj";
        std::vector<std::string> compileLines;
        compileLines.push_back("call " + q(tc_.vcvars64) + " x64 >nul");
        compileLines.push_back("cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(cFile) +
                               " /I" + q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                               q(tc_.jdkIncludeWin));
        compileLines.push_back("cl /nologo /O2 /TC /utf-8 /c /Fo" + q(runtimeObj) + " " +
                               q(crtSrc / "vmc4" / "mj2x_vmc4_runtime.c") + " /I" + q(crtSrc) +
                               " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin));
        compileLines.push_back(
            "link /nologo /DLL /OUT:" + q(dll) + " " + q(obj) + " " + q(runtimeObj));
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

protected:
    /// 双后端对拍（Phase 2.D）：同一 Java 方法集分别经 Vmc4Backend / RegCBackend
    /// 虚拟化，各自编译 DLL 跑 Driver，输出必须与 baseline 完全一致。
    void runDualBackendCase(const std::string& className, const std::string& javaSource,
                            const std::vector<JavaCheck>& checks,
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

        fs::path inClass = dir / (className + ".class");

        // ---- VMC4 侧 ----
        std::vector<std::string> vmc4Out;
        runProtected(className, inClass, dir, "vmc4", dispatcherKind, vmc4Out);
        // ---- RegC 侧 ----
        std::vector<std::string> regcOut;
        runProtected(className, inClass, dir, "regc", dispatcherKind, regcOut);

        for (const auto& [label, actual] : {std::pair{"vmc4", vmc4Out},
                                            std::pair{"regc", regcOut}}) {
            ASSERT_EQ(baseline.size(), actual.size())
                << label << " baseline:\n" << joinLines(baseline) << label << ":\n"
                << joinLines(actual);
            for (size_t i = 0; i < baseline.size(); ++i) {
                EXPECT_EQ(baseline[i], actual[i]) << label << " line " << i;
            }
        }
    }

private:

    /// 单后端跑一轮：pipeline（后端由 backendName 选择）→ 编译 DLL → Driver 输出。
    /// 注意：inClass 每次跑前都是原始字节码（对拍用独立输出名，不覆盖输入）。
    void runProtected(const std::string& className, const fs::path& inClass, const fs::path& dir,
                      const std::string& backendName, mj2x::VmDispatcherKind dispatcherKind,
                      std::vector<std::string>& out)
    {
        std::error_code ec;
        fs::path outClass = dir / (className + "_" + backendName + ".class");
        fs::path cFile    = dir / (className + "_" + backendName + ".c");

        mj2x::PipelineConfig config;
        config.inputPath  = inClass.string();
        config.outputPath = outClass.string();
        mj2x::Pipeline pipeline(std::move(config));
        if (backendName == "regc") {
            auto backend = std::make_unique<mj2x::RegCBackend>();
            backend->setOutputPath(cFile.string());
            pipeline.registerBackend(std::move(backend));
            pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
            pipeline.registerPreprocessor(std::make_unique<mj2x::InitClinitMoverPreprocessor>());
            pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
            pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
        } else {
            auto backend = std::make_unique<mj2x::Vmc4Backend>();
            backend->setOutputPath(cFile.string());
            mj2x::Vmc4Options opts;
            opts.dispatcherKind = dispatcherKind;
            opts.driftVariants  = 2;
            opts.driftPeriod    = 16;
            backend->setOptions(opts);
            pipeline.registerBackend(std::move(backend));
            pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
            pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
        }
        ASSERT_EQ(pipeline.run(), 0) << backendName;
        ASSERT_TRUE(fileExists(cFile)) << backendName;

        // 编译 DLL（VMC4 自包含 /TC；RegC 需 CRT + runtime 目标文件）。
        fs::path dll    = dir / (className + "_" + backendName + ".dll");
        fs::path objDir = dir / ("obj_" + backendName);
        fs::create_directories(objDir, ec);
        std::vector<std::string> compileLines;
        compileLines.push_back("call " + q(tc_.vcvars64) + " x64 >nul");
        fs::path crtSrc = fs::current_path() / "mj2x" / "mj2x-rt" / "src";
        if (backendName == "regc") {
            fs::path generatedObj = objDir / "generated.obj";
            compileLines.push_back("cl /nologo /O2 /utf-8 /c /Fo" + q(generatedObj) + " " +
                                   q(cFile) + " /I" + q(crtSrc) + " /I" + q(tc_.jdkInclude) +
                                   " /I" + q(tc_.jdkIncludeWin));
            std::vector<fs::path> objs;
            auto compileCrt = [&](const std::string& rel, const std::string& objName) {
                fs::path src = crtSrc / rel;
                fs::path obj = objDir / objName;
                objs.push_back(obj);
                compileLines.push_back("cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(src) +
                                       " /I" + q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                                       q(tc_.jdkIncludeWin));
            };
            compileCrt("regc/mj2x_regc_crt.c", "mj2x_regc_crt.obj");
            compileCrt("regc/mj2x_regc_runtime.c", "mj2x_regc_runtime.obj");
            compileCrt("regc/mj2x_regc_safe_env.c", "mj2x_regc_safe_env.obj");
            compileCrt("regc/mj2x_regc_string.c", "mj2x_regc_string.obj");
            compileCrt("regc/mj2x_regc_string_builder.c", "mj2x_regc_string_builder.obj");
            // wrapper/Math/Random intrinsic 与 RegC 同源:可分发 C99 版
            // mj2x_regc_intrinsic.c(mj2x-runtime 历史 in-tree 版已删除)。
            compileCrt("regc/mj2x_regc_intrinsic.c", "mj2x_regc_intrinsic.obj");
            std::string linkLine = "link /nologo /DLL /OUT:" + q(dll) + " " + q(generatedObj);
            for (const auto& obj : objs) linkLine += " " + q(obj);
            compileLines.push_back(linkLine);
        } else {
            fs::path generatedObj = objDir / "generated.obj";
            compileLines.push_back("cl /nologo /O2 /utf-8 /c /Fo" + q(generatedObj) + " " +
                                   q(cFile) + " /I" + q(crtSrc) + " /I" + q(tc_.jdkInclude) +
                                   " /I" + q(tc_.jdkIncludeWin));
            // VMC4 运行时 ABI(#894 下沉):解释器 helper 链接消费。
            fs::path vmc4RuntimeObj = objDir / "mj2x_vmc4_runtime.obj";
            compileLines.push_back(
                "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(vmc4RuntimeObj) + " " +
                q(crtSrc / "vmc4" / "mj2x_vmc4_runtime.c") + " /I" + q(crtSrc) + " /I" +
                q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin));
            std::string linkLine =
                "link /nologo /DLL /OUT:" + q(dll) + " " + q(generatedObj) + " " + q(vmc4RuntimeObj);
            compileLines.push_back(linkLine);
        }
        auto cl = runBat(compileLines);
        ASSERT_EQ(cl.exitCode, 0) << backendName << " compile:\n" << cl.out;
        ASSERT_TRUE(fileExists(dll)) << backendName;

        auto protectedRun = runBat({
            "set \"MORPHER_DLL=" + dll.string() + "\"",
            q(tc_.java) + " -cp " + q(dir) + " Driver",
        });
        ASSERT_EQ(protectedRun.exitCode, 0) << backendName << " run:\n" << protectedRun.out;
        out = splitLines(protectedRun.out);
    }

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

// 四后端语义回归集成员（X2）：超界/负数移位计数与字符串比较是跨后端
// 易漂移点——历史缺陷是 RegC/LLVM 缺 JVM &31/&63 掩码（StackC/VMC4 一直
// 正确）。矩阵三后端由 mtest_set g13.shift_out_of_range / g12.string_compare
// 覆盖，这里补 VMC4 列；移位计数走参数防常量折叠。
TEST_P(Vmc4BackendE2E, ShiftMaskAndStringCompareRoundTrip)
{
    runCase("Vmc4SemanticDrift",
            "public class Vmc4SemanticDrift {\n"
            "  public static int iShl(int a, int b) { return (1 << a) ^ (-1 << b) ^ (5 << 3); }\n"
            "  public static int iShr(int a, int b) { return (-1 >> a) ^ (-256 >> b) ^ (96 >> 2); }\n"
            "  public static int iUShr(int a, int b) { return (-1 >>> a) ^ (-1 >>> b) ^ (-1 >>> 31); }\n"
            "  public static long lShl(int a, int b) { return (1L << a) ^ (1L << b) ^ (1L << 63); }\n"
            "  public static long lUShr(int a, int b) { return (-1L >>> a) ^ (-1L >>> b) ^ (-1L >>> 63); }\n"
            "  public static boolean strEq(String a, String b) { return a.equals(b); }\n"
            "  public static int strCmp(String a, String b) { return a.compareTo(b); }\n"
            "  public static boolean strEqIC(String a, String b) { return a.equalsIgnoreCase(b); }\n"
            "}\n",
            {
                {"iShl(33, -1)", "Vmc4SemanticDrift.iShl(33, -1)"},
                {"iShr(32, 33)", "Vmc4SemanticDrift.iShr(32, 33)"},
                {"iUShr(32, 33)", "Vmc4SemanticDrift.iUShr(32, 33)"},
                {"lShl(64, 65)", "Vmc4SemanticDrift.lShl(64, 65)"},
                {"lUShr(64, 65)", "Vmc4SemanticDrift.lUShr(64, 65)"},
                {"iShl(3, 4)", "Vmc4SemanticDrift.iShl(3, 4)"},
                {"iShr(-256, 1)", "Vmc4SemanticDrift.iShr(-256, 1)"},
                {"lUShr(-8, 1)", "Vmc4SemanticDrift.lUShr(-8, 1)"},
                {"strEq(abc, abc)", "Vmc4SemanticDrift.strEq(\"abc\", \"abc\")"},
                {"strEq(abc, abd)", "Vmc4SemanticDrift.strEq(\"abc\", \"abd\")"},
                {"strCmp(abc, abd)", "Vmc4SemanticDrift.strCmp(\"abc\", \"abd\")"},
                {"strCmp(b, a)", "Vmc4SemanticDrift.strCmp(\"b\", \"a\")"},
                {"strEqIC(ABC, abc)", "Vmc4SemanticDrift.strEqIC(\"ABC\", \"abc\")"},
            },
            8, GetParam().kind);
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

// fail-closed 混合：同一类里纯计算方法转 VM，含对象/实例方法的方法保留字节码。
// 验证两类方法在同一类中共存并各自正确。（switch 自 Phase 3 起支持，sw 转 VM。）
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
            // pure/echo/sw/useInst/inst 五方法全部虚拟化（switch 已支持）。
            5, GetParam().kind);
}

// Phase 2.0 spike：静态调用链。viaAdd/viaLong/viaDouble 各含一条 invokestatic Call
// 指令；chain 含两条同目标调用（验证 methodRef 去重 + argBuf 复用同一 helper）。
// 7 个方法全部转 VM：叶子方法（add/addLong/addDouble）methodRefs 空（传 call_table=0），
// 调用方方法 methodRefs 非空 → 发射 per-callsite helper + mj2x_vm_call_table_<tag>，
// 运行时经 JNI CallStatic<Type>MethodA 链回叶子方法的 VM 实现。
TEST_P(Vmc4BackendE2E, StaticCallChainRoundTrip)
{
    runCase("Vmc4NativeCall",
            "public class Vmc4NativeCall {\n"
            "  public static int add(int a, int b) { return a + b; }\n"
            "  public static int viaAdd(int x, int y) { return add(x, y); }\n"
            "  public static int chain(int a, int b, int c) { return add(add(a, b), c); }\n"
            "  public static long addLong(long a, long b) { return a + b; }\n"
            "  public static long viaLong(long a, long b) { return addLong(a, b); }\n"
            "  public static double addDouble(double a, double b) { return a + b; }\n"
            "  public static double viaDouble(double a, double b) { return addDouble(a, b); }\n"
            "}\n",
            {
                {"add(7, 8)", "Vmc4NativeCall.add(7, 8)"},
                {"viaAdd(3, 4)", "Vmc4NativeCall.viaAdd(3, 4)"},
                {"viaAdd(-5, 2)", "Vmc4NativeCall.viaAdd(-5, 2)"},
                {"chain(1, 2, 3)", "Vmc4NativeCall.chain(1, 2, 3)"},
                {"chain(100, 200, 300)", "Vmc4NativeCall.chain(100, 200, 300)"},
                {"addLong(10, 20)", "Vmc4NativeCall.addLong(10L, 20L)"},
                {"viaLong(10, 20)", "Vmc4NativeCall.viaLong(10L, 20L)"},
                {"viaLong(-5, -7)", "Vmc4NativeCall.viaLong(-5L, -7L)"},
                {"addDouble(2, 3)", "Vmc4NativeCall.addDouble(2.0, 3.0)"},
                {"viaDouble(1.5, 2.5)", "Vmc4NativeCall.viaDouble(1.5, 2.5)"},
                {"viaDouble(-1.0, 0.5)", "Vmc4NativeCall.viaDouble(-1.0, 0.5)"},
            },
            7, GetParam().kind);
}

// Phase 2.0 spike：instance 调用路径。viaBox 是 static 方法，对对象参数 b 做
// invokevirtual（b.get()）。验证 helper 的非 static 分支：GetMethodID（非 GetStatic）+
// CallIntMethodA(env, (jobject)regs[recv].ref, ...) + 对象 receiver marshalling。
// Box 是内部类，其构造器/get() 是实例方法（fail-closed 保留字节码），viaBox 调它们时
// 经 JNI 链回字节码实现。仅 viaBox 转 VM（expectedNativeMethods=1）。
TEST_P(Vmc4BackendE2E, InstanceCallOnObjectParamRoundTrip)
{
    runCase("Vmc4NativeInstCall",
            "public class Vmc4NativeInstCall {\n"
            "  public static class Box { int v; public Box(int v) { this.v = v; } "
            "public int get() { return v; } }\n"
            "  public static int viaBox(Box b) { return b.get(); }\n"
            "}\n",
            {
                {"viaBox(42)", "Vmc4NativeInstCall.viaBox(new Vmc4NativeInstCall.Box(42))"},
                {"viaBox(-7)", "Vmc4NativeInstCall.viaBox(new Vmc4NativeInstCall.Box(-7))"},
                {"viaBox(0)", "Vmc4NativeInstCall.viaBox(new Vmc4NativeInstCall.Box(0))"},
            },
            1, GetParam().kind);
}

// Phase 2.A：字段访问（getfield/putfield/getstatic/putstatic）。
// static 方法经对象参数做 getfield（receiver=参数），或直接访问 static 字段。
// 验证 per-site field helper：懒缓存 class + fieldID，按 desc 精确选 JNI accessor
// （GetIntField/GetLongField/GetDoubleField/GetObjectField/GetBooleanField/GetByteField）。
// Box/Narrow 的 <init> 是实例方法（fail-closed 保留字节码），字段由 VM 经 JNI 访问。
TEST_P(Vmc4BackendE2E, FieldAccessRoundTrip)
{
    runCase("Vmc4NativeField",
            "public class Vmc4NativeField {\n"
            "  public static int counter = 10;\n"
            "  public static String label = \"init\";\n"
            "  public static class Box {\n"
            "    public int x; public long y; public double z; public String name;\n"
            "    public Box(int x, long y, double z, String n) { this.x=x; this.y=y; this.z=z; "
            "this.name=n; }\n"
            "  }\n"
            "  public static class Narrow {\n"
            "    public boolean flag; public byte by; public char ch; public short sh;\n"
            "    public Narrow(boolean f, byte b, char c, short s) { flag=f; by=b; ch=c; sh=s; }\n"
            "  }\n"
            "  public static int readX(Box b) { return b.x; }\n"
            "  public static long readY(Box b) { return b.y; }\n"
            "  public static double readZ(Box b) { return b.z; }\n"
            "  public static String readName(Box b) { return b.name; }\n"
            "  public static int writeX(Box b, int v) { b.x = v; return b.x; }\n"
            "  public static boolean readFlag(Narrow n) { return n.flag; }\n"
            "  public static int readByte(Narrow n) { return n.by; }\n"
            "  public static int readChar(Narrow n) { return n.ch; }\n"
            "  public static int readShort(Narrow n) { return n.sh; }\n"
            "  public static int getCounter() { return counter; }\n"
            "  public static String getLabel() { return label; }\n"
            "  public static int setCounter(int v) { counter = v; return counter; }\n"
            "}\n",
            {
                {"readX", "Vmc4NativeField.readX(new Vmc4NativeField.Box(7, 0L, 0.0, null))"},
                {"readY", "Vmc4NativeField.readY(new Vmc4NativeField.Box(0, 100L, 0.0, null))"},
                {"readZ", "Vmc4NativeField.readZ(new Vmc4NativeField.Box(0, 0L, 2.5, null))"},
                {"readName", "Vmc4NativeField.readName(new Vmc4NativeField.Box(0, 0L, 0.0, \"hi\"))"},
                {"writeX", "Vmc4NativeField.writeX(new Vmc4NativeField.Box(0, 0L, 0.0, null), 42)"},
                {"readFlagT", "Vmc4NativeField.readFlag(new Vmc4NativeField.Narrow(true, (byte)0, "
                              "(char)0, (short)0))"},
                {"readFlagF", "Vmc4NativeField.readFlag(new Vmc4NativeField.Narrow(false, (byte)0, "
                              "(char)0, (short)0))"},
                {"readByte", "Vmc4NativeField.readByte(new Vmc4NativeField.Narrow(true, (byte)-7, "
                             "(char)0, (short)0))"},
                {"readChar", "Vmc4NativeField.readChar(new Vmc4NativeField.Narrow(true, (byte)0, "
                             "(char)65, (short)0))"},
                {"readShort", "Vmc4NativeField.readShort(new Vmc4NativeField.Narrow(true, (byte)0, "
                              "(char)0, (short)-99))"},
                {"getCounter", "Vmc4NativeField.getCounter()"},
                {"getLabel", "Vmc4NativeField.getLabel()"},
                {"setCounter", "Vmc4NativeField.setCounter(99)"},
            },
            12, GetParam().kind);
}

// Phase 2.A：数组访问（iaload..aaload / iastore..aastore）。
// static 方法经对象参数（数组）做 ArrayLoad/ArrayStore。验证 per-site helper：按
// VmArrayElemKind 精确选 JNI accessor（GetIntArrayRegion/GetBooleanArrayRegion/...，
// GetObjectArrayElement for ref），null 数组显式抛 NPE，越界交 JNI 抛 AIOOBE。
// 关键：boolean[] 与 byte[] 都用 baload，但 elementType 区分（BOOL→Boolean vs I8→Byte）。
TEST_P(Vmc4BackendE2E, ArrayAccessRoundTrip)
{
    runCase("Vmc4NativeArray",
            "public class Vmc4NativeArray {\n"
            "  public static int readI(int[] a, int i) { return a[i]; }\n"
            "  public static long readL(long[] a, int i) { return a[i]; }\n"
            "  public static float readF(float[] a, int i) { return a[i]; }\n"
            "  public static double readD(double[] a, int i) { return a[i]; }\n"
            "  public static String readRef(String[] a, int i) { return a[i]; }\n"
            "  public static boolean readFlag(boolean[] a, int i) { return a[i]; }\n"
            "  public static int readByte(byte[] a, int i) { return a[i]; }\n"
            "  public static int readChar(char[] a, int i) { return a[i]; }\n"
            "  public static int readShort(short[] a, int i) { return a[i]; }\n"
            "  public static int writeI(int[] a, int i, int v) { a[i] = v; return a[i]; }\n"
            "}\n",
            {
                {"readI", "Vmc4NativeArray.readI(new int[]{10, 20, 30}, 1)"},
                {"readL", "Vmc4NativeArray.readL(new long[]{1L, 2L, 3L}, 2)"},
                {"readF", "Vmc4NativeArray.readF(new float[]{1.5f, 2.5f}, 1)"},
                {"readD", "Vmc4NativeArray.readD(new double[]{1.5, 2.5}, 0)"},
                {"readRef", "Vmc4NativeArray.readRef(new String[]{\"a\", \"b\"}, 1)"},
                {"readFlagT", "Vmc4NativeArray.readFlag(new boolean[]{true, false}, 0)"},
                {"readFlagF", "Vmc4NativeArray.readFlag(new boolean[]{true, false}, 1)"},
                {"readByte", "Vmc4NativeArray.readByte(new byte[]{(byte)-1, (byte)7}, 1)"},
                {"readChar", "Vmc4NativeArray.readChar(new char[]{65, 66}, 0)"},
                {"readShort", "Vmc4NativeArray.readShort(new short[]{(short)5, (short)-3}, 1)"},
                {"writeI", "Vmc4NativeArray.writeI(new int[]{0, 0, 0}, 1, 42)"},
            },
            10, GetParam().kind);
}

// Phase 2.A：Allocate（NEW/NEWARRAY/ANEWARRAY）+ array.length。
// static 方法内部分配对象/数组并使用。Object 分配 = AllocObject + 独立 <init> Call；
// Array = New<Type>Array / NewObjectArray；array.length = GetArrayLength。
TEST_P(Vmc4BackendE2E, AllocateAndArrayLengthRoundTrip)
{
    runCase("Vmc4NativeAlloc",
            "public class Vmc4NativeAlloc {\n"
            "  public static class Box {\n"
            "    public int v;\n"
            "    public Box(int v) { this.v = v; }\n"
            "  }\n"
            "  public static int newBoxValue(int v) { return new Box(v).v; }\n"
            "  public static int newIntArrayLen(int n) { return new int[n].length; }\n"
            "  public static int newIntArrayFirst(int n) { int[] a = new int[n]; return a[0]; }\n"
            "  public static int newIntArrayWrite(int n, int idx, int val) {\n"
            "    int[] a = new int[n]; a[idx] = val; return a[idx];\n"
            "  }\n"
            "  public static int newDoubleArrayLen(int n) { return new double[n].length; }\n"
            "  public static int newRefArrayLen(int n) { return new Box[n].length; }\n"
            "}\n",
            {
                {"newBoxValue", "Vmc4NativeAlloc.newBoxValue(42)"},
                {"newBoxValue0", "Vmc4NativeAlloc.newBoxValue(0)"},
                {"newIntArrayLen", "Vmc4NativeAlloc.newIntArrayLen(7)"},
                {"newIntArrayFirst", "Vmc4NativeAlloc.newIntArrayFirst(3)"},
                {"newIntArrayWrite", "Vmc4NativeAlloc.newIntArrayWrite(5, 2, 99)"},
                {"newDoubleArrayLen", "Vmc4NativeAlloc.newDoubleArrayLen(4)"},
                {"newRefArrayLen", "Vmc4NativeAlloc.newRefArrayLen(6)"},
            },
            6, GetParam().kind);
}

// Phase 2.E：String LDC（ldc_string Opaque 物质化为常量槽，wrapper NewStringUTF
// 预加载）+ MULTIANEWARRAY（递归 NewObjectArray，Phase 2.E 起支持）。
// greeting：LDC + ARETURN；greetLen/isHello/startsWithHi：LDC 参与方法调用
// （String.length/equals/startsWith 是 JNI 调用，LDC 槽作为实参传入）；
// multi*：多维 int/String 数组分配 + 元素读写/长度访问。
// 注：String 连接（"tag:" + s）javac 9+ 编译为 invokedynamic（indy:0），
// 不在本用例范围。
TEST_P(Vmc4BackendE2E, StringLdcAndMultiArrayRoundTrip)
{
    runCase("Vmc4NativeStrArr",
            "public class Vmc4NativeStrArr {\n"
            "  public static String greeting() { return \"hello\"; }\n"
            "  public static int greetLen() { return \"hello\".length(); }\n"
            "  public static boolean isHello(String s) { return \"hello\".equals(s); }\n"
            "  public static boolean startsWithHi(String s) { return s.startsWith(\"hi\"); }\n"
            "  public static int multiRows() { int[][] m = new int[2][3]; return m.length; }\n"
            "  public static int multiDims() { int[][] m = new int[2][3]; return m[0].length; }\n"
            "  public static int multiFill() {\n"
            "    int[][] m = new int[2][3];\n"
            "    m[0][1] = 7; m[1][2] = 9;\n"
            "    return m[0][1] + m[1][2];\n"
            "  }\n"
            "  public static int multiRefLen() { String[][] m = new String[2][3]; return "
            "m.length + m[0].length; }\n"
            "}\n",
            {
                {"greeting", "Vmc4NativeStrArr.greeting()"},
                {"greetLen", "Vmc4NativeStrArr.greetLen()"},
                {"isHelloTrue", "Vmc4NativeStrArr.isHello(\"hello\")"},
                {"isHelloFalse", "Vmc4NativeStrArr.isHello(\"world\")"},
                {"startsHiTrue", "Vmc4NativeStrArr.startsWithHi(\"hi there\")"},
                {"startsHiFalse", "Vmc4NativeStrArr.startsWithHi(\"yo\")"},
                {"multiRows", "Vmc4NativeStrArr.multiRows()"},
                {"multiDims", "Vmc4NativeStrArr.multiDims()"},
                {"multiFill", "Vmc4NativeStrArr.multiFill()"},
                {"multiRefLen", "Vmc4NativeStrArr.multiRefLen()"},
            },
            8, GetParam().kind);
}

// Phase 2.B：虚分派 + 接口调用 + 实例方法调用链（单类输入：只有主类方法转 VM）。
// viaInterface：invokeinterface 经接口引用调用 → JVM 虚分派到主类已转 VM 的 calc
// （VM→JNI→JVM 分派→VM 闭环）；chain/double_：实例方法互相调用（this 经 regs[0]）；
// shapeArea：invokevirtual 经声明类型 Shape 引用虚分派到 Square 覆盖实现（嵌套类
// 保留字节码，JVM 侧分派，验证 VM 内 JNI 虚分派语义与基线一致）。
TEST_P(Vmc4BackendE2E, VirtualDispatchAndInterfaceRoundTrip)
{
    runCase("Vmc4NativeVirt",
            "public class Vmc4NativeVirt implements Calc {\n"
            "  public int calc(int x) { return x * 3; }\n"
            "  public static int viaInterface(Calc c, int x) { return c.calc(x); }\n"
            "  public int chain(int x) { return this.double_(x) + 1; }\n"
            "  public int double_(int x) { return x * 2; }\n"
            "  public static class Shape {\n"
            "    public int area() { return 0; }\n"
            "  }\n"
            "  public static class Square extends Shape {\n"
            "    int side;\n"
            "    public Square(int s) { this.side = s; }\n"
            "    public int area() { return side * side; }\n"
            "  }\n"
            "  public static int shapeArea(Shape s) { return s.area(); }\n"
            "}\n"
            "interface Calc {\n"
            "  int calc(int x);\n"
            "}\n",
            {
                {"viaInterface", "Vmc4NativeVirt.viaInterface(new Vmc4NativeVirt(), 7)"},
                {"chain", "new Vmc4NativeVirt().chain(4)"},
                {"shapeAreaSquare", "Vmc4NativeVirt.shapeArea(new Vmc4NativeVirt.Square(5))"},
                {"shapeAreaShape", "Vmc4NativeVirt.shapeArea(new Vmc4NativeVirt.Shape())"},
            },
            // 主类 5 方法转 VM：calc/viaInterface/chain/double_/shapeArea（嵌套类不转）。
            5, GetParam().kind);
}

// Phase 2.B：invokespecial（private）在 VM 内执行（CallNonvirtual）+ null receiver NPE。
// callPriv 里的 priv() 由 javac 编译为 INVOKESPECIAL Vmc4NativeSpec.priv（私有方法
// 不虚分派）→ VM 内 CallNonvirtualIntMethodA 到已转 VM 的 priv；subWho/baseWho：
// invokevirtual 经 Base 引用虚分派到嵌套类 Sub 覆盖（保留字节码，JVM 侧分派）；
// nullCall(null)：instance 调用 null receiver 显式抛 NPE（JVM 语义）。
TEST_P(Vmc4BackendE2E, InvokeSpecialAndNullReceiverRoundTrip)
{
    runCase("Vmc4NativeSpec",
            "public class Vmc4NativeSpec {\n"
            "  public static class Base {\n"
            "    public int who() { return 1; }\n"
            "  }\n"
            "  public static class Sub extends Base {\n"
            "    public int who() { return 2; }\n"
            "  }\n"
            "  private int priv() { return 42; }\n"
            "  public int callPriv() { return priv(); }\n"
            "  public static int viaPriv(Vmc4NativeSpec o) { return o.callPriv(); }\n"
            "  public static int subWho(Sub s) { return s.who(); }\n"
            "  public static int baseWho(Base b) { return b.who(); }\n"
            "  public static int nullCall(Vmc4NativeSpec o) { return o.callPriv(); }\n"
            "}\n",
            {
                {"callPriv", "new Vmc4NativeSpec().callPriv()"},
                {"viaPriv", "Vmc4NativeSpec.viaPriv(new Vmc4NativeSpec())"},
                {"subWho", "Vmc4NativeSpec.subWho(new Vmc4NativeSpec.Sub())"},
                {"baseWhoSub", "Vmc4NativeSpec.baseWho(new Vmc4NativeSpec.Sub())"},
                {"baseWhoBase", "Vmc4NativeSpec.baseWho(new Vmc4NativeSpec.Base())"},
                {"nullCall", "Vmc4NativeSpec.nullCall(null)"},
            },
            // 主类 6 方法转 VM：priv/callPriv/viaPriv/subWho/baseWho/nullCall。
            6, GetParam().kind);
}

// Phase 2.C：异常语义 round-trip。
// divCatch：try 块内除零 → 解释器抛 ArithmeticException → 检查点按区域表匹配
// handler（IsInstanceOf）→ exceptionReg 承接异常 → 跳 handler 返回 -1；
// divUncaught：除零无区域 → 检查点不匹配 → status=1 传播 → Java 侧抛
// ArithmeticException（Driver 捕获 THROW:...）；
// throwCatch/throwUncaught：athrow（mj2x_vm_throw）+ 区域匹配 / 传播；
// nestedCatch：嵌套 try（内层 ArithmeticException 先匹配）。
TEST_P(Vmc4BackendE2E, ExceptionRoundTrip)
{
    runCase("Vmc4NativeExc",
            "public class Vmc4NativeExc {\n"
            "  public static int divCatch(int a, int b) {\n"
            "    try { return a / b; }\n"
            "    catch (ArithmeticException e) { return -1; }\n"
            "  }\n"
            "  public static int divUncaught(int a, int b) { return a / b; }\n"
            "  public static int throwCatch(int x) {\n"
            "    try {\n"
            "      if (x < 0) throw new IllegalArgumentException();\n"
            "      return x * 2;\n"
            "    } catch (IllegalArgumentException e) { return -100; }\n"
            "  }\n"
            "  public static int throwUncaught(int x) {\n"
            "    if (x < 0) throw new IllegalStateException();\n"
            "    return x;\n"
            "  }\n"
            "  public static int nestedCatch(int a, int b) {\n"
            "    try {\n"
            "      try { return a / b; }\n"
            "      catch (ArithmeticException e) { return 1; }\n"
            "    } catch (Exception e) { return 2; }\n"
            "  }\n"
            "}\n",
            {
                {"divCatchOk", "Vmc4NativeExc.divCatch(10, 2)"},
                {"divCatchZero", "Vmc4NativeExc.divCatch(10, 0)"},
                {"divUncaught", "Vmc4NativeExc.divUncaught(10, 0)"},
                {"throwCatchOk", "Vmc4NativeExc.throwCatch(5)"},
                {"throwCatchNeg", "Vmc4NativeExc.throwCatch(-1)"},
                {"throwUncaughtNeg", "Vmc4NativeExc.throwUncaught(-1)"},
                {"throwUncaughtOk", "Vmc4NativeExc.throwUncaught(7)"},
                {"nestedInner", "Vmc4NativeExc.nestedCatch(10, 0)"},
                {"nestedOk", "Vmc4NativeExc.nestedCatch(10, 2)"},
            },
            5, GetParam().kind);
}

// 后续项：CHECKCAST（castcheck:<desc> intrinsic）语义锁定——
// null 通过（JVM 语义）、命中原值传递、失败抛 ClassCastException（经 Driver
// 捕获 THROW:...）、try 内失败被保护区捕获（异常精确化链路对 castcheck 生效）。
TEST_P(Vmc4BackendE2E, CheckCastRoundTrip)
{
    runCase("Vmc4NativeCast",
            "public class Vmc4NativeCast {\n"
            "  public static int castOk(Object o) { return ((String)o).length(); }\n"
            "  public static String castNull(Object o) { return (String)o; }\n"
            "  public static int castFail(Object o) { return ((String)o).length(); }\n"
            "  public static int castCatch(Object o) {\n"
            "    try { return ((String)o).length(); }\n"
            "    catch (ClassCastException e) { return -7; }\n"
            "  }\n"
            "}\n",
            {
                {"ok", "Vmc4NativeCast.castOk(\"hello\")"},
                {"nullPass", "Vmc4NativeCast.castNull(null)"},
                {"fail", "Vmc4NativeCast.castFail(Integer.valueOf(1))"},
                {"catchIt", "Vmc4NativeCast.castCatch(Integer.valueOf(1))"},
            },
            4, GetParam().kind);
}

// 后续项：NullLiteral (object ref) → REF NULL 常量槽——null 比较/返回 null/
// null 实参/checkcast+null 比较（jvm-obf-tester 剩余 fail-closed 主因之一）。
TEST_P(Vmc4BackendE2E, NullLiteralRoundTrip)
{
    runCase("Vmc4NativeNull",
            "public class Vmc4NativeNull {\n"
            "  public static boolean isNull(Object o) { return o == null; }\n"
            "  public static String retNull(int x) { return x < 0 ? null : \"ok\"; }\n"
            "  public static boolean passNull() { return accept(null); }\n"
            "  public static boolean accept(String s) { return s == null; }\n"
            "  public static int castNullCmp(Object o) { return (String)o == null ? 7 : 0; }\n"
            "}\n",
            {
                {"isNullTrue", "Vmc4NativeNull.isNull(null)"},
                {"isNullFalse", "Vmc4NativeNull.isNull(\"x\")"},
                {"retNullNeg", "Vmc4NativeNull.retNull(-1)"},
                {"retNullOk", "Vmc4NativeNull.retNull(3)"},
                {"passNull", "Vmc4NativeNull.passNull()"},
                {"castNullCmp", "Vmc4NativeNull.castNullCmp(null)"},
            },
            5, GetParam().kind);
}

// 后续项：`.class` 字面量（ldc_class Opaque）→ FindClass 预加载 Class 常量槽
// （JVM ldc 类常量语义：触发加载不触发初始化；jvm-obf-tester 剩余 fail-closed
// 主因之一）。
TEST_P(Vmc4BackendE2E, ClassLiteralRoundTrip)
{
    runCase("Vmc4NativeClass",
            "public class Vmc4NativeClass {\n"
            "  public static String selfName() { return Vmc4NativeClass.class.getName(); }\n"
            "  public static boolean isString(Object o) { return o.getClass() == String.class; }\n"
            "  public static boolean isSelf(Class<?> c) { return c == Vmc4NativeClass.class; }\n"
            "}\n",
            {
                {"selfName", "Vmc4NativeClass.selfName()"},
                {"isStringTrue", "Vmc4NativeClass.isString(\"x\")"},
                {"isStringFalse", "Vmc4NativeClass.isString(Integer.valueOf(1))"},
                {"isSelfTrue", "Vmc4NativeClass.isSelf(Vmc4NativeClass.class)"},
                {"isSelfFalse", "Vmc4NativeClass.isSelf(String.class)"},
            },
            3, GetParam().kind);
}

// switch terminator（tableswitch/lookupswitch 统一形态，Phase 3 增补）：
// 密集表（0-2）、default、稀疏表（lookupswitch：5/-3/100）、0-6 密集表
// （对齐 jvm-obf-tester ControlFlowProbe 形态）、循环内 switch、try 内 switch。
TEST_P(Vmc4BackendE2E, SwitchRoundTrip)
{
    runCase("Vmc4NativeSwitch",
            "public class Vmc4NativeSwitch {\n"
            "  public static int table(int x) {\n"
            "    switch (x) {\n"
            "      case 0: return 10;\n"
            "      case 1: return 11;\n"
            "      case 2: return 12;\n"
            "      default: return 99;\n"
            "    }\n"
            "  }\n"
            "  public static int lookup(int x) {\n"
            "    switch (x) {\n"
            "      case 5: return 50;\n"
            "      case -3: return 30;\n"
            "      case 100: return 100;\n"
            "      default: return -1;\n"
            "    }\n"
            "  }\n"
            "  public static int dense(int x) {\n"
            "    switch (x) {\n"
            "      case 0: return 0;\n"
            "      case 1: return 1;\n"
            "      case 2: return 2;\n"
            "      case 3: return 3;\n"
            "      case 4: return 4;\n"
            "      case 5: return 5;\n"
            "      default: return 6;\n"
            "    }\n"
            "  }\n"
            "  public static int loopSwitch(int n) {\n"
            "    int acc = 0;\n"
            "    for (int i = 0; i < n; i++) {\n"
            "      switch (i % 3) {\n"
            "        case 0: acc += 1; break;\n"
            "        case 1: acc += 10; break;\n"
            "        default: acc += 100;\n"
            "      }\n"
            "    }\n"
            "    return acc;\n"
            "  }\n"
            "  public static int trySwitch(int x) {\n"
            "    try {\n"
            "      switch (x) { case 0: return 7; default: return 8; }\n"
            "    } catch (Exception e) { return -7; }\n"
            "  }\n"
            "}\n",
            {
                {"table0", "Vmc4NativeSwitch.table(0)"},
                {"table1", "Vmc4NativeSwitch.table(1)"},
                {"table2", "Vmc4NativeSwitch.table(2)"},
                {"tableDefault", "Vmc4NativeSwitch.table(9)"},
                {"lookup5", "Vmc4NativeSwitch.lookup(5)"},
                {"lookupNeg3", "Vmc4NativeSwitch.lookup(-3)"},
                {"lookup100", "Vmc4NativeSwitch.lookup(100)"},
                {"lookupDefault", "Vmc4NativeSwitch.lookup(7)"},
                {"dense0", "Vmc4NativeSwitch.dense(0)"},
                {"dense3", "Vmc4NativeSwitch.dense(3)"},
                {"dense5", "Vmc4NativeSwitch.dense(5)"},
                {"denseDefault", "Vmc4NativeSwitch.dense(9)"},
                {"loop5", "Vmc4NativeSwitch.loopSwitch(5)"},
                {"loop1", "Vmc4NativeSwitch.loopSwitch(1)"},
                {"try0", "Vmc4NativeSwitch.trySwitch(0)"},
                {"tryDefault", "Vmc4NativeSwitch.trySwitch(5)"},
            },
            5, GetParam().kind);
}

// Phase 2.C 回归：athrow 异常归属 = athrow 所在块，而非布局顺序的下一块。
// throwCatchTail 的 athrow 块（保护区内）之后紧跟 handler/未保护块（try 内
// throw、try 外 return）：若检查点按「pc 之后的块」归属，异常会逃逸传播
// （修复前 4 dispatcher 实测均为 THROW 而非 -1）。
TEST_P(Vmc4BackendE2E, AthrowAttributionRoundTrip)
{
    runCase("Vmc4NativeExcTail",
            "public class Vmc4NativeExcTail {\n"
            "  public static int throwCatchTail(int x) {\n"
            "    try {\n"
            "      if (x < 0) throw new IllegalArgumentException();\n"
            "    } catch (IllegalArgumentException e) { return -1; }\n"
            "    return x;\n"
            "  }\n"
            "}\n",
            {
                {"neg", "Vmc4NativeExcTail.throwCatchTail(-1)"},
                {"pos", "Vmc4NativeExcTail.throwCatchTail(5)"},
            },
            1, GetParam().kind);
}

// Phase 2.C 稀疏异常语义锁定：保护区边界精确性（try 前/后指令不在保护区内）。
// semiCatch：try 前 x*2 与 try 后 return 不受保护；try 内除零（y/x）进 catch，
// try 以非终结指令 y++ 结束（end label 切块边界形态）。semiAfter：try 后的
// x/0 除零必须传播（不被 catch）。
TEST_P(Vmc4BackendE2E, ExceptionPrecisionRoundTrip)
{
    runCase("Vmc4NativeExcPrec",
            "public class Vmc4NativeExcPrec {\n"
            "  public static int semiCatch(int x) {\n"
            "    int y = x * 2;\n"
            "    try {\n"
            "      y = y / x;\n"
            "      y++;\n"
            "    } catch (ArithmeticException e) { return -1; }\n"
            "    return y;\n"
            "  }\n"
            "  public static int semiAfter(int x) {\n"
            "    try { x /= 1; } catch (ArithmeticException e) { return -1; }\n"
            "    return x / 0;\n"
            "  }\n"
            "}\n",
            {
                {"semiCatchZero", "Vmc4NativeExcPrec.semiCatch(0)"},
                {"semiCatchOk", "Vmc4NativeExcPrec.semiCatch(4)"},
                {"semiAfter", "Vmc4NativeExcPrec.semiAfter(5)"},
            },
            2, GetParam().kind);
}

// Phase 2.D：Intrinsic 子集 round-trip。
// isString/isNullInstanceOf：instanceof:<desc>（null → false，IsInstanceOf 懒缓存）；
// longCmp：LCMP（cmp.nan_less 整型路径，三值 -1/0/1）；
// floatCmpG：FCMPG（cmp.nan_greater 浮点 NaN-aware：NaN → 1）；
// syncInc：synchronized 块（monitor.enter/exit + 异常表 catch-all + athrow 重抛组合）。
TEST_P(Vmc4BackendE2E, IntrinsicAndMonitorRoundTrip)
{
    runCase("Vmc4NativeIntr",
            "public class Vmc4NativeIntr {\n"
            "  public static int isString(Object o) { return o instanceof String ? 1 : 0; }\n"
            "  public static int isIntr(Object o) { return o instanceof Vmc4NativeIntr ? 1 : 0; }\n"
            "  public static int longCmp(long a, long b) { return a > b ? 1 : (a < b ? -1 : 0); }\n"
            "  public static int floatCmpG(float a, float b) { return a > b ? 1 : (a < b ? -1 : 0); }\n"
            "  public static int syncInc(Object lock, int x) {\n"
            "    synchronized (lock) { return x + 1; }\n"
            "  }\n"
            "}\n",
            {
                {"isStringOk", "Vmc4NativeIntr.isString(\"x\")"},
                {"isStringNot", "Vmc4NativeIntr.isString(Integer.valueOf(5))"},
                {"isIntrNull", "Vmc4NativeIntr.isIntr(null)"},
                {"isIntrSelf", "Vmc4NativeIntr.isIntr(new Vmc4NativeIntr())"},
                {"longCmpGt", "Vmc4NativeIntr.longCmp(5, 3)"},
                {"longCmpLt", "Vmc4NativeIntr.longCmp(3, 5)"},
                {"longCmpEq", "Vmc4NativeIntr.longCmp(4, 4)"},
                {"floatCmpGGt", "Vmc4NativeIntr.floatCmpG(1.5f, 1.0f)"},
                {"floatCmpGLt", "Vmc4NativeIntr.floatCmpG(1.0f, 1.5f)"},
                {"floatCmpGNaN", "Vmc4NativeIntr.floatCmpG(Float.NaN, 1.0f)"},
                {"syncInc", "Vmc4NativeIntr.syncInc(new Object(), 5)"},
            },
            5, GetParam().kind);
}

// Phase 2.D：双后端对拍——同一 Math/wrapper 方法集分别经 VMC4（JNI Call 路径）
// 与 RegC（intrinsic helper 路径）虚拟化，输出必须与 baseline 完全一致。
// abs/sqrt/max/min 走 invokestatic（VMC4 per-callsite JNI helper ↔ RegC intrinsic
// registry 特化 helper）；parseInt 走 Integer.parseInt 静态调用。
TEST_P(Vmc4BackendE2E, MathIntrinsicDualBackendParity)
{
    runDualBackendCase("Vmc4NativeMath",
                       "public class Vmc4NativeMath {\n"
                       "  public static int absInt(int x) { return Math.abs(x); }\n"
                       "  public static long absLong(long x) { return Math.abs(x); }\n"
                       "  public static double absDouble(double x) { return Math.abs(x); }\n"
                       "  public static double sqrt(double x) { return Math.sqrt(x); }\n"
                       "  public static int maxInt(int a, int b) { return Math.max(a, b); }\n"
                       "  public static int minInt(int a, int b) { return Math.min(a, b); }\n"
                       "  public static int parseInt(String s) { return Integer.parseInt(s); }\n"
                       "}\n",
                       {
                           {"absIntNeg", "Vmc4NativeMath.absInt(-42)"},
                           {"absLongMin", "Vmc4NativeMath.absLong(-9223372036854775807L - 1)"},
                           {"absDoubleNeg", "Vmc4NativeMath.absDouble(-3.25)"},
                           {"sqrt", "Vmc4NativeMath.sqrt(2.0)"},
                           {"maxInt", "Vmc4NativeMath.maxInt(7, 3)"},
                           {"minInt", "Vmc4NativeMath.minInt(7, 3)"},
                           {"parseInt", "Vmc4NativeMath.parseInt(\"12345\")"},
                       },
                       GetParam().kind);
}

// ===========================================================================
// 程序 blob at-rest 加密（issue #760 首切片，--vmc4-encrypt-program 等价配置）：
// 全链路 round-trip —— 生成 C 数据段为密文（结构断言），JNI_OnLoad 加载期
// 原地解密，Java 调用结果与 baseline 完全一致（与 drift 叠加验证正交性）。
// ===========================================================================
TEST(Vmc4BackendEncryptE2E, ProgramBlobEncryptRoundTrip)
{
    Toolchain tc = probeToolchain();
    if (!tc.available) {
        GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc.reason;
    }
    const std::string className = "Vmc4Encrypted";
    fs::path dir =
        test::getOutPath("mj2x/vmc4_e2e/Vmc4Encrypted/.keep").parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);

    fs::path javaFile = dir / (className + ".java");
    {
        std::ofstream out(javaFile);
        out << "public class Vmc4Encrypted {\n"
            << "  public static int addMul(int a, int b) { return (a + b) * 3; }\n"
            << "  public static int loopSum(int n) {\n"
            << "    int acc = 0;\n"
            << "    for (int i = 0; i < n; i++) acc += i;\n"
            << "    return acc;\n"
            << "  }\n"
            << "}\n";
    }
    auto javac = runBat({q(tc.javac) + " -d " + q(dir) + " " + q(javaFile)});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    // 固定 Driver（不依赖 fixture 的 buildDriver）。
    fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream out(driverFile);
        out << "public class Driver {\n"
            << "  public static void main(String[] args) throws Exception {\n"
            << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
            << "    if (dll != null) System.load(dll);\n"
            << "    System.out.println(\"addMul=\" + Vmc4Encrypted.addMul(3, 4));\n"
            << "    System.out.println(\"addMulWrap=\" + Vmc4Encrypted.addMul(2147483647, 1));\n"
            << "    System.out.println(\"loopSum0=\" + Vmc4Encrypted.loopSum(0));\n"
            << "    System.out.println(\"loopSum7=\" + Vmc4Encrypted.loopSum(7));\n"
            << "    System.out.println(\"loopSum100=\" + Vmc4Encrypted.loopSum(100));\n"
            << "  }\n"
            << "}\n";
    }
    auto driverCompile =
        runBat({q(tc.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
    ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

    auto baselineRun = runBat({q(tc.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
    const auto baseline = splitLines(baselineRun.out);
    ASSERT_EQ(baseline.size(), 5u);

    fs::path inClass  = dir / (className + ".class");
    fs::path outClass = dir / (className + "_protected.class");
    fs::path cFile    = dir / (className + "_enc.c");

    mj2x::PipelineConfig config;
    config.inputPath  = inClass.string();
    config.outputPath = outClass.string();
    mj2x::Pipeline    pipeline(std::move(config));
    auto              backend = std::make_unique<mj2x::Vmc4Backend>();
    backend->setOutputPath(cFile.string());
    mj2x::Vmc4Options opts;
    opts.dispatcherKind      = mj2x::VmDispatcherKind::SwitchTable;
    opts.driftVariants       = 2;    // drift 全开：与加密叠加验证正交性
    opts.driftPeriod         = 16;
    opts.programBlobEncrypt  = true;
    backend->setOptions(opts);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
    ASSERT_EQ(pipeline.run(), 0);
    ASSERT_TRUE(fileExists(cFile));
    const std::string generatedC = readText(cFile);

    // 产物结构断言：数据段为 non-const 密文数组；不存在明文 const 数组形态；
    // JNI_OnLoad 含每方法的 mj2x_vm_blob_decrypt 原地解密调用。
    EXPECT_NE(generatedC.find("static unsigned char mj2x_vm_code_"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static const unsigned char mj2x_vm_code_"), std::string::npos)
        << "plaintext bytecode array must not survive in the encrypted product\n"
        << generatedC;
    EXPECT_EQ(countOccurrences(generatedC, "mj2x_vm_blob_decrypt(mj2x_vm_code_"), 2u)
        << "one decrypt call per virtualized method\n"
        << generatedC;

    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    // 编译生成的 C（与 runCase 同一编译口径：mj2x_vmc4_runtime.c 一并链接）。
    fs::path dll = dir / (className + "_enc.dll");
    fs::path obj = dir / (className + "_enc.obj");
    fs::path crtSrc = fs::current_path() / "mj2x" / "mj2x-rt" / "src";
    fs::path runtimeObj = dir / "mj2x_vmc4_runtime_enc.obj";
    auto cl = runBat({
        "call " + q(tc.vcvars64) + " x64 >nul",
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(cFile) + " /I" + q(crtSrc) +
            " /I" + q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(runtimeObj) + " " +
            q(crtSrc / "vmc4" / "mj2x_vmc4_runtime.c") + " /I" + q(crtSrc) + " /I" +
            q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "link /nologo /DLL /OUT:" + q(dll) + " " + q(obj) + " " + q(runtimeObj),
    });
    ASSERT_EQ(cl.exitCode, 0) << cl.out;
    ASSERT_TRUE(fileExists(dll));

    auto protectedRun = runBat({
        "set \"MORPHER_DLL=" + dll.string() + "\"",
        q(tc.java) + " -cp " + q(dir) + " Driver",
    });
    ASSERT_EQ(protectedRun.exitCode, 0) << protectedRun.out;
    const auto actual = splitLines(protectedRun.out);

    ASSERT_EQ(baseline.size(), actual.size()) << "baseline:\n"
                                              << joinLines(baseline) << "protected:\n"
                                              << joinLines(actual);
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(baseline[i], actual[i]) << "line " << i;
    }
}

// ===========================================================================
// 分块懒解密（issue #760 切片 2，--vmc4-lazy-decrypt 等价配置）：全链路
// round-trip —— 生成 C 数据段为分块密文 + cend/cstate 表 + touch helper，
// JNI_OnLoad 无解密调用；解释器首执行到块才经 CRT mj2x_vm_lazy_decrypt 原地
// 解密（本 e2e 同时是发射侧 vmc4DeriveChunkKey 与 CRT 侧 mj2x_vm_chunk_key
// 双镜像公式的执行级对拍门）；Java 调用结果与 baseline 完全一致（drift
// 叠加验证正交性）。
// ===========================================================================
TEST(Vmc4BackendEncryptE2E, LazyChunkDecryptRoundTrip)
{
    Toolchain tc = probeToolchain();
    if (!tc.available) {
        GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc.reason;
    }
    const std::string className = "Vmc4Lazy";
    fs::path dir =
        test::getOutPath("mj2x/vmc4_e2e/Vmc4Lazy/.keep").parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);

    fs::path javaFile = dir / (className + ".java");
    {
        std::ofstream out(javaFile);
        out << "public class Vmc4Lazy {\n"
            << "  public static int addMul(int a, int b) { return (a + b) * 3; }\n"
            << "  public static int loopSum(int n) {\n"
            << "    int acc = 0;\n"
            << "    for (int i = 0; i < n; i++) acc += i;\n"
            << "    return acc;\n"
            << "  }\n"
            << "  public static int pick(int x) {\n"
            << "    switch (x % 7) {\n"
            << "      case 0: return 100; case 1: return 101; case 2: return 102;\n"
            << "      case 3: return 103; case 4: return 104; case 5: return 105;\n"
            << "      default: return 106;\n"
            << "    }\n"
            << "  }\n"
            << "}\n";
    }
    auto javac = runBat({q(tc.javac) + " -d " + q(dir) + " " + q(javaFile)});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream out(driverFile);
        out << "public class Driver {\n"
            << "  public static void main(String[] args) throws Exception {\n"
            << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
            << "    if (dll != null) System.load(dll);\n"
            << "    System.out.println(\"addMul=\" + Vmc4Lazy.addMul(3, 4));\n"
            << "    System.out.println(\"loopSum7=\" + Vmc4Lazy.loopSum(7));\n"
            << "    System.out.println(\"loopSum100=\" + Vmc4Lazy.loopSum(100));\n"
            << "    System.out.println(\"pick3=\" + Vmc4Lazy.pick(3));\n"
            << "    System.out.println(\"pick12=\" + Vmc4Lazy.pick(12));\n"
            << "  }\n"
            << "}\n";
    }
    auto driverCompile =
        runBat({q(tc.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
    ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

    auto baselineRun = runBat({q(tc.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
    const auto baseline = splitLines(baselineRun.out);
    ASSERT_EQ(baseline.size(), 5u);

    fs::path inClass  = dir / (className + ".class");
    fs::path outClass = dir / (className + "_protected.class");
    fs::path cFile    = dir / (className + "_lazy.c");

    mj2x::PipelineConfig config;
    config.inputPath  = inClass.string();
    config.outputPath = outClass.string();
    mj2x::Pipeline    pipeline(std::move(config));
    auto              backend = std::make_unique<mj2x::Vmc4Backend>();
    backend->setOutputPath(cFile.string());
    mj2x::Vmc4Options opts;
    opts.dispatcherKind       = mj2x::VmDispatcherKind::SwitchTable;
    opts.driftVariants        = 2;    // drift 全开：与懒解密叠加验证正交性
    opts.driftPeriod          = 16;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    backend->setOptions(opts);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
    ASSERT_EQ(pipeline.run(), 0);
    ASSERT_TRUE(fileExists(cFile));
    const std::string generatedC = readText(cFile);

    // 产物结构断言：non-const 密文数组 + 懒解密基础设施；JNI_OnLoad 不含
    // mj2x_vm_blob_decrypt（块保持密文到首次执行）。
    EXPECT_NE(generatedC.find("chunked at rest, decrypted lazily on first"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("mj2x_vm_blob_decrypt"), std::string::npos)
        << "lazy product must not decrypt the whole blob in JNI_OnLoad\n"
        << generatedC;
    EXPECT_EQ(countOccurrences(generatedC, "static void mj2x_vm_lazy_touch_"), 3u)
        << "one touch helper per virtualized method\n"
        << generatedC;
    EXPECT_EQ(countOccurrences(generatedC, "static volatile unsigned char mj2x_vm_cstate_"), 3u)
        << generatedC;

    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    // 编译生成的 C（与 ProgramBlobEncryptRoundTrip 同一编译口径）。
    fs::path dll = dir / (className + "_lazy.dll");
    fs::path obj = dir / (className + "_lazy.obj");
    fs::path crtSrc = fs::current_path() / "mj2x" / "mj2x-rt" / "src";
    fs::path runtimeObj = dir / "mj2x_vmc4_runtime_lazy.obj";
    auto cl = runBat({
        "call " + q(tc.vcvars64) + " x64 >nul",
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(cFile) + " /I" + q(crtSrc) +
            " /I" + q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(runtimeObj) + " " +
            q(crtSrc / "vmc4" / "mj2x_vmc4_runtime.c") + " /I" + q(crtSrc) + " /I" +
            q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "link /nologo /DLL /OUT:" + q(dll) + " " + q(obj) + " " + q(runtimeObj),
    });
    ASSERT_EQ(cl.exitCode, 0) << cl.out;
    ASSERT_TRUE(fileExists(dll));

    auto protectedRun = runBat({
        "set \"MORPHER_DLL=" + dll.string() + "\"",
        q(tc.java) + " -cp " + q(dir) + " Driver",
    });
    ASSERT_EQ(protectedRun.exitCode, 0) << protectedRun.out;
    const auto actual = splitLines(protectedRun.out);

    ASSERT_EQ(baseline.size(), actual.size()) << "baseline:\n"
                                              << joinLines(baseline) << "protected:\n"
                                              << joinLines(actual);
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(baseline[i], actual[i]) << "line " << i;
    }
}

// ===========================================================================
// 块乱序 e2e（issue #1116 切片 A，--vmc4-shuffle-blocks）：三档 round-trip
//   shuffle 单开 / shuffle+drift / shuffle+drift+encrypt-program，全部
//   -Xverify:all 真编译执行。样本含 switch + 多路分支 + 循环 + try/catch
//   （异常区域重映射的执行级实证）。机制：runShuffleRoundTrip 驱动（无 gtest
//   宏，错误经 detail 上抛），TEST 只选档 + 结构断言 + 输出对拍。
// ===========================================================================

namespace {

struct ShuffleRoundTripOut
{
    bool                     ok = false;
    std::string              detail;
    std::vector<std::string> baseline;
    std::vector<std::string> actual;
    std::string              generatedC;
};

/// 一档 shuffle round-trip（shuffle 恒开；driftVariants 0=关；encrypt 选加密档）。
ShuffleRoundTripOut runShuffleRoundTrip(const Toolchain& tc, const std::string& className,
                                        int driftVariants, bool encrypt)
{
    ShuffleRoundTripOut r;
    fs::path dir = test::getOutPath("mj2x/vmc4_e2e/" + className + "/.keep").parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);

    // 样本：switch + 多路分支 + 循环 + try/catch（异常区域跨块重映射）。
    fs::path javaFile = dir / (className + ".java");
    {
        std::ofstream out(javaFile);
        out << "public class " << className << " {\n"
            << "  public static int classify(int x) {\n"
            << "    int r;\n"
            << "    switch (x % 4) {\n"
            << "      case 0: r = x + 1; break;\n"
            << "      case 1: r = x * 2; break;\n"
            << "      case 2: r = x - 3; break;\n"
            << "      default: r = -x; break;\n"
            << "    }\n"
            << "    int guard = 0;\n"
            << "    while (r > 40) {\n"
            << "      r -= 40;\n"
            << "      guard++;\n"
            << "      if (guard > 10) { r = -1; break; }\n"
            << "    }\n"
            << "    return r;\n"
            << "  }\n"
            << "  public static int divSafe(int a, int b) {\n"
            << "    try {\n"
            << "      return a / b + classify(a);\n"
            << "    } catch (ArithmeticException e) {\n"
            << "      return -7;\n"
            << "    }\n"
            << "  }\n"
            << "}\n";
    }
    auto javac = runBat({q(tc.javac) + " -d " + q(dir) + " " + q(javaFile)});
    if (javac.exitCode != 0) {
        r.detail = "javac failed: " + javac.out;
        return r;
    }

    fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream out(driverFile);
        out << "public class Driver {\n"
            << "  public static void main(String[] args) {\n"
            << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
            << "    if (dll != null) System.load(dll);\n"
            << "    int[] xs = {0, 1, 2, 3, 7, 44, 444, -3, 84, -45};\n"
            << "    for (int x : xs) System.out.println(\"classify\" + x + \"=\" + "
            << className << ".classify(x));\n"
            << "    int[][] pairs = {{10, 2}, {7, 0}, {-9, 3}, {100, 4}, {1, 3}};\n"
            << "    for (int[] p : pairs) System.out.println(\"divSafe\" + p[0] + \"_\" + p[1]"
            << " + \"=\" + " << className << ".divSafe(p[0], p[1]));\n"
            << "  }\n"
            << "}\n";
    }
    auto driverCompile =
        runBat({q(tc.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
    if (driverCompile.exitCode != 0) {
        r.detail = "driver javac failed: " + driverCompile.out;
        return r;
    }
    auto baselineRun = runBat({q(tc.java) + " -Xverify:all -cp " + q(dir) + " Driver"});
    if (baselineRun.exitCode != 0 || splitLines(baselineRun.out).size() != 15u) {
        r.detail = "baseline run failed: " + baselineRun.out;
        return r;
    }
    r.baseline = splitLines(baselineRun.out);

    // 保护管线：shuffle 恒开，drift/encrypt 按档。
    fs::path inClass  = dir / (className + ".class");
    fs::path outClass = dir / (className + "_protected.class");
    fs::path cFile    = dir / (className + "_shuf.c");
    mj2x::PipelineConfig config;
    config.inputPath  = inClass.string();
    config.outputPath = outClass.string();
    mj2x::Pipeline    pipeline(std::move(config));
    auto              backend = std::make_unique<mj2x::Vmc4Backend>();
    backend->setOutputPath(cFile.string());
    mj2x::Vmc4Options opts;
    opts.dispatcherKind      = mj2x::VmDispatcherKind::SwitchTable;
    opts.shuffleBlocks       = true;
    opts.driftVariants       = driftVariants;
    opts.driftPeriod         = 16;
    opts.programBlobEncrypt  = encrypt;
    backend->setOptions(opts);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
    if (pipeline.run() != 0 || !fileExists(cFile)) {
        r.detail = "pipeline failed";
        return r;
    }
    r.generatedC = readText(cFile);

    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        r.detail = "copy protected class failed: " + ec.message();
        return r;
    }

    // 编译 DLL（VMC4 自包含 /TC + 运行时 ABI）。
    fs::path dll         = dir / (className + "_shuf.dll");
    fs::path obj         = dir / (className + "_shuf.obj");
    fs::path crtSrc      = fs::current_path() / "mj2x" / "mj2x-rt" / "src";
    fs::path runtimeObj  = dir / "mj2x_vmc4_runtime_shuf.obj";
    auto cl = runBat({
        "call " + q(tc.vcvars64) + " x64 >nul",
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(cFile) + " /I" + q(crtSrc) +
            " /I" + q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "cl /nologo /O2 /TC /utf-8 /c /Fo" + q(runtimeObj) + " " +
            q(crtSrc / "vmc4" / "mj2x_vmc4_runtime.c") + " /I" + q(crtSrc) + " /I" +
            q(tc.jdkInclude) + " /I" + q(tc.jdkIncludeWin),
        "link /nologo /DLL /OUT:" + q(dll) + " " + q(obj) + " " + q(runtimeObj),
    });
    if (cl.exitCode != 0 || !fileExists(dll)) {
        r.detail = "cl/link failed: " + cl.out;
        return r;
    }

    // -Xverify:all 真编译执行（逐行与 baseline 对拍在 TEST 侧）。
    auto protectedRun = runBat({
        "set \"MORPHER_DLL=" + dll.string() + "\"",
        q(tc.java) + " -Xverify:all -cp " + q(dir) + " Driver",
    });
    if (protectedRun.exitCode != 0) {
        r.detail = "protected run failed: " + protectedRun.out;
        return r;
    }
    r.actual = splitLines(protectedRun.out);
    r.ok     = true;
    return r;
}

}   // namespace

/// shuffle 单开（drift/encrypt 关）：布局解耦独立生效。
TEST(Vmc4BackendShuffleE2E, ShuffleOnlyRoundTrip)
{
    Toolchain tc = probeToolchain();
    if (!tc.available) {
        GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc.reason;
    }
    auto r = runShuffleRoundTrip(tc, "Vmc4ShufA", 0, false);
    ASSERT_TRUE(r.ok) << r.detail;
    // 产物结构：targets 表密文 non-const（shuffle）；字节码数组保持 const 明文
    // （encrypt 关）；JNI_OnLoad 有 targets 解密调用（2 个方法各一条）。
    EXPECT_NE(r.generatedC.find("static unsigned int mj2x_vm_targets_"), std::string::npos)
        << r.generatedC;
    EXPECT_EQ(r.generatedC.find("static const unsigned int mj2x_vm_targets_"),
              std::string::npos);
    EXPECT_EQ(countOccurrences(r.generatedC,
                               "mj2x_vm_blob_decrypt((unsigned char*)mj2x_vm_targets_"), 2u);
    EXPECT_NE(r.generatedC.find("static const unsigned char mj2x_vm_code_"), std::string::npos);
    EXPECT_EQ(r.generatedC.find("static unsigned char mj2x_vm_code_"), std::string::npos);
    EXPECT_EQ(r.baseline.size(), r.actual.size())
        << "baseline:\n" << joinLines(r.baseline) << "protected:\n" << joinLines(r.actual);
    for (size_t i = 0; i < r.baseline.size() && i < r.actual.size(); ++i) {
        EXPECT_EQ(r.baseline[i], r.actual[i]) << "line " << i;
    }
}

/// shuffle + drift 叠加：执行解码层轮转与布局解耦正交。
TEST(Vmc4BackendShuffleE2E, ShufflePlusDriftRoundTrip)
{
    Toolchain tc = probeToolchain();
    if (!tc.available) {
        GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc.reason;
    }
    auto r = runShuffleRoundTrip(tc, "Vmc4ShufB", 2, false);
    ASSERT_TRUE(r.ok) << r.detail;
    // drift unmap 表 + shuffle targets 密文表共存。
    EXPECT_NE(r.generatedC.find("mj2x_vm_unmap_"), std::string::npos) << r.generatedC;
    EXPECT_NE(r.generatedC.find("static unsigned int mj2x_vm_targets_"), std::string::npos);
    EXPECT_EQ(r.generatedC.find("static const unsigned int mj2x_vm_targets_"),
              std::string::npos);
    EXPECT_EQ(r.baseline.size(), r.actual.size())
        << "baseline:\n" << joinLines(r.baseline) << "protected:\n" << joinLines(r.actual);
    for (size_t i = 0; i < r.baseline.size() && i < r.actual.size(); ++i) {
        EXPECT_EQ(r.baseline[i], r.actual[i]) << "line " << i;
    }
}

/// shuffle + drift + encrypt-program 三开关全开：编码层/执行层/落盘层正交叠加。
TEST(Vmc4BackendShuffleE2E, ShuffleDriftEncryptRoundTrip)
{
    Toolchain tc = probeToolchain();
    if (!tc.available) {
        GTEST_SKIP() << "VMC4 native E2E toolchain unavailable: " << tc.reason;
    }
    auto r = runShuffleRoundTrip(tc, "Vmc4ShufC", 2, true);
    ASSERT_TRUE(r.ok) << r.detail;
    // 三层形态共存：blob 密文 non-const（encrypt）+ targets 密文 non-const
    // （shuffle）+ drift unmap 表；两类解密调用各 2 条（每虚拟化方法一条）。
    EXPECT_NE(r.generatedC.find("static unsigned char mj2x_vm_code_"), std::string::npos);
    EXPECT_EQ(r.generatedC.find("static const unsigned char mj2x_vm_code_"), std::string::npos);
    EXPECT_NE(r.generatedC.find("static unsigned int mj2x_vm_targets_"), std::string::npos);
    EXPECT_EQ(r.generatedC.find("static const unsigned int mj2x_vm_targets_"),
              std::string::npos);
    EXPECT_NE(r.generatedC.find("mj2x_vm_unmap_"), std::string::npos);
    EXPECT_EQ(countOccurrences(r.generatedC, "mj2x_vm_blob_decrypt(mj2x_vm_code_"), 2u);
    EXPECT_EQ(countOccurrences(r.generatedC,
                               "mj2x_vm_blob_decrypt((unsigned char*)mj2x_vm_targets_"), 2u);
    EXPECT_EQ(r.baseline.size(), r.actual.size())
        << "baseline:\n" << joinLines(r.baseline) << "protected:\n" << joinLines(r.actual);
    for (size_t i = 0; i < r.baseline.size() && i < r.actual.size(); ++i) {
        EXPECT_EQ(r.baseline[i], r.actual[i]) << "line " << i;
    }
}

}   // namespace

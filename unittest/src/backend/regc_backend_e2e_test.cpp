// RegC native E2E harness: Java source -> javac -> RegCBackend -> .c -> DLL
// -> Java loads the DLL and calls the protected native methods.
//
// This is intentionally narrower than StackCBackendE2E.  It grows with the RegC
// runtime-helper surface and avoids call-helper cases until that ABI is wired.

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
#include "libca/process/subprocess.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/init_clinit_mover_preprocessor.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / JdkToolchain / probeJdk

namespace {

// mj2x RegC E2E 工具链：在 test::jdk::JdkToolchain 之上加 vcvars64（RegC 直接调库，
// 不需要 mj2x-cli）。runBat / q / fileExists / ExecResult / probeJdk 复用 test::jdk。
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

Toolchain probeToolchain()
{
    Toolchain tc;
    // JDK：test::jdk::probeJdk 底层走 libjdk_finder，并校验 jni.h。
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

    // vcvars64：复用 libjdk_finder 的探测。
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

class RegCBackendE2E : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "RegC native E2E toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/regc_e2e/" + name + "/.keep").parent_path();
    }

    void runCase(const std::string& className, const std::string& javaSource,
                 const std::vector<JavaCheck>& checks)
    {
        fs::path        dir = caseDir(className);
        std::error_code ec;
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
        auto           backend = std::make_unique<mj2x::RegCBackend>();
        backend->setOutputPath(cFile.string());
        pipeline.registerBackend(std::move(backend));
        // R6.1：先展开 StringConcatFactory invokedynamic → StringBuilder 字节码，
        // 再 mark-all，使 concat 方法能进入 RegC native 流程。
        pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
        // 搬运法（regc_conversion_decisions.md §7.1）：<init>/<clinit> 体先进
        // __regc_* 静态方法（MarkAll 标记后编译成 native），骨架保留字节码。
        pipeline.registerPreprocessor(std::make_unique<mj2x::InitClinitMoverPreprocessor>());
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
        // RegC 用 RegisterNatives 动态绑定，需要 mj2x/cloader/Loader 桥接类
        // （JNI_OnLoad 绑其 registerNativesForClass 到 _mj2x_register_for_class）。
        pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
        ASSERT_EQ(pipeline.run(), 0);
        ASSERT_TRUE(fileExists(cFile));
        std::string generatedC = readText(cFile);
        EXPECT_NE(generatedC.find("#include \"regc/mj2x_regc_crt.h\""), std::string::npos)
            << generatedC;
        EXPECT_EQ(generatedC.find("static int32_t mj2x_regc_div_i32"), std::string::npos)
            << generatedC;
        EXPECT_EQ(generatedC.find("static void _mj2x_regc_barrier"), std::string::npos)
            << generatedC;
        EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_int_compare"), std::string::npos)
            << generatedC;

        fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        fs::path dll    = dir / (className + ".dll");
        fs::path objDir = dir / "obj";
        fs::create_directories(objDir, ec);
        ASSERT_FALSE(ec) << ec.message();
        fs::path generatedObj = objDir / "generated.obj";
        fs::path runtimeSrc   = fs::current_path() / "mj2x" / "mj2x-runtime" / "src";
        fs::path crtSrc       = fs::current_path() / "mj2x" / "mj2x-crt" / "src";

        // Generated RegC includes the distributable CRT ABI directly. The
        // remaining runtime sources only back non-CRT Math/Random/shadow
        // intrinsic surfaces.
        std::vector<std::string> compileLines;
        // vcvarsall.bat 需带 x64 参数（libjdk_finder 返回 vcvarsall.bat）。
        compileLines.push_back("call " + q(tc_.vcvars64) + " x64 >nul");
        compileLines.push_back("cl /nologo /O2 /utf-8 /c /Fo" + q(generatedObj) + " " + q(cFile) +
                               " /I" + q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                               q(tc_.jdkIncludeWin));

        std::vector<fs::path> runtimeObjs;
        auto                  compileRuntime = [&](const std::string& relPath,
                                  const std::string& objName,
                                  const std::string& extraFlags = "") {
            fs::path src = runtimeSrc / relPath;
            ASSERT_TRUE(fileExists(src)) << src;
            fs::path obj = objDir / objName;
            runtimeObjs.push_back(obj);
            compileLines.push_back("cl /nologo /O2 /c /Fo" + q(obj) + " " + q(src) +
                                   (extraFlags.empty() ? "" : " " + extraFlags) + " /I" +
                                   q(runtimeSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                                   q(tc_.jdkIncludeWin));
        };
        auto compileCrt = [&](const std::string& relPath, const std::string& objName) {
            fs::path src = crtSrc / relPath;
            ASSERT_TRUE(fileExists(src)) << src;
            fs::path obj = objDir / objName;
            runtimeObjs.push_back(obj);
            compileLines.push_back("cl /nologo /O2 /TC /utf-8 /c /Fo" + q(obj) + " " + q(src) + " /I" +
                                   q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                                   q(tc_.jdkIncludeWin));
        };
        compileCrt("regc/mj2x_regc_crt.c", "mj2x_regc_crt.obj");
        compileCrt("regc/mj2x_regc_safe_env.c", "mj2x_regc_safe_env.obj");
        compileCrt("regc/mj2x_regc_string.c", "mj2x_regc_string.obj");
        compileCrt("regc/mj2x_regc_string_builder.c", "mj2x_regc_string_builder.obj");
        // 权威 wrapper/Math/Random/位操作 intrinsic 全部在 crt 的
        // mj2x_regc_intrinsic.c（可分发 C99 版，生成工程同源）；runtime 的
        // intrinsic_wrapper/intrinsic_math/intrinsic_random 是历史 in-tree 版，
        // 与 crt 重复定义，不再链接。
        compileCrt("regc/mj2x_regc_intrinsic.c", "mj2x_regc_intrinsic.obj");

        std::string linkLine = "link /nologo /DLL /OUT:" + q(dll) + " " + q(generatedObj);
        for (const auto& obj : runtimeObjs) linkLine += " " + q(obj);
        compileLines.push_back(linkLine);

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
            EXPECT_EQ(baseline[i], actual[i]);
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

Toolchain RegCBackendE2E::tc_;

}   // namespace

TEST(RegCBackendE2EHarnessTest, RunBatStopsAfterFirstFailingCommand)
{
    fs::path        marker = test::getTempOutPath("mj2x_regc_e2e_fail_fast_marker.txt");
    std::error_code ec;
    fs::remove(marker, ec);

    auto result = runBat({
        "cmd /c exit /b 7",
        "echo stale-object-risk > " + q(marker),
    });

    EXPECT_NE(result.exitCode, 0) << result.out;
    EXPECT_FALSE(fileExists(marker)) << marker;
}

TEST_F(RegCBackendE2E, PureArithmeticLoopAndArrayRoundTrip)
{
    runCase("RegCNativePure",
            "public class RegCNativePure {\n"
            "  public static int addMul(int a, int b) { return (a + b) * 3; }\n"
            "  public static int loopSum(int n) {\n"
            "    int acc = 0;\n"
            "    for (int i = 0; i < n; i++) acc += i;\n"
            "    return acc;\n"
            "  }\n"
            "  public static int intArray(int n) {\n"
            "    int[] values = new int[3];\n"
            "    values[0] = n;\n"
            "    values[1] = n + 1;\n"
            "    values[2] = values[0] + values[1];\n"
            "    return values.length + values[2];\n"
            "  }\n"
            "}\n",
            {
                {"addMul(3, 4)", "RegCNativePure.addMul(3, 4)"},
                {"addMul(2147483647, 1)", "RegCNativePure.addMul(2147483647, 1)"},
                {"loopSum(0)", "RegCNativePure.loopSum(0)"},
                {"loopSum(7)", "RegCNativePure.loopSum(7)"},
                {"intArray(5)", "RegCNativePure.intArray(5)"},
            });
}

TEST_F(RegCBackendE2E, RandomNextIntBoundTrapRoundTrip)
{
    // Random.nextInt(bound) 切片的 fail-closed 预检（Y4）：JVM 对 bound<=0 抛
    // IllegalArgumentException；crt 直调（mj2x_random_next_int_bound）无异常
    // 通道（旧实现 bound<=0 返回 INT32_MIN 哨兵 = fail-open），发射端预检把
    // 异常挂到 context，由调用后异常分发/JNI 边界统一处理。正常 bound 的
    // 结果与 JVM 逐位一致（标准 Java LCG）。
    runCase("RegCRandomBound",
            "public class RegCRandomBound {\n"
            "  public static int okBound(long seed, int bound) {\n"
            "    return new java.util.Random(seed).nextInt(bound);\n"
            "  }\n"
            "  public static int trap(long seed, int bound) {\n"
            "    try {\n"
            "      return new java.util.Random(seed).nextInt(bound);\n"
            "    } catch (IllegalArgumentException e) {\n"
            "      return -1;\n"
            "    }\n"
            "  }\n"
            "  public static int trapInTry(long seed, int bound) {\n"
            "    int sink = 0;\n"
            "    try {\n"
            "      sink = new java.util.Random(seed).nextInt(bound);\n"
            "    } catch (IllegalArgumentException e) {\n"
            "      sink = -2;\n"
            "    }\n"
            "    return sink;\n"
            "  }\n"
            "}\n",
            {
                {"okBound(42, 100)", "RegCRandomBound.okBound(42L, 100)"},
                {"okBound(7, 16)", "RegCRandomBound.okBound(7L, 16)"},
                {"trap(42, 0)", "RegCRandomBound.trap(42L, 0)"},
                {"trap(42, -3)", "RegCRandomBound.trap(42L, -3)"},
                {"trapInTry(42, 0)", "RegCRandomBound.trapInTry(42L, 0)"},
                {"trapInTry(42, 10)", "RegCRandomBound.trapInTry(42L, 10)"},
            });
}

TEST_F(RegCBackendE2E, FieldAndReferenceHelpersRoundTrip)
{
    runCase("RegCNativeHelpers",
            "public class RegCNativeHelpers {\n"
            "  public int value;\n"
            "  public static int sink;\n"
            "  public int instanceField(int x) {\n"
            "    value = x;\n"
            "    return value + 1;\n"
            "  }\n"
            "  public static int staticField(int x) {\n"
            "    sink = x * 2;\n"
            "    return sink + 3;\n"
            "  }\n"
            "  public static int isString(Object value) {\n"
            "    return value instanceof String ? 1 : 0;\n"
            "  }\n"
            "}\n",
            {
                {"instanceField(5)", "new RegCNativeHelpers().instanceField(5)"},
                {"staticField(7)", "RegCNativeHelpers.staticField(7)"},
                {"isString(\"x\")", "RegCNativeHelpers.isString(\"x\")"},
                {"isString(new Object())", "RegCNativeHelpers.isString(new Object())"},
            });
}

TEST_F(RegCBackendE2E, MultiArrayHelpersRoundTrip)
{
    runCase("RegCMultiArrayHelpers",
            "public class RegCMultiArrayHelpers {\n"
            "  public static int cube(int a, int b, int c) {\n"
            "    int[][][] values = new int[a][b][c];\n"
            "    values[a - 1][b - 1][c - 1] = a + b + c;\n"
            "    return values.length + values[0].length + values[0][0].length + values[a - 1][b - "
            "1][c - 1];\n"
            "  }\n"
            "}\n",
            {
                {"cube(2, 3, 4)", "RegCMultiArrayHelpers.cube(2, 3, 4)"},
            });
}

TEST_F(RegCBackendE2E, SystemArraycopyRoundTrip)
{
    runCase("RegCNativeArrayCopy",
            "public class RegCNativeArrayCopy {\n"
            "  public static int copyInts(int n) {\n"
            "    int[] src = new int[4];\n"
            "    for (int i = 0; i < 4; i++) src[i] = i * 10 + n;\n"
            "    int[] dst = new int[4];\n"
            "    System.arraycopy(src, 1, dst, 0, 3);\n"
            "    return dst[0] + dst[1] + dst[2] + dst[3];\n"
            "  }\n"
            "  public static int overlapCopy() {\n"
            "    int[] a = {1, 2, 3, 4, 5};\n"
            "    System.arraycopy(a, 0, a, 1, 4);\n"
            "    return a[0] * 10000 + a[1] * 1000 + a[2] * 100 + a[3] * 10 + a[4];\n"
            "  }\n"
            "  public static long copyLongs(int n) {\n"
            "    long[] src = {n, n + 1, n + 2};\n"
            "    long[] dst = new long[3];\n"
            "    System.arraycopy(src, 0, dst, 0, 3);\n"
            "    return dst[0] + dst[1] + dst[2];\n"
            "  }\n"
            "  public static int copyBytes(int n) {\n"
            "    byte[] src = {1, 2, 3};\n"
            "    byte[] dst = new byte[3];\n"
            "    System.arraycopy(src, 1, dst, 0, 2);\n"
            "    return dst[0] + dst[1] + dst[2];\n"
            "  }\n"
            "  public static int copyChars() {\n"
            "    char[] src = {'a', 'b', 'c'};\n"
            "    char[] dst = new char[3];\n"
            "    System.arraycopy(src, 2, dst, 0, 1);\n"
            "    return dst[0];\n"
            "  }\n"
            "  public static int nullSource() {\n"
            "    try { System.arraycopy(null, 0, new int[1], 0, 1); return 0; }\n"
            "    catch (NullPointerException e) { return 1; }\n"
            "  }\n"
            "  public static int negativeLen() {\n"
            "    try { System.arraycopy(new int[2], 0, new int[2], 0, -1); return 0; }\n"
            "    catch (IndexOutOfBoundsException e) { return 2; }\n"
            "  }\n"
            "  public static int outOfRange() {\n"
            "    try { System.arraycopy(new int[2], 0, new int[2], 0, 3); return 0; }\n"
            "    catch (IndexOutOfBoundsException e) { return 3; }\n"
            "  }\n"
            "  public static int objectArrayFallback(int n) {\n"
            "    String[] src = {\"a\", \"b\", \"c\"};\n"
            "    Object[] dst = new Object[3];\n"
            "    System.arraycopy(src, 0, dst, 0, 3);\n"
            "    return dst[2].toString().length();\n"
            "  }\n"
            "}\n",
            {
                {"copyInts(5)", "RegCNativeArrayCopy.copyInts(5)"},
                {"copyInts(0)", "RegCNativeArrayCopy.copyInts(0)"},
                {"overlapCopy()", "RegCNativeArrayCopy.overlapCopy()"},
                {"copyLongs(7)", "RegCNativeArrayCopy.copyLongs(7)"},
                {"copyBytes(0)", "RegCNativeArrayCopy.copyBytes(0)"},
                {"copyChars()", "RegCNativeArrayCopy.copyChars()"},
                {"nullSource()", "RegCNativeArrayCopy.nullSource()"},
                {"negativeLen()", "RegCNativeArrayCopy.negativeLen()"},
                {"outOfRange()", "RegCNativeArrayCopy.outOfRange()"},
                {"objectArrayFallback(0)", "RegCNativeArrayCopy.objectArrayFallback(0)"},
            });
}

TEST_F(RegCBackendE2E, CallHelpersRoundTrip)
{
    runCase("RegCCallHelpers",
            "public class RegCCallHelpers {\n"
            "  public int value;\n"
            "  public int addValue(int x) { return value + x; }\n"
            "  public static int twice(int x) { return x * 2; }\n"
            "  public static int staticCaller(int x) { return twice(x) + 1; }\n"
            "  public static int instanceCaller(int x) {\n"
            "    RegCCallHelpers box = new RegCCallHelpers();\n"
            "    box.value = 4;\n"
            "    return box.addValue(x);\n"
            "  }\n"
            "}\n",
            {
                {"staticCaller(9)", "RegCCallHelpers.staticCaller(9)"},
                {"instanceCaller(5)", "RegCCallHelpers.instanceCaller(5)"},
            });
}

// R6.1 构造器/静态块搬运验证（regc_conversion_decisions.md §7.1 搬运法）：
// <init>/<clinit> 方法体被搬进 __regc_init_*/__regc_clinit_* 静态方法后经
// MarkAll 编成 native；原方法保留字节码骨架（super 调用链 / registerNatives 注入点）。
// javac 默认( -g:lines,source)在 <init> 头部生成 LineNumberTable 锚点 label，
// 正好覆盖 v1「允许切点前 LABEL/LINE、拒绝跨切点引用」的放宽边界。
TEST_F(RegCBackendE2E, InitAndClinitMoveRoundTrip)
{
    runCase("RegCNativeInit",
            "public class RegCNativeInit {\n"
            "  public int value;\n"
            "  public static int counter;\n"
            "  static {\n"
            "    counter = 100;\n"
            "  }\n"
            "  public RegCNativeInit(int x) {\n"
            "    value = x * 3;\n"
            "  }\n"
            "  public RegCNativeInit(int x, long y) {\n"
            "    value = x + (int) y;\n"
            "  }\n"
            "  public int getValue() { return value; }\n"
            "  public static int getCounter() { return counter; }\n"
            "  public static int readStatic() { return counter + new RegCNativeInit(5).value; }\n"
            "}\n",
            {
                {"staticBlock", "RegCNativeInit.getCounter()"},
                {"init1", "new RegCNativeInit(7).getValue()"},
                {"initLong", "new RegCNativeInit(2, 40L).getValue()"},
                {"readStatic", "RegCNativeInit.readStatic()"},
            });
}

// R6 反射子集放开 + JNI local ref 引用管理验证。这些方法调用 Object.getClass /
// java/lang/Class.* —— 放开黑名单前它们保留字节码路径不转 native；放开后转 native，
// 每次调用产生 Class/String 临时 local ref。loopNames 在循环内反复制造临时 ref，
// 验证 PushLocalFrame/PopLocalFrame 帧管理不撑爆 JNI 默认保证的 16 槽 local ref 表，
// 且 name()/simpleName() 的 ref 返回值经 PopLocalFrame promote 后仍有效。
TEST_F(RegCBackendE2E, ReflectionRoundTrip)
{
    runCase("RegCReflection",
            "public class RegCReflection {\n"
            "  public static String name(Object o) {\n"
            "    return o.getClass().getName();\n"
            "  }\n"
            "  public static String simpleName(Object o) {\n"
            "    return o.getClass().getSimpleName();\n"
            "  }\n"
            "  public static int nameLength(Object o) {\n"
            "    return o.getClass().getName().length();\n"
            "  }\n"
            "  public static int sameClass(Object a, Object b) {\n"
            "    return a.getClass() == b.getClass() ? 1 : 0;\n"
            "  }\n"
            "  // Loop repeatedly creates transient Class/String local refs to\n"
            "  // stress the JNI local ref table capacity.\n"
            "  public static int loopNames(int n) {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < n; i++) {\n"
            "      total += new RegCReflection().getClass().getName().length();\n"
            "    }\n"
            "    return total;\n"
            "  }\n"
            "}\n",
            {
                {"name(\"x\")", "RegCReflection.name(\"x\")"},
                {"name(new Object())", "RegCReflection.name(new Object())"},
                {"simpleName(new RegCReflection())",
                 "RegCReflection.simpleName(new RegCReflection())"},
                {"nameLength(\"x\")", "RegCReflection.nameLength(\"x\")"},
                {"sameClass(\"a\", \"b\")", "RegCReflection.sameClass(\"a\", \"b\")"},
                {"sameClass(\"a\", new Object())",
                 "RegCReflection.sameClass(\"a\", new Object())"},
                {"loopNames(64)", "RegCReflection.loopNames(64)"},
            });
}

// R6.1 阶段1：StringConcatFactory invokedynamic 展开验证。javac 把 "a"+x+"b"+... 编译成
// invokedynamic makeConcatWithConstants —— InvokeDynamicPreprocessor 把它改写成
// new StringBuilder + append 链 + toString 字节码，RegC 已有 StringBuilder intrinsic
// 支持故能转 native（不走 StackC 的 runtime bridge，绕开 RegC 缺 owner-class 障碍）。
// 覆盖多类型参数（int/long/String/null）、纯字面量、连续动态参数。
TEST_F(RegCBackendE2E, InvokeDynamicStringConcatRoundTrip)
{
    runCase("RegCConcat",
            "public class RegCConcat {\n"
            "  public static String join(int a, long b, String s) {\n"
            "    return \"a=\" + a + \",b=\" + b + \",s=\" + s;\n"
            "  }\n"
            "  public static String withNull(String s) { return \"n=\" + s; }\n"
            "  public static String adjacent(String x, String y) { return x + y; }\n"
            "  public static String types(boolean f, char c, double d) {\n"
            "    return \"f=\" + f + \",c=\" + c + \",d=\" + d;\n"
            "  }\n"
            "}\n",
            {
                {"join(7, 42L, \"ok\")", "RegCConcat.join(7, 42L, \"ok\")"},
                {"join(-1, 9223372036854775807L, \"x\")",
                 "RegCConcat.join(-1, 9223372036854775807L, \"x\")"},
                {"withNull(null)", "RegCConcat.withNull(null)"},
                {"adjacent(\"foo\", \"bar\")", "RegCConcat.adjacent(\"foo\", \"bar\")"},
                {"types(true, 'z', 3.5)", "RegCConcat.types(true, 'z', 3.5)"},
            });
}

// R6.1 阶段2：无状态 lambda（方法引用）展开验证。javac 把 `IntUnaryOperator op = X::base`
// 编译成 invokedynamic LambdaMetafactory.metafactory —— InvokeDynamicPreprocessor 合成一个
// 实现 IntUnaryOperator 的类（SAM 方法 invokestatic 转调 base），把 indy 改写成
// new 合成类/dup/<init>，并把私有 base 提权。RegC 转 native 时走通用 new/invoke 路径
// （不走 StackC runtime bridge，绕开 owner-class 障碍）。stateless 使用方法引用（无捕获）。
TEST_F(RegCBackendE2E, InvokeDynamicStatelessLambdaRoundTrip)
{
    runCase("RegCLambda",
            "import java.util.function.IntUnaryOperator;\n"
            "public class RegCLambda {\n"
            "  private static int base(int x) { return x + 3; }\n"
            "  public static int useRef(int x) {\n"
            "    IntUnaryOperator op = RegCLambda::base;\n"
            "    return op.applyAsInt(x);\n"
            "  }\n"
            "  public static int twice(int x) {\n"
            "    IntUnaryOperator op = RegCLambda::base;\n"
            "    return op.applyAsInt(op.applyAsInt(x));\n"
            "  }\n"
            "}\n",
            {
                {"useRef(9)", "RegCLambda.useRef(9)"},
                {"useRef(-3)", "RegCLambda.useRef(-3)"},
                {"twice(10)", "RegCLambda.twice(10)"},
            });
}

// R6.1 阶段2b：捕获 lambda 展开验证。`v -> v + delta` 捕获局部变量 delta —— javac 生成
// invokedynamic desc `(I)L...`（捕获值作 indy 参数）+ 合成方法 lambda$..(I I)I（捕获值在前）。
// InvokeDynamicPreprocessor 合成带 private final 字段的类，构造器接收捕获值存字段，SAM
// 方法 GETFIELD 取回后接 SAM 参数一起转调 impl。站点栈上捕获值先 xSTORE 到临时槽、再
// new/dup/顺序 xLOAD/invokespecial <init>(捕获描述符)。覆盖单捕获、多捕获、混合类型捕获。
TEST_F(RegCBackendE2E, InvokeDynamicCapturingLambdaRoundTrip)
{
    runCase("RegCCapture",
            "import java.util.function.IntUnaryOperator;\n"
            "import java.util.function.IntBinaryOperator;\n"
            "public class RegCCapture {\n"
            "  public static int addDelta(int x, int delta) {\n"
            "    IntUnaryOperator op = v -> v + delta;\n"
            "    return op.applyAsInt(x);\n"
            "  }\n"
            "  public static int affine(int x, int a, int b) {\n"
            "    IntUnaryOperator op = v -> a * v + b;\n"
            "    return op.applyAsInt(x);\n"
            "  }\n"
            "  public static int mixed(int x, long scale, int bias) {\n"
            "    IntUnaryOperator op = v -> (int)(scale * v) + bias;\n"
            "    return op.applyAsInt(x);\n"
            "  }\n"
            "  public static int combine(int a, int b, int c) {\n"
            "    IntBinaryOperator op = (u, v) -> u + v + c;\n"
            "    return op.applyAsInt(a, b);\n"
            "  }\n"
            "}\n",
            {
                {"addDelta(10, 5)", "RegCCapture.addDelta(10, 5)"},
                {"affine(4, 3, 7)", "RegCCapture.affine(4, 3, 7)"},
                {"mixed(6, 2L, 1)", "RegCCapture.mixed(6, 2L, 1)"},
                {"combine(2, 3, 100)", "RegCCapture.combine(2, 3, 100)"},
            });

    // 回归断言：javac 为捕获 lambda 生成的合成方法 lambda$xxx 必须被 RegC 编译为
    // native（InvokeDynamicPreprocessor 已把 indy 站点改写为合成类，lambda$ 体不再
    // 按方法名前缀 skip）。这里检查生成 C 里出现每个 lambda$ 方法的 regc_ helper 定义——
    // 一个被转 native 的方法会发射 `static ... regc_<Class>_lambda_<n>__<desc>` helper，
    // 被 skip 的方法则不会出现在 C 源里。方法名中 `$` 在 C 标识符里映射为 `_`。
    fs::path capDir  = caseDir("RegCCapture");
    fs::path capC    = capDir / "RegCCapture.c";
    ASSERT_TRUE(fileExists(capC)) << capC;
    std::string captureC = readText(capC);
    EXPECT_NE(captureC.find("regc_RegCCapture_lambda_addDelta_0__II_I"), std::string::npos)
        << captureC;
    EXPECT_NE(captureC.find("regc_RegCCapture_lambda_affine_1__III_I"), std::string::npos)
        << captureC;
    EXPECT_NE(captureC.find("regc_RegCCapture_lambda_mixed_2__JII_I"), std::string::npos)
        << captureC;
    EXPECT_NE(captureC.find("regc_RegCCapture_lambda_combine_3__III_I"), std::string::npos)
        << captureC;
}

// R6.1 阶段2c：实例 / 构造器方法引用展开验证。javac 为不同 method-handle 形态生成 indy：
//   String::length → H_INVOKEVIRTUAL（unbound 实例引用，receiver = SAM 首参）
//   String::charAt → H_INVOKEVIRTUAL（unbound + 方法参数）
//   base::concat   → H_INVOKEVIRTUAL（bound 实例引用，receiver 捕获入字段）
//   String::new    → H_NEWINVOKESPECIAL（构造器引用，NEW/DUP/args/INVOKESPECIAL <init>）
// InvokeDynamicPreprocessor 按 tag 合成对应 SAM 体，RegC 走通用 new/invokeinterface 路径
// 构造合成对象并派发（与阶段2/2b 相同机制）。用非泛型自定义 functional interface
// （erased==instantiated）规避装箱/checkcast 缺口，impl 目标均为 JDK 方法（合成 SAM 以纯
// 字节码转调 JDK，故不要求 impl 目标本身可 native 化）。
TEST_F(RegCBackendE2E, InvokeDynamicMethodRefRoundTrip)
{
    runCase("RegCMethodRef",
            "public class RegCMethodRef {\n"
            // functional interface 必须 public：合成类位于 mj2x.lambda 包，跨包实现
            // 包私有接口会在运行期抛 IllegalAccessError（JVM 仅放行 public 跨包访问）。
            "  public interface StrLen { int len(String s); }\n"
            "  public interface CharPick { char pick(String s, int i); }\n"
            "  public interface StrFromChars { String make(char[] c); }\n"
            "  public interface Prefixer { String apply(String s); }\n"
            "  public static int lenOf(String s) {\n"
            "    StrLen f = String::length;\n"
            "    return f.len(s);\n"
            "  }\n"
            "  public static char charOf(String s, int i) {\n"
            "    CharPick f = String::charAt;\n"
            "    return f.pick(s, i);\n"
            "  }\n"
            "  public static String ctorFromChars(char a, char b, char c) {\n"
            "    StrFromChars f = String::new;\n"
            "    return f.make(new char[]{a, b, c});\n"
            "  }\n"
            "  public static String boundConcat(String base, String suffix) {\n"
            "    Prefixer p = base::concat;\n"
            "    return p.apply(suffix);\n"
            "  }\n"
            "}\n",
            {
                {"lenOf(\"hello\")", "RegCMethodRef.lenOf(\"hello\")"},
                {"charOf(\"world\",1)", "RegCMethodRef.charOf(\"world\", 1)"},
                {"ctorFromChars(abc)", "RegCMethodRef.ctorFromChars('a', 'b', 'c')"},
                {"boundConcat(foo-,bar)", "RegCMethodRef.boundConcat(\"foo-\", \"bar\")"},
            });
}

// R6.1 阶段2d：装箱/桥接适配验证。泛型 functional interface（Function/Supplier/Predicate/
// ToIntFunction）经类型擦除后 samMethodType 形参/返回是 Object，与 impl 的具体/基本类型不同，
// javac 为此生成的 indy 此前 fail-closed。展开时在合成 SAM 体静态发射适配字节码：
//   Math::abs → CHECKCAST Integer + intValue 拆箱、返回 Integer.valueOf 装箱
//   String::length / String::isEmpty → receiver 从 Object CHECKCAST 到 String
//   String::valueOf(long) → CHECKCAST Long + longValue 拆箱
//   String::new (Supplier) → 构造器引用 + 擦除返回
//   x -> x + base → 捕获 + 擦除参数 CHECKCAST（impl 为 javac 合成 lambda$ 方法）
TEST_F(RegCBackendE2E, InvokeDynamicErasedGenericRoundTrip)
{
    runCase("RegCIndyBoxing",
            "import java.util.function.Function;\n"
            "import java.util.function.Predicate;\n"
            "import java.util.function.Supplier;\n"
            "import java.util.function.ToIntFunction;\n"
            "public class RegCIndyBoxing {\n"
            "  public static int mapAbs(int x) {\n"
            "    Function<Integer, Integer> f = Math::abs;\n"
            "    return f.apply(x);\n"
            "  }\n"
            "  public static int strLen(String s) {\n"
            "    ToIntFunction<String> f = String::length;\n"
            "    return f.applyAsInt(s);\n"
            "  }\n"
            "  public static String longText(long v) {\n"
            "    Function<Long, String> f = String::valueOf;\n"
            "    return f.apply(v);\n"
            "  }\n"
            "  public static boolean blank(String s) {\n"
            "    Predicate<String> p = String::isEmpty;\n"
            "    return p.test(s);\n"
            "  }\n"
            "  public static int emptyLen() {\n"
            "    Supplier<String> s = String::new;\n"
            "    return s.get().length();\n"
            "  }\n"
            "  public static int addCaptured(int base) {\n"
            "    Function<Integer, Integer> f = x -> x + base;\n"
            "    return f.apply(7);\n"
            "  }\n"
            "}\n",
            {
                {"mapAbs(-5)", "RegCIndyBoxing.mapAbs(-5)"},
                {"strLen(\"hello\")", "RegCIndyBoxing.strLen(\"hello\")"},
                {"longText(1234567890123)", "RegCIndyBoxing.longText(1234567890123L)"},
                {"blank(\"\")", "RegCIndyBoxing.blank(\"\")"},
                {"blank(\"x\")", "RegCIndyBoxing.blank(\"x\")"},
                {"emptyLen()", "RegCIndyBoxing.emptyLen()"},
                {"addCaptured(35)", "RegCIndyBoxing.addCaptured(35)"},
            });
}

// R6.1 阶段2e：altMetafactory（serializable lambda）展开验证。序列化 lambda（functional
// interface 继承 Serializable）由 javac 编成 invokedynamic altMetafactory + FLAG_SERIALIZABLE。
// InvokeDynamicPreprocessor 合成的类追加 implements java/io/Serializable（marker，SAM 体不变），
// 靠默认序列化 round-trip 其 capN 字段（不复刻 JVM 原生 SerializedLambda 格式）。测试在同一
// 进程内 writeObject → readObject → invoke，验证 native 合成类与 JVM 原生 lambda 行为一致。
// 覆盖无状态方法引用与捕获 lambda 两种形态。
TEST_F(RegCBackendE2E, InvokeDynamicSerializableLambdaRoundTrip)
{
    runCase("RegCSerLambda",
            "import java.io.*;\n"
            "public class RegCSerLambda {\n"
            // functional interface 须 public（合成类跨包实现）且继承 Serializable。
            "  public interface IntOp extends Serializable { int apply(int x); }\n"
            "  private static int base(int x) { return x * 7 + 1; }\n"
            "  private static int reserialize(IntOp op, int x) throws Exception {\n"
            "    ByteArrayOutputStream bos = new ByteArrayOutputStream();\n"
            "    ObjectOutputStream oos = new ObjectOutputStream(bos);\n"
            "    oos.writeObject(op);\n"
            "    oos.close();\n"
            "    ObjectInputStream ois =\n"
            "        new ObjectInputStream(new ByteArrayInputStream(bos.toByteArray()));\n"
            "    IntOp back = (IntOp) ois.readObject();\n"
            "    return back.apply(x);\n"
            "  }\n"
            "  public static int roundTripRef(int x) throws Exception {\n"
            "    IntOp op = RegCSerLambda::base;\n"
            "    return reserialize(op, x);\n"
            "  }\n"
            "  public static int roundTripCapture(int x, int k) throws Exception {\n"
            "    IntOp op = v -> v + k;\n"
            "    return reserialize(op, x);\n"
            "  }\n"
            "}\n",
            {
                {"roundTripRef(6)", "RegCSerLambda.roundTripRef(6)"},
                {"roundTripCapture(10,5)", "RegCSerLambda.roundTripCapture(10, 5)"},
            });
}

// R6.1 阶段2f：基本类型加宽桥接展开验证。method reference 的实参/返回基本类型与 SAM 相差
// 一个 JLS 加宽（int→long/double、long→double、float→double）时，LambdaMetafactory 运行期
// 插入 i2l/i2d/l2d/f2d；InvokeDynamicPreprocessor 在合成 SAM 体静态发射这些转换字节码。
// 覆盖返回加宽（IntToLong/IntToDouble/LongToDouble/FloatToDouble）与实参加宽（I2L 首参）。
// 用非泛型自定义 functional interface（避免与阶段2d 装箱路径叠加），impl 均为私有静态（展开
// 后提权）。
TEST_F(RegCBackendE2E, InvokeDynamicPrimitiveWideningRoundTrip)
{
    runCase("RegCWiden",
            "public class RegCWiden {\n"
            "  public interface IntToLong    { long   apply(int x); }\n"
            "  public interface IntToDouble  { double apply(int x); }\n"
            "  public interface LongToDouble { double apply(long x); }\n"
            "  public interface FloatToDouble{ double apply(float x); }\n"
            "  public interface ArgWiden     { long   apply(int x); }\n"
            "  private static int   idI(int x)   { return x; }\n"
            "  private static long  idL(long x)  { return x; }\n"
            "  private static float idF(float x) { return x; }\n"
            "  private static long  echoLong(long v) { return v * 3; }\n"
            "  public static long   retI2L(int x)   { IntToLong f = RegCWiden::idI; return f.apply(x); }\n"
            "  public static double retI2D(int x)   { IntToDouble f = RegCWiden::idI; return f.apply(x); }\n"
            "  public static double retL2D(long x)  { LongToDouble f = RegCWiden::idL; return f.apply(x); }\n"
            "  public static double retF2D(float x) { FloatToDouble f = RegCWiden::idF; return f.apply(x); }\n"
            "  public static long   argI2L(int x)   { ArgWiden f = RegCWiden::echoLong; return f.apply(x); }\n"
            "}\n",
            {
                {"retI2L(7)", "RegCWiden.retI2L(7)"},
                {"retI2D(9)", "RegCWiden.retI2D(9)"},
                {"retL2D(1234567890123)", "RegCWiden.retL2D(1234567890123L)"},
                {"retF2D(2.5f)", "RegCWiden.retF2D(2.5f)"},
                {"argI2L(11)", "RegCWiden.argI2L(11)"},
            });
}

// R6-A IO 组放开验证。这些方法在 native 帧内调用 InputStream/ByteArrayInputStream 的
// read()/available()/read(byte[]) —— 放开黑名单前保留字节码；放开后转 native。
// read() 返回 int 单字节，read(byte[]) 返回读入数并经 ref-array 写入 byte[]，
// available() 返回 int。readAll() 反复构造 ByteArrayInputStream 制造临时 ref，验证
// 引用管理在 IO 路径同样不撑爆 local ref 表。
TEST_F(RegCBackendE2E, IOGroupRoundTrip)
{
    runCase("RegCIO",
            "import java.io.ByteArrayInputStream;\n"
            "import java.io.InputStream;\n"
            "public class RegCIO {\n"
            "  public static int readFirst(byte[] data) throws Exception {\n"
            "    InputStream in = new ByteArrayInputStream(data);\n"
            "    return in.read();\n"
            "  }\n"
            "  public static int availableAfterOpen(byte[] data) throws Exception {\n"
            "    InputStream in = new ByteArrayInputStream(data);\n"
            "    return in.available();\n"
            "  }\n"
            "  public static int readIntoBuffer(byte[] data) throws Exception {\n"
            "    InputStream in = new ByteArrayInputStream(data);\n"
            "    byte[] buf = new byte[data.length];\n"
            "    int n = in.read(buf);\n"
            // 折叠所有字节，使 read(byte[]) 的写入对结果可见。
            "    int sum = 0;\n"
            "    for (int i = 0; i < n; i++) sum += buf[i];\n"
            "    return sum;\n"
            "  }\n"
            "  public static int readAll(byte[] data, int rounds) throws Exception {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < rounds; i++) {\n"
            "      InputStream in = new ByteArrayInputStream(data);\n"
            "      total += in.read();\n"
            "    }\n"
            "    return total;\n"
            "  }\n"
            "}\n",
            {
                {"readFirst({65,66,67})", "RegCIO.readFirst(new byte[]{65,66,67})"},
                {"availableAfterOpen({1,2,3,4})",
                 "RegCIO.availableAfterOpen(new byte[]{1,2,3,4})"},
                {"readIntoBuffer({10,20,30})",
                 "RegCIO.readIntoBuffer(new byte[]{10,20,30})"},
                {"readAll({97}, 64)", "RegCIO.readAll(new byte[]{97}, 64)"},
            });
}

// R6-B 类加载组放开验证。这些方法在 native 帧内调用 ClassLoader.loadClass(String)、
// getResourceAsStream(String) —— 放开黑名单前保留字节码；放开后转 native。
// loadClass 返回 Class（ref），getResourceAsStream 返回 InputStream（ref），参数 String 也是
// ref，验证引用管理在类加载路径同样不撑爆 local ref 表，且 ref 返回值经 PopLocalFrame
// promote 后仍可用。loadAndCheckName 链式 getName() 验证抽象/具体类实例方法分派。
TEST_F(RegCBackendE2E, ClassLoaderGroupRoundTrip)
{
    runCase("RegCLoader",
            "public class RegCLoader {\n"
            "  public static String loadClassName(String name) throws Exception {\n"
            "    ClassLoader cl = ClassLoader.getSystemClassLoader();\n"
            "    return cl.loadClass(name).getName();\n"
            "  }\n"
            "  public static int resourceAvailable(String name) throws Exception {\n"
            "    ClassLoader cl = ClassLoader.getSystemClassLoader();\n"
            "    java.io.InputStream in = cl.getResourceAsStream(name);\n"
            // Driver 运行在 -cp dir，资源不存在时返回 null，available() 不可调；
            // 用 (in != null) ? 1 : 0 表达「资源是否存在」这一可观测的 int 结果。
            "    return in != null ? 1 : 0;\n"
            "  }\n"
            "  public static String systemLoaderName() throws Exception {\n"
            "    return ClassLoader.getSystemClassLoader().getClass().getName();\n"
            "  }\n"
            "}\n",
            {
                {"loadClassName(\"java.lang.String\")",
                 "RegCLoader.loadClassName(\"java.lang.String\")"},
                {"resourceAvailable(\"RegCLoader.class\")",
                 "RegCLoader.resourceAvailable(\"RegCLoader.class\")"},
                {"systemLoaderName()", "RegCLoader.systemLoaderName()"},
            });
}

// R6-C 内部类访问路径验证。RegisterNatives 动态绑定（PR #661）已让内部类的 native 方法
// 像普通类一样注册，函数名/符号导出与 JNI 查找脱钩。本用例验证内部类更深的语义路径：
//   - 非静态内部类持有 this$0 合成字段（指向外部类实例），其上的字段访问在 native 帧内
//     经 _mj2x_regc_load_*_field 走通。
//   - 内部类方法调用外部类的实例方法（Outer.outerMethod()），跨类实例调用经 cached
//     instance method 分派（含抽象 owner 修正后的 receiver-class 解析）。
//   - 内部类自身的实例方法（innerAdd）注册并经 RegisterNatives 绑定。
// Outer/Inner 两个类都进同一 .c，Driver 只调 Outer 的静态/实例入口。
TEST_F(RegCBackendE2E, InnerClassFieldAccessRoundTrip)
{
    runCase("RegCInnerOuter",
            "public class RegCInnerOuter {\n"
            "  public int base;\n"
            "  public RegCInnerOuter(int b) { base = b; }\n"
            "  public int outerMethod(int x) { return base + x; }\n"
            "  public static int viaInner(int b, int x) {\n"
            "    RegCInnerOuter outer = new RegCInnerOuter(b);\n"
            "    Inner inner = outer.new Inner();\n"
            "    return inner.innerAdd(x);\n"
            "  }\n"
            // 非静态内部类：访问外部类 base 字段 + 调用外部类 outerMethod。
            "  class Inner {\n"
            "    public int innerAdd(int x) {\n"
            "      return outerMethod(x) + RegCInnerOuter.this.base;\n"
            "    }\n"
            "  }\n"
            "}\n",
            {
                {"viaInner(10, 5)", "RegCInnerOuter.viaInner(10, 5)"},
                {"viaInner(100, 1)", "RegCInnerOuter.viaInner(100, 1)"},
            });
}

TEST_F(RegCBackendE2E, NarrowPrimitiveRoundTrip)
{
    runCase(
        "RegCNarrowHelpers",
        "public class RegCNarrowHelpers {\n"
        "  public byte b;\n"
        "  public short s;\n"
        "  public char c;\n"
        "  public boolean z;\n"
        "  public static byte sb;\n"
        "  public static short ss;\n"
        "  public static char sc;\n"
        "  public static boolean sz;\n"
        "  public int id(int v) { return v + 1; }\n"
        "  public int narrowFields(byte vb, short vs, char vc, boolean vz) {\n"
        "    b = vb;\n"
        "    s = vs;\n"
        "    c = vc;\n"
        "    z = vz;\n"
        "    return b + s + c;\n"
        "  }\n"
        "  public static int staticNarrowFields(byte vb, short vs, char vc, boolean vz) {\n"
        "    sb = vb;\n"
        "    ss = vs;\n"
        "    sc = vc;\n"
        "    sz = vz;\n"
        "    return sb + ss + sc;\n"
        "  }\n"
        "  public static int narrowArrays(byte vb, short vs, char vc, boolean vz) {\n"
        "    byte[] b = new byte[2];\n"
        "    boolean[] z = new boolean[2];\n"
        "    short[] s = new short[2];\n"
        "    char[] c = new char[2];\n"
        "    b[0] = vb;\n"
        "    z[1] = vz;\n"
        "    s[0] = vs;\n"
        "    c[0] = vc;\n"
        "    return b[0] + s[0] + c[0];\n"
        "  }\n"
        "  public static int castNarrow(int x) {\n"
        "    byte b = (byte)(x - 130);\n"
        "    short s = (short)(x * 3);\n"
        "    char c = (char)(x + 65);\n"
        "    return b + s + c;\n"
        "  }\n"
        "}\n",
        {
            {"narrowFields",
             "new RegCNarrowHelpers().narrowFields((byte)-12, (short)18, (char)70, true)"},
            {"staticNarrowFields",
             "RegCNarrowHelpers.staticNarrowFields((byte)-10, (short)20, (char)71, false)"},
            {"narrowArrays", "RegCNarrowHelpers.narrowArrays((byte)-8, (short)22, (char)72, true)"},
            {"castNarrow", "RegCNarrowHelpers.castNarrow(7)"},
        });
}

TEST_F(RegCBackendE2E, StackJoinTernaryRoundTrip)
{
    runCase(
        "RegCStackJoinTernary",
        "public class RegCStackJoinTernary {\n"
        "  public static int booleanTernary(boolean flag) { return flag ? 11 : 3; }\n"
        "  public static int intTernary(int value) { return (value & 1) != 0 ? value + 7 : value - "
        "5; }\n"
        "  public static Object referenceTernary(boolean flag, Object left, Object right) {\n"
        "    return flag ? left : right;\n"
        "  }\n"
        "}\n",
        {
            {"booleanTernary(true)", "RegCStackJoinTernary.booleanTernary(true)"},
            {"booleanTernary(false)", "RegCStackJoinTernary.booleanTernary(false)"},
            {"intTernary(6)", "RegCStackJoinTernary.intTernary(6)"},
            {"intTernary(7)", "RegCStackJoinTernary.intTernary(7)"},
            {"referenceTernary(true)", "RegCStackJoinTernary.referenceTernary(true, \"L\", \"R\")"},
            {"referenceTernary(false)",
             "RegCStackJoinTernary.referenceTernary(false, \"L\", \"R\")"},
        });
}

TEST_F(RegCBackendE2E, NullTrapHandlerRoundTrip)
{
    runCase("RegCNullTrapHandlers",
            "public class RegCNullTrapHandlers {\n"
            "  public int value;\n"
            "  public int id(int input) { return input + 1; }\n"
            "  public static int catchNullField() {\n"
            "    try { RegCNullTrapHandlers box = null; return box.value; }\n"
            "    catch (NullPointerException ex) { return 55; }\n"
            "  }\n"
            "  public static int catchNullCall() {\n"
            "    try { RegCNullTrapHandlers box = null; return box.id(1); }\n"
            "    catch (NullPointerException ex) { return 66; }\n"
            "  }\n"
            "  public static int catchNullArray() {\n"
            "    try { int[] values = null; return values[0]; }\n"
            "    catch (NullPointerException ex) { return 77; }\n"
            "  }\n"
            "}\n",
            {
                {"catchNullField()", "RegCNullTrapHandlers.catchNullField()"},
                {"catchNullCall()", "RegCNullTrapHandlers.catchNullCall()"},
                {"catchNullArray()", "RegCNullTrapHandlers.catchNullArray()"},
            });
}

TEST_F(RegCBackendE2E, ThrowHelperRoundTrip)
{
    runCase("RegCThrowHelpers",
            "public class RegCThrowHelpers {\n"
            "  public static int raise(int x) {\n"
            "    if (x == 0) return 7;\n"
            "    throw new IllegalStateException();\n"
            "  }\n"
            "}\n",
            {
                {"raise(0)", "RegCThrowHelpers.raise(0)"},
                {"raise(1)", "RegCThrowHelpers.raise(1)"},
            });
}

TEST_F(RegCBackendE2E, TryCatchDispatchRoundTrip)
{
    runCase("RegCExceptionDispatch",
            "public class RegCExceptionDispatch {\n"
            "  public static int catchOwn(int x) {\n"
            "    try {\n"
            "      if (x > 0) throw new IllegalArgumentException();\n"
            "      return 1;\n"
            "    } catch (IllegalArgumentException e) {\n"
            "      return 2;\n"
            "    }\n"
            "  }\n"
            "  public static int missCatch() {\n"
            "    try {\n"
            "      throw new IllegalStateException();\n"
            "    } catch (IllegalArgumentException e) {\n"
            "      return 3;\n"
            "    }\n"
            "  }\n"
            "  public static void fail() { throw new IllegalArgumentException(); }\n"
            "  public static int catchCall() {\n"
            "    try {\n"
            "      fail();\n"
            "      return 1;\n"
            "    } catch (IllegalArgumentException e) {\n"
            "      return 9;\n"
            "    }\n"
            "  }\n"
            "}\n",
            {
                {"catchOwn(0)", "RegCExceptionDispatch.catchOwn(0)"},
                {"catchOwn(1)", "RegCExceptionDispatch.catchOwn(1)"},
                {"missCatch()", "RegCExceptionDispatch.missCatch()"},
                {"catchCall()", "RegCExceptionDispatch.catchCall()"},
            });
}

TEST_F(RegCBackendE2E, IntegerDivisionTrapRoundTrip)
{
    runCase("RegCIntegerTraps",
            "public class RegCIntegerTraps {\n"
            "  public static int divOrCatch(int a, int b) {\n"
            "    try { return a / b; }\n"
            "    catch (ArithmeticException e) { return 77; }\n"
            "  }\n"
            "  public static int remOrCatch(int a, int b) {\n"
            "    try { return a % b; }\n"
            "    catch (ArithmeticException e) { return 88; }\n"
            "  }\n"
            "  public static int divPropagate(int a) { return 10 / a; }\n"
            "  public static int minDiv() { return Integer.MIN_VALUE / -1; }\n"
            "  public static int minRem() { return Integer.MIN_VALUE % -1; }\n"
            "  public static long longDivOrCatch(long a, long b) {\n"
            "    try { return a / b; }\n"
            "    catch (ArithmeticException e) { return 99L; }\n"
            "  }\n"
            "}\n",
            {
                {"divOrCatch(9, 3)", "RegCIntegerTraps.divOrCatch(9, 3)"},
                {"divOrCatch(9, 0)", "RegCIntegerTraps.divOrCatch(9, 0)"},
                {"remOrCatch(10, 4)", "RegCIntegerTraps.remOrCatch(10, 4)"},
                {"remOrCatch(10, 0)", "RegCIntegerTraps.remOrCatch(10, 0)"},
                {"divPropagate(2)", "RegCIntegerTraps.divPropagate(2)"},
                {"divPropagate(0)", "RegCIntegerTraps.divPropagate(0)"},
                {"minDiv()", "RegCIntegerTraps.minDiv()"},
                {"minRem()", "RegCIntegerTraps.minRem()"},
                {"longDivOrCatch(12L, 3L)", "RegCIntegerTraps.longDivOrCatch(12L, 3L)"},
                {"longDivOrCatch(12L, 0L)", "RegCIntegerTraps.longDivOrCatch(12L, 0L)"},
            });
}

// JDK intrinsic E2E: Integer.parseInt + Math.* + String hot paths run entirely
// in native code. The generated C must call mj2x_math_* /
// _mj2x_intrinsic_parse_int / _mj2x_intrinsic_string_* and must not dispatch
// Math/parseInt/wrapper/Random/String through the JNI call helper.
// Results match the original Java baseline (parseInt "42" -> 42, Math.sin via
// the same IEEE 754 sin both sides use, Random via the Java-compatible LCG).
TEST_F(RegCBackendE2E, IntrinsicMathAndParseIntRoundTrip)
{
    runCase(
        "RegCIntrinsic",
        "public class RegCIntrinsic {\n"
        "  public static int parseInt42() { return Integer.parseInt(\"42\"); }\n"
        "  public static int parseIntArg(String s) { return Integer.parseInt(s); }\n"
        "  public static int parseIntRadix(String s, int radix) { return Integer.parseInt(s, "
        "radix); }\n"
        "  public static long parseLongArg(String s) { return Long.parseLong(s); }\n"
        "  public static long parseLongRadix(String s, int radix) { return Long.parseLong(s, "
        "radix); }\n"
        "  public static short parseShortArg(String s) { return Short.parseShort(s); }\n"
        "  public static short parseShortRadix(String s, int radix) { return Short.parseShort(s, "
        "radix); }\n"
        "  public static byte parseByteArg(String s) { return Byte.parseByte(s); }\n"
        "  public static byte parseByteRadix(String s, int radix) { return Byte.parseByte(s, "
        "radix); }\n"
        "  public static boolean parseBooleanArg(String s) { return Boolean.parseBoolean(s); }\n"
        "  public static float parseFloatArg(String s) { return Float.parseFloat(s); }\n"
        "  public static double parseDoubleArg(String s) { return Double.parseDouble(s); }\n"
        "  public static int parseShortOverflow(String s) {\n"
        "    try { Short.parseShort(s); return 0; } catch (NumberFormatException expected) { "
        "return 1; }\n"
        "  }\n"
        "  public static int parseByteOverflow(String s) {\n"
        "    try { Byte.parseByte(s); return 0; } catch (NumberFormatException expected) { return "
        "1; }\n"
        "  }\n"
        "  public static String intString(int x) { return Integer.toString(x); }\n"
        "  public static String longString(long x) { return Long.toString(x); }\n"
        "  public static String byteString(byte x) { return Byte.toString(x); }\n"
        "  public static String shortString(short x) { return Short.toString(x); }\n"
        "  public static String boolString(boolean x) { return Boolean.toString(x); }\n"
        "  public static String charString(char x) { return Character.toString(x); }\n"
        "  public static String floatString(float x) { return Float.toString(x); }\n"
        "  public static String doubleString(double x) { return Double.toString(x); }\n"
        "  public static int intCompare(int a, int b) { return Integer.compare(a, b); }\n"
        "  public static int longCompare(long a, long b) { return Long.compare(a, b); }\n"
        "  public static int byteCompare(byte a, byte b) { return Byte.compare(a, b); }\n"
        "  public static int shortCompare(short a, short b) { return Short.compare(a, b); }\n"
        "  public static int boolCompare(boolean a, boolean b) { return Boolean.compare(a, b); }\n"
        "  public static int charCompare(char a, char b) { return Character.compare(a, b); }\n"
        "  public static int floatCompare(float a, float b) { return Float.compare(a, b); }\n"
        "  public static int doubleCompare(double a, double b) { return Double.compare(a, b); }\n"
        "  public static int literalCompare() { return Float.compare(-0.0f, 0.0f) + "
        "Double.compare(Double.NaN, 1.0d); }\n"
        "  public static double sinDemo(double x) { return Math.sin(x); }\n"
        "  public static double sqrtDemo(double x) { return Math.sqrt(x); }\n"
        "  public static int mathChain(int a, int b) {\n"
        "    return Math.max(a, b) + Math.abs(a - b);\n"
        "  }\n"
        "  public static long roundDemo(double x) { return Math.round(x); }\n"
        "  public static int boxedInt(int x) { return Integer.valueOf(x).intValue(); }\n"
        "  public static long boxedLong(long x) { return Long.valueOf(x).longValue(); }\n"
        "  public static Object boxedIntObject(int x) { return Integer.valueOf(x); }\n"
        "  private static Object wrapperSink;\n"
        "  public static String boxedIntField(int x) { wrapperSink = Integer.valueOf(x); return "
        "wrapperSink.toString(); }\n"
        "  public static String boxedIntArray(int x) { Object[] values = new Object[1]; values[0] "
        "= Integer.valueOf(x); return values[0].toString(); }\n"
        "  public static String consumeBox(Object value) { return value.toString(); }\n"
        "  public static String boxedIntUnknownCall(int x) { return "
        "consumeBox(Integer.valueOf(x)); }\n"
        "  public static int randomNextInt(long seed) {\n"
        "    java.util.Random r = new java.util.Random(seed);\n"
        "    return r.nextInt();\n"
        "  }\n"
        "  public static int randomNextBound(long seed, int bound) {\n"
        "    java.util.Random r = new java.util.Random(seed);\n"
        "    return r.nextInt(bound);\n"
        "  }\n"
        "  public static int stringLength(String s) { return s.length(); }\n"
        "  public static int stringCharAt(String s, int index) { return s.charAt(index); }\n"
        "  public static int stringHash(String s) { return s.hashCode(); }\n"
        "  public static int stringEquals(String lhs, Object rhs) { return lhs.equals(rhs) ? 1 : "
        "0; }\n"
        "  public static int stringContains(String s, String needle) { return s.contains(needle) ? "
        "1 : 0; }\n"
        "  public static int stringContainsSeq(String s, CharSequence needle) { return "
        "s.contains(needle) ? 1 : 0; }\n"
        "  public static String stringConcat(String s, String suffix) { return s.concat(suffix); "
        "}\n"
        "  public static String stringSubstringFrom(String s, int begin) { return "
        "s.substring(begin); }\n"
        "  public static String stringSubstringRange(String s, int begin, int end) { return "
        "s.substring(begin, end); }\n"
        "  public static int stringCompareTo(String s, String other) { return s.compareTo(other); "
        "}\n"
        "  public static int stringIndexOfChar(String s, int ch) { return s.indexOf(ch); }\n"
        "  public static int stringIndexOfCharFrom(String s, int ch, int from) { return "
        "s.indexOf(ch, from); }\n"
        "  public static int stringIndexOfString(String s, String needle) { return "
        "s.indexOf(needle); }\n"
        "  public static int stringIndexOfStringFrom(String s, String needle, int from) { return "
        "s.indexOf(needle, from); }\n"
        "  public static String stringReplace(String s, char oldChar, char newChar) { return "
        "s.replace(oldChar, newChar); }\n"
        "  public static String stringTrim(String s) { return s.trim(); }\n"
        "  public static String stringSplitLiteral(String s, String delimiter) {\n"
        "    return java.util.Arrays.toString(s.split(delimiter));\n"
        "  }\n"
        "  public static String stringSplitLimit(String s, String delimiter, int limit) {\n"
        "    return java.util.Arrays.toString(s.split(delimiter, limit));\n"
        "  }\n"
        "  public static int stringSplitInvalidRegex(String s) {\n"
        "    try { s.split(\"[\"); return 0; }\n"
        "    catch (java.util.regex.PatternSyntaxException expected) { return 1; }\n"
        "  }\n"
        "  public static int stringSubstringBounds(String s) {\n"
        "    try { s.substring(-1); return 0; } catch (StringIndexOutOfBoundsException expected) { "
        "return 1; }\n"
        "  }\n"
        "  public static final class Named {\n"
        "    public String toString() { return \"named\"; }\n"
        "  }\n"
        "  public static final class ThrowingName {\n"
        "    public String toString() { throw new IllegalStateException(\"boom\"); }\n"
        "  }\n"
        "  public static final class OddSequence implements CharSequence {\n"
        "    public int length() { return 4; }\n"
        "    public char charAt(int index) { return \"A\\u4e2dBC\".charAt(index); }\n"
        "    public CharSequence subSequence(int start, int end) { return "
        "\"A\\u4e2dBC\".substring(start, end); }\n"
        "    public String toString() { return \"WRONG\"; }\n"
        "  }\n"
        "  public static String stringValueOfInt(int value) { return String.valueOf(value); }\n"
        "  public static String stringValueOfLong(long value) { return String.valueOf(value); }\n"
        "  public static String stringValueOfBoolean(boolean value) { return "
        "String.valueOf(value); }\n"
        "  public static String stringValueOfChar(char value) { return String.valueOf(value); }\n"
        "  public static String stringValueOfFloat(float value) { return String.valueOf(value); }\n"
        "  public static String stringValueOfDouble(double value) { return String.valueOf(value); "
        "}\n"
        "  public static String stringValueOfObject(Object value) { return String.valueOf(value); "
        "}\n"
        "  public static String stringBuilderChain(String s, int x) {\n"
        "    return new "
        "StringBuilder().append(s).append(x).append(7L).append(true).append('!').toString();\n"
        "  }\n"
        "  public static String stringBuilderFromString(String s) {\n"
        "    return new StringBuilder(s).append(\"!\").toString();\n"
        "  }\n"
        "  public static String stringBuilderObject(Object value) {\n"
        "    return new StringBuilder().append(value).toString();\n"
        "  }\n"
        "  public static String stringBuilderCharSequence(CharSequence value) {\n"
        "    return new StringBuilder().append(value).toString();\n"
        "  }\n"
        "  public static String stringBuilderChars(char[] value) {\n"
        "    return new StringBuilder().append(value).toString();\n"
        "  }\n"
        "  public static String stringBuilderFloat(float value) {\n"
        "    return new StringBuilder().append(value).toString();\n"
        "  }\n"
        "  public static String stringBuilderDouble(double value) {\n"
        "    return new StringBuilder().append(value).toString();\n"
        "  }\n"
        "  public static String stringBuilderRange(char[] chars) {\n"
        "    return new StringBuilder(2).append(new OddSequence()).append(new OddSequence(), 1, "
        "3)\n"
        "      .append(chars, 1, 2).appendCodePoint(0x1f600).toString();\n"
        "  }\n"
        "  public static int stringBuilderCapacity() {\n"
        "    StringBuilder value = new StringBuilder(2); value.append(\"abc\");\n"
        "    int grown = value.capacity(); value.ensureCapacity(20); int ensured = "
        "value.capacity();\n"
        "    value.setLength(5); value.setCharAt(3, 'X'); value.trimToSize();\n"
        "    return grown * 10000 + ensured * 100 + value.capacity() * 10 + value.charAt(3);\n"
        "  }\n"
        "  public static String stringBuilderFromSequence() {\n"
        "    return new StringBuilder(new OddSequence()).toString();\n"
        "  }\n"
        "  public static String stringBuilderExceptionCleanup() {\n"
        "    try { return new StringBuilder().append(new ThrowingName()).toString(); }\n"
        "    catch (IllegalStateException expected) { return \"caught\"; }\n"
        "  }\n"
        "  public static StringBuilder stringBuilderReturn(String value) {\n"
        "    return new StringBuilder().append(value);\n"
        "  }\n"
        "  public static StringBuilder stringBuilderCapacityReturn() {\n"
        "    StringBuilder value = new StringBuilder(3); value.ensureCapacity(40); return value;\n"
        "  }\n"
        "  public static String consumeBuilder(Object value) { return value.toString(); }\n"
        "  public static String stringBuilderUnknownCall(String value) {\n"
        "    return consumeBuilder(new StringBuilder().append(value));\n"
        "  }\n"
        "  private static Object builderSink;\n"
        "  public static String stringBuilderFieldEscape(String value) {\n"
        "    builderSink = new StringBuilder().append(value); return builderSink.toString();\n"
        "  }\n"
        "  public static String stringBuilderArrayEscape(String value) {\n"
        "    Object[] values = new Object[1]; values[0] = new StringBuilder().append(value);\n"
        "    return values[0].toString();\n"
        "  }\n"
        "  public static String stringBufferChain(String value, char[] chars) {\n"
        "    return new StringBuffer(1).append(value).append(new OddSequence(), 1, 3)\n"
        "      .append(chars, 0, 2).appendCodePoint(0x1f600).toString();\n"
        "  }\n"
        "  public static int stringBufferCapacity() {\n"
        "    StringBuffer value = new StringBuffer(1); value.append(\"ab\"); "
        "value.ensureCapacity(12);\n"
        "    value.setLength(4); value.setCharAt(2, 'Z'); value.trimToSize();\n"
        "    return value.capacity() * 100 + value.length() * 10 + value.charAt(2);\n"
        "  }\n"
        "  public static StringBuffer stringBufferReturn(String value) {\n"
        "    return new StringBuffer().append(value);\n"
        "  }\n"
        "  public static StringBuffer stringBufferCapacityReturn() {\n"
        "    StringBuffer value = new StringBuffer(3); value.ensureCapacity(40); return value;\n"
        "  }\n"
        "  public static String stringBufferUnknownCall(String value) {\n"
        "    return consumeBuilder(new StringBuffer().append(value));\n"
        "  }\n"
        "  public static String stringBufferUseAfterEscape(String value) {\n"
        "    StringBuffer buffer = new StringBuffer().append(value); consumeBuilder(buffer);\n"
        "    return buffer.append('!').toString();\n"
        "  }\n"
        "}\n",
        {
            {"parseInt42()", "RegCIntrinsic.parseInt42()"},
            {"parseIntArg(\"-123\")", "RegCIntrinsic.parseIntArg(\"-123\")"},
            {"parseIntRadix(\"7fffffff\", 16)", "RegCIntrinsic.parseIntRadix(\"7fffffff\", 16)"},
            {"parseIntRadix(\"-80000000\", 16)", "RegCIntrinsic.parseIntRadix(\"-80000000\", 16)"},
            {"parseLongArg(\"-1234567890123\")", "RegCIntrinsic.parseLongArg(\"-1234567890123\")"},
            {"parseLongRadix(\"zzzzzz\", 36)", "RegCIntrinsic.parseLongRadix(\"zzzzzz\", 36)"},
            {"parseLongRadix(\"-8000000000000000\", 16)",
             "RegCIntrinsic.parseLongRadix(\"-8000000000000000\", 16)"},
            {"parseShortArg(\"-32768\")", "RegCIntrinsic.parseShortArg(\"-32768\")"},
            {"parseShortRadix(\"7fff\", 16)", "RegCIntrinsic.parseShortRadix(\"7fff\", 16)"},
            {"parseByteArg(\"-128\")", "RegCIntrinsic.parseByteArg(\"-128\")"},
            {"parseByteRadix(\"7f\", 16)", "RegCIntrinsic.parseByteRadix(\"7f\", 16)"},
            {"parseBooleanArg(\"TrUe\")", "RegCIntrinsic.parseBooleanArg(\"TrUe\")"},
            {"parseBooleanArg(null)", "RegCIntrinsic.parseBooleanArg(null)"},
            {"parseFloatArg(\"-0.0\")", "RegCIntrinsic.parseFloatArg(\"-0.0\")"},
            {"parseFloatArg(\"NaN\")", "RegCIntrinsic.parseFloatArg(\"NaN\")"},
            {"parseDoubleArg(\"0x1.8p2\")", "RegCIntrinsic.parseDoubleArg(\"0x1.8p2\")"},
            {"parseDoubleArg(\"Infinity\")", "RegCIntrinsic.parseDoubleArg(\"Infinity\")"},
            {"parseShortOverflow(\"32768\")", "RegCIntrinsic.parseShortOverflow(\"32768\")"},
            {"parseByteOverflow(\"128\")", "RegCIntrinsic.parseByteOverflow(\"128\")"},
            {"intString(-42)", "RegCIntrinsic.intString(-42)"},
            {"longString(1234567890123L)", "RegCIntrinsic.longString(1234567890123L)"},
            {"byteString((byte)-7)", "RegCIntrinsic.byteString((byte)-7)"},
            {"shortString((short)1234)", "RegCIntrinsic.shortString((short)1234)"},
            {"boolString(true)", "RegCIntrinsic.boolString(true)"},
            {"boolString(false)", "RegCIntrinsic.boolString(false)"},
            {"charString('Z')", "RegCIntrinsic.charString('Z')"},
            {"floatString(1.25f)", "RegCIntrinsic.floatString(1.25f)"},
            {"floatString(-0.0f)", "RegCIntrinsic.floatString(-0.0f)"},
            {"doubleString(1.25d)", "RegCIntrinsic.doubleString(1.25d)"},
            {"doubleString(Double.NaN)", "RegCIntrinsic.doubleString(Double.NaN)"},
            {"intCompare(3, 7)", "RegCIntrinsic.intCompare(3, 7)"},
            {"intCompare(9, 4)", "RegCIntrinsic.intCompare(9, 4)"},
            {"longCompare(9L, 9L)", "RegCIntrinsic.longCompare(9L, 9L)"},
            {"longCompare(-5L, 2L)", "RegCIntrinsic.longCompare(-5L, 2L)"},
            {"byteCompare((byte)-3, (byte)4)", "RegCIntrinsic.byteCompare((byte)-3, (byte)4)"},
            {"shortCompare((short)9, (short)-2)",
             "RegCIntrinsic.shortCompare((short)9, (short)-2)"},
            {"boolCompare(true, false)", "RegCIntrinsic.boolCompare(true, false)"},
            {"boolCompare(false, true)", "RegCIntrinsic.boolCompare(false, true)"},
            {"charCompare('A', 'Z')", "RegCIntrinsic.charCompare('A', 'Z')"},
            {"floatCompare(1.0f, 2.0f)", "RegCIntrinsic.floatCompare(1.0f, 2.0f)"},
            {"floatCompare(-0.0f, 0.0f)", "RegCIntrinsic.floatCompare(-0.0f, 0.0f)"},
            {"floatCompare(Float.NaN, 1.0f)", "RegCIntrinsic.floatCompare(Float.NaN, 1.0f)"},
            {"doubleCompare(2.0d, 1.0d)", "RegCIntrinsic.doubleCompare(2.0d, 1.0d)"},
            {"doubleCompare(-0.0d, 0.0d)", "RegCIntrinsic.doubleCompare(-0.0d, 0.0d)"},
            {"doubleCompare(Double.NaN, 1.0d)", "RegCIntrinsic.doubleCompare(Double.NaN, 1.0d)"},
            {"literalCompare()", "RegCIntrinsic.literalCompare()"},
            {"sinDemo(1.0)", "RegCIntrinsic.sinDemo(1.0)"},
            {"sqrtDemo(2.0)", "RegCIntrinsic.sqrtDemo(2.0)"},
            {"mathChain(3, 9)", "RegCIntrinsic.mathChain(3, 9)"},
            {"mathChain(-5, -2)", "RegCIntrinsic.mathChain(-5, -2)"},
            {"roundDemo(2.6)", "RegCIntrinsic.roundDemo(2.6)"},
            {"roundDemo(2.5)", "RegCIntrinsic.roundDemo(2.5)"},
            {"roundDemo(-2.5)", "RegCIntrinsic.roundDemo(-2.5)"},
            {"boxedInt(-99)", "RegCIntrinsic.boxedInt(-99)"},
            {"boxedLong(1234567890123L)", "RegCIntrinsic.boxedLong(1234567890123L)"},
            {"boxedIntObject(321).toString()", "RegCIntrinsic.boxedIntObject(321).toString()"},
            {"boxedIntField(322)", "RegCIntrinsic.boxedIntField(322)"},
            {"boxedIntArray(323)", "RegCIntrinsic.boxedIntArray(323)"},
            {"boxedIntUnknownCall(324)", "RegCIntrinsic.boxedIntUnknownCall(324)"},
            {"randomNextInt(42L)", "RegCIntrinsic.randomNextInt(42L)"},
            {"randomNextBound(42L, 37)", "RegCIntrinsic.randomNextBound(42L, 37)"},
            {"stringLength(\"abcdef\")", "RegCIntrinsic.stringLength(\"abcdef\")"},
            {"stringCharAt(\"abcdef\", 2)", "RegCIntrinsic.stringCharAt(\"abcdef\", 2)"},
            {"stringHash(\"abcdef\")", "RegCIntrinsic.stringHash(\"abcdef\")"},
            {"stringEquals(\"same\", \"same\")", "RegCIntrinsic.stringEquals(\"same\", \"same\")"},
            {"stringEquals(\"same\", \"other\")",
             "RegCIntrinsic.stringEquals(\"same\", \"other\")"},
            {"stringEquals(\"same\", null)", "RegCIntrinsic.stringEquals(\"same\", null)"},
            {"stringEquals(\"same\", new Object())",
             "RegCIntrinsic.stringEquals(\"same\", new Object())"},
            {"stringContains(\"abcdef\", \"cd\")",
             "RegCIntrinsic.stringContains(\"abcdef\", \"cd\")"},
            {"stringContains(\"abcdef\", \"xy\")",
             "RegCIntrinsic.stringContains(\"abcdef\", \"xy\")"},
            {"stringContainsSeq(\"abcdef\", new StringBuilder(\"cd\"))",
             "RegCIntrinsic.stringContainsSeq(\"abcdef\", new StringBuilder(\"cd\"))"},
            {"stringConcat(\"A\\u4e2d\", \"\\ud83d\\ude00\")",
             "RegCIntrinsic.stringConcat(\"A\\u4e2d\", \"\\ud83d\\ude00\")"},
            {"stringConcat(\"same\", \"\")", "RegCIntrinsic.stringConcat(\"same\", \"\")"},
            {"stringSubstringFrom(\"A\\u4e2d\\ud83d\\ude00Z\", 2)",
             "RegCIntrinsic.stringSubstringFrom(\"A\\u4e2d\\ud83d\\ude00Z\", 2)"},
            {"stringSubstringRange(\"A\\u4e2d\\ud83d\\ude00Z\", 1, 4)",
             "RegCIntrinsic.stringSubstringRange(\"A\\u4e2d\\ud83d\\ude00Z\", 1, 4)"},
            {"stringCompareTo(\"abc\", \"abd\")",
             "RegCIntrinsic.stringCompareTo(\"abc\", \"abd\")"},
            {"stringCompareTo(\"\\ud83d\\ude00\", \"\\ud83d\\ude01\")",
             "RegCIntrinsic.stringCompareTo(\"\\ud83d\\ude00\", \"\\ud83d\\ude01\")"},
            {"stringIndexOfChar(\"x\\ud83d\\ude00y\", 0x1F600)",
             "RegCIntrinsic.stringIndexOfChar(\"x\\ud83d\\ude00y\", 0x1F600)"},
            {"stringIndexOfCharFrom(\"ababa\", 'a', 2)",
             "RegCIntrinsic.stringIndexOfCharFrom(\"ababa\", 'a', 2)"},
            {"stringIndexOfCharFrom(\"ababa\", 'a', -10)",
             "RegCIntrinsic.stringIndexOfCharFrom(\"ababa\", 'a', -10)"},
            {"stringIndexOfString(\"A\\u4e2dB\\u4e2d\", \"\\u4e2d\")",
             "RegCIntrinsic.stringIndexOfString(\"A\\u4e2dB\\u4e2d\", \"\\u4e2d\")"},
            {"stringIndexOfStringFrom(\"ababa\", \"ba\", 2)",
             "RegCIntrinsic.stringIndexOfStringFrom(\"ababa\", \"ba\", 2)"},
            {"stringIndexOfStringFrom(\"abc\", \"\", 99)",
             "RegCIntrinsic.stringIndexOfStringFrom(\"abc\", \"\", 99)"},
            {"stringReplace(\"a\\u4e2da\", 'a', 'b')",
             "RegCIntrinsic.stringReplace(\"a\\u4e2da\", 'a', 'b')"},
            {"stringTrim(\" \\t\\u4e2d \\n\")", "RegCIntrinsic.stringTrim(\" \\t\\u4e2d \\n\")"},
            {"stringSplitLiteral(\"a--b--\", \"--\")",
             "RegCIntrinsic.stringSplitLiteral(\"a--b--\", \"--\")"},
            {"stringSplitLimit(\"a--b--\", \"--\", -1)",
             "RegCIntrinsic.stringSplitLimit(\"a--b--\", \"--\", -1)"},
            {"stringSplitLiteral(\"a,,b:::c\", \"[,:]+\")",
             "RegCIntrinsic.stringSplitLiteral(\"a,,b:::c\", \"[,:]+\")"},
            {"stringSplitInvalidRegex(\"abc\")", "RegCIntrinsic.stringSplitInvalidRegex(\"abc\")"},
            {"stringSubstringBounds(\"abc\")", "RegCIntrinsic.stringSubstringBounds(\"abc\")"},
            {"stringValueOfInt(-42)", "RegCIntrinsic.stringValueOfInt(-42)"},
            {"stringValueOfLong(1234567890123L)",
             "RegCIntrinsic.stringValueOfLong(1234567890123L)"},
            {"stringValueOfBoolean(true)", "RegCIntrinsic.stringValueOfBoolean(true)"},
            {"stringValueOfBoolean(false)", "RegCIntrinsic.stringValueOfBoolean(false)"},
            {"stringValueOfChar('A')", "RegCIntrinsic.stringValueOfChar('A')"},
            {"stringValueOfFloat(1.25f)", "RegCIntrinsic.stringValueOfFloat(1.25f)"},
            {"stringValueOfFloat(-0.0f)", "RegCIntrinsic.stringValueOfFloat(-0.0f)"},
            {"stringValueOfDouble(1.25d)", "RegCIntrinsic.stringValueOfDouble(1.25d)"},
            {"stringValueOfDouble(Double.NaN)", "RegCIntrinsic.stringValueOfDouble(Double.NaN)"},
            {"stringValueOfObject(new RegCIntrinsic.Named())",
             "RegCIntrinsic.stringValueOfObject(new RegCIntrinsic.Named())"},
            {"stringValueOfObject(null)", "RegCIntrinsic.stringValueOfObject(null)"},
            {"stringBuilderChain(\"v\", 42)", "RegCIntrinsic.stringBuilderChain(\"v\", 42)"},
            {"stringBuilderFromString(\"seed\")",
             "RegCIntrinsic.stringBuilderFromString(\"seed\")"},
            {"stringBuilderObject(new RegCIntrinsic.Named())",
             "RegCIntrinsic.stringBuilderObject(new RegCIntrinsic.Named())"},
            {"stringBuilderObject(null)", "RegCIntrinsic.stringBuilderObject(null)"},
            {"stringBuilderCharSequence(new StringBuilder(\"seq\"))",
             "RegCIntrinsic.stringBuilderCharSequence(new StringBuilder(\"seq\"))"},
            {"stringBuilderCharSequence(null)", "RegCIntrinsic.stringBuilderCharSequence(null)"},
            {"stringBuilderChars(new char[]{'A','\\u4e2d','!'})",
             "RegCIntrinsic.stringBuilderChars(new char[]{'A','\\u4e2d','!'})"},
            {"stringBuilderChars(new char[]{'\\ud83d','\\ude00'})",
             "RegCIntrinsic.stringBuilderChars(new char[]{'\\ud83d','\\ude00'})"},
            {"stringBuilderFloat(1.25f)", "RegCIntrinsic.stringBuilderFloat(1.25f)"},
            {"stringBuilderFloat(-0.0f)", "RegCIntrinsic.stringBuilderFloat(-0.0f)"},
            {"stringBuilderDouble(1.25d)", "RegCIntrinsic.stringBuilderDouble(1.25d)"},
            {"stringBuilderDouble(Double.NaN)", "RegCIntrinsic.stringBuilderDouble(Double.NaN)"},
            {"stringBuilderRange(new char[]{'x','Y','\\u4e2d','z'})",
             "RegCIntrinsic.stringBuilderRange(new char[]{'x','Y','\\u4e2d','z'})"},
            {"stringBuilderCapacity()", "RegCIntrinsic.stringBuilderCapacity()"},
            {"stringBuilderFromSequence()", "RegCIntrinsic.stringBuilderFromSequence()"},
            {"stringBuilderExceptionCleanup()", "RegCIntrinsic.stringBuilderExceptionCleanup()"},
            {"stringBuilderReturn(\"escaped\").toString()",
             "RegCIntrinsic.stringBuilderReturn(\"escaped\").toString()"},
            {"stringBuilderCapacityReturn().capacity()",
             "RegCIntrinsic.stringBuilderCapacityReturn().capacity()"},
            {"stringBuilderUnknownCall(\"unknown\")",
             "RegCIntrinsic.stringBuilderUnknownCall(\"unknown\")"},
            {"stringBuilderFieldEscape(\"field\")",
             "RegCIntrinsic.stringBuilderFieldEscape(\"field\")"},
            {"stringBuilderArrayEscape(\"array\")",
             "RegCIntrinsic.stringBuilderArrayEscape(\"array\")"},
            {"stringBufferChain(\"buf\", new char[]{'X','\\u4e2d','!'})",
             "RegCIntrinsic.stringBufferChain(\"buf\", new char[]{'X','\\u4e2d','!'})"},
            {"stringBufferCapacity()", "RegCIntrinsic.stringBufferCapacity()"},
            {"stringBufferReturn(\"escaped-buffer\").toString()",
             "RegCIntrinsic.stringBufferReturn(\"escaped-buffer\").toString()"},
            {"stringBufferCapacityReturn().capacity()",
             "RegCIntrinsic.stringBufferCapacityReturn().capacity()"},
            {"stringBufferUnknownCall(\"unknown-buffer\")",
             "RegCIntrinsic.stringBufferUnknownCall(\"unknown-buffer\")"},
            {"stringBufferUseAfterEscape(\"fallback-buffer\")",
             "RegCIntrinsic.stringBufferUseAfterEscape(\"fallback-buffer\")"},
        });

    // After runCase succeeds, the generated C file is on disk.  Re-open it to
    // assert the intrinsic path was taken (no JNI dispatch at the matched call
    // sites).  This is the structural guarantee the task requires: only the
    // boundary wrappers carry JNI, intrinsic call sites stay native.
    fs::path dir   = caseDir("RegCIntrinsic");
    fs::path cFile = dir / "RegCIntrinsic.c";
    ASSERT_TRUE(fileExists(cFile)) << cFile;
    std::string generatedC = readText(cFile);
    EXPECT_NE(generatedC.find("mj2x_math_sin("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_sqrt("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_max_i32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_abs_i32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_round("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_int("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_int_radix("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_long("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_long_radix("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_short("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_short_radix("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_byte("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_byte_radix("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_boolean("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_float("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_double("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_int_to_string("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_long_to_string("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_bool_to_string("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_char_to_string("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_float_to_string("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_double_to_string("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_object_to_string("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_int_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_long_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_byte_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_short_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_bool_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_char_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_float_compare("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_double_compare("), std::string::npos) << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_int_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_long_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_byte_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_short_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_bool_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_char_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_float_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static int32_t _mj2x_intrinsic_double_compare"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static mj2x_ref _mj2x_intrinsic_int_to_string"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static mj2x_ref _mj2x_intrinsic_long_to_string"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static mj2x_ref _mj2x_intrinsic_bool_to_string"), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("static mj2x_ref _mj2x_intrinsic_char_to_string"), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_literal("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_int_value_of("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_int_int_value("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_long_value_of("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_long_long_value("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_random_init(&native_r"), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_random_next_int(&native_r"), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_random_next_int_bound(&native_r"), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_length("), std::string::npos) << generatedC;
    // charAt/hashCode/equals 回退缓存 methodID 调用（1 JNI），不再发射
    // StringMethod intrinsic 调用点（多 JNI 数据管道实测更慢）。helper 定义
    // 仍在 prelude 无条件发射，无法用 find 区分定义与调用——用下面三个缓存
    // 声明的存在性证明调用点已回退。
    EXPECT_NE(generatedC.find("java/lang/String\", \"charAt\""), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("java/lang/String\", \"hashCode\""), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("java/lang/String\", \"equals\""), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_contains("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_concat("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_substring_from("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_substring_range("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_compare_to("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_index_of_char("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_index_of_string_from("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_replace_char("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_trim("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_split("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_split_limit("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_jstring("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_object("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_i32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_i64("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_bool("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_char("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_char_array("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_char_sequence_range("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_char_array_range("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_code_point("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_capacity("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_ensure_capacity("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_set_length("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_f32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_append_f64("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_to_jstring("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_destroy("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_builder_materialize("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_buffer_append_char_sequence_range("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_buffer_append_char_array_range("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_buffer_to_jstring("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_buffer_materialize("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_string_buffer_destroy("), std::string::npos) << generatedC;
    EXPECT_EQ(generatedC.find("mj2x_regc_local_string_builder"), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find(
                  "{\"java/lang/Object\", \"toString\", \"()Ljava/lang/String;\", NULL, NULL, "
                  "NULL, NULL}"),
              std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find(
                  "{\"java/lang/String\", \"valueOf\", \"(F)Ljava/lang/String;\", NULL, NULL}"),
              std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find(
                  "{\"java/lang/String\", \"valueOf\", \"(D)Ljava/lang/String;\", NULL, NULL}"),
              std::string::npos)
        << generatedC;
    // The matched JDK call sites must not dispatch through the JNI call helper.
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i64(\"java/lang/Math\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"parseInt\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i64(\"java/lang/Long\", \"parseLong\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Short\", \"parseShort\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Byte\", \"parseByte\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Boolean\", \"parseBoolean\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_f32(\"java/lang/Float\", \"parseFloat\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_f64(\"java/lang/Double\", \"parseDouble\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Integer\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Long\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Byte\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Short\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Boolean\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Character\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Float\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Double\", \"toString\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Long\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Byte\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Short\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Boolean\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Character\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Float\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Double\", \"compare\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/String\", \"valueOf\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Integer\", \"valueOf\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"intValue\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/Long\", \"valueOf\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i64(\"java/lang/Long\", \"longValue\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_alloc_object(\"java/util/Random\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/util/Random\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/String\", \"length\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/String\", \"charAt\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/String\", \"hashCode\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/String\", \"equals\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/String\", \"contains\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/String\", \"split\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_alloc_object(\"java/lang/StringBuilder\""),
              std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_ref(\"java/lang/StringBuilder\""), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("uint16_t mutable_string_value_"), std::string::npos) << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_void(\"java/lang/StringBuilder\""),
              std::string::npos)
        << generatedC;
}

// ---------------------------------------------------------------------------
// safe-env E2E：复刻 c2j-native-deobfuscator 的原地改写攻击姿势
// （*env->functions = hooked_copy，指针不变），验证：
//   1. 运行时 install（attach 场景）→ 入口恢复后 JNI 调用不再经过 hook；
//   2. 启动期污染（JNI_OnLoad 前已 hook）→ 硬模式库加载失败；
//   3. 启动期污染 + debug 模式 → 告警后继续加载。
// ---------------------------------------------------------------------------

namespace {

// 受保护样例：Integer.valueOf + hashCode 循环 → 逐次 GetObjectClass /
// CallIntMethodA JNI 调用（被 hook 计数）。
const char* kSafeEnvSampleJava =
    "public class SafeEnvSample {\n"
    "  public static int sumHash(int n) {\n"
    "    int acc = 0;\n"
    "    for (int i = 0; i < n; i++) {\n"
    "      Integer v = Integer.valueOf(i);\n"
    "      acc += v.hashCode();\n"
    "    }\n"
    "    return acc;\n"
    "  }\n"
    "}\n";

const char* kSafeEnvHookC = R"hook(
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

static struct JNINativeInterface_ g_saved;
static struct JNINativeInterface_* g_live;
static long volatile g_getobjclass_calls = 0;
static long volatile g_callint_calls = 0;

static jclass JNICALL h_GetObjectClass(JNIEnv* env, jobject obj)
{
    InterlockedIncrement(&g_getobjclass_calls);
    return g_saved.GetObjectClass(env, obj);
}

static jint JNICALL h_CallIntMethodA(JNIEnv* env, jobject obj, jmethodID m, const jvalue* a)
{
    InterlockedIncrement(&g_callint_calls);
    return g_saved.CallIntMethodA(env, obj, m, a);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    g_live = (struct JNINativeInterface_*)(((const struct JNIEnv_*)(const void*)env)->functions);
    g_saved = *g_live;
    if (getenv("HOOK_AT_LOAD")) {
        g_live->GetObjectClass = h_GetObjectClass;
        g_live->CallIntMethodA = h_CallIntMethodA;
    }
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_Hook_install(JNIEnv* env, jclass clazz)
{
    (void)env; (void)clazz;
    g_live->GetObjectClass = h_GetObjectClass;
    g_live->CallIntMethodA = h_CallIntMethodA;
}

JNIEXPORT jlong JNICALL Java_Hook_count(JNIEnv* env, jclass clazz)
{
    (void)env; (void)clazz;
    return (jlong)(g_getobjclass_calls + g_callint_calls);
}
)hook";

const char* kSafeEnvDriverJava =
    "public class SafeEnvDriver {\n"
    "  public static void main(String[] a) throws Exception {\n"
    "    String hook = System.getenv(\"MORPHER_HOOK_DLL\");\n"
    "    String prot = System.getenv(\"MORPHER_DLL\");\n"
    "    if (hook != null) System.load(hook);\n"
    "    if (prot != null) System.load(prot);\n"
    "    long w1 = SafeEnvSample.sumHash(50);\n"
    "    Hook.install();\n"
    "    long c1 = Hook.count();\n"
    "    long r1 = SafeEnvSample.sumHash(200);\n"
    "    long c2 = Hook.count();\n"
    "    long r2 = SafeEnvSample.sumHash(200);\n"
    "    long c3 = Hook.count();\n"
    "    System.out.println(\"w1=\" + w1);\n"
    "    System.out.println(\"r1=\" + r1);\n"
    "    System.out.println(\"r2=\" + r2);\n"
    "    System.out.println(\"delta1=\" + (c2 - c1));\n"
    "    System.out.println(\"delta2=\" + (c3 - c2));\n"
    "  }\n"
    "}\n";

const char* kSafeEnvLoadDriverJava =
    "public class SafeEnvLoadDriver {\n"
    "  public static void main(String[] a) {\n"
    "    System.load(System.getenv(\"MORPHER_HOOK_DLL\"));\n"
    "    try {\n"
    "      System.load(System.getenv(\"MORPHER_DLL\"));\n"
    "      System.out.println(\"load=OK\");\n"
    "    } catch (UnsatisfiedLinkError e) {\n"
    "      System.out.println(\"load=FAIL:\" + e.getMessage());\n"
    "    }\n"
    "  }\n"
    "}\n";

const char* kSafeEnvHookJava =
    "public class Hook {\n"
    "  static { System.load(System.getenv(\"MORPHER_HOOK_DLL\")); }\n"
    "  public static native void install();\n"
    "  public static native long count();\n"
    "}\n";

// ---- libca_process 子进程工具（本节的 javac/cl/link/java 全部直接启动，
//      不写临时 .bat 脚本；MSVC 环境通过 vcvarsall.bat 的 `set` 输出捕获一次）----

using SafeEnvProcessEnv = std::vector<std::pair<std::string, std::string>>;

struct SafeEnvExecResult
{
    int         exitCode = -1;
    std::string out;
};

// 捕获 vcvarsall.bat x64 环境（cmd /c "call vcvarsall.bat && set"），
// 后续 cl/link 以完整环境直接启动。命令串放环境变量，规避 Windows 命令行的
// 引号转义（同 mj2x-cli native_compiler.cpp loadMsvcEnvironment）。
SafeEnvProcessEnv loadVcvarsEnv(const Toolchain& tc, std::string& error)
{
    SafeEnvProcessEnv env;
    ca::process::Command shell("cmd.exe");
    shell.current_dir(tc.vcvars64.parent_path().string());
    shell.env("MJ2X_VCVARS_COMMAND", "chcp 65001 >nul && call vcvarsall.bat x64 >nul && set");
    shell.arg("/d").arg("/c").arg("%MJ2X_VCVARS_COMMAND%");
    auto captured = shell.output();
    if (captured.is_err()) {
        error = captured.unwrap_err().to_string();
        return env;
    }
    auto output = std::move(captured).unwrap();
    if (!output.status.success()) {
        error = output.stdout_data + output.stderr_data;
        return env;
    }
    std::istringstream lines(output.stdout_data);
    std::string        line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0) continue;
        env.emplace_back(line.substr(0, separator), line.substr(separator + 1));
    }
    return env;
}

// 直接启动程序（不经 shell），可追加环境变量覆盖，返回退出码 + stdout/stderr 合并。
// 注意：程序名必须是全路径 —— CreateProcessW 的裸名搜索只查父进程（测试运行器）
// 的 PATH，不查 lpEnvironment 里传入的 PATH；测试运行器在 Git Bash 下启动，
// 父 PATH 未必含 MSVC，故工具名一律先经 findToolInPath 解析为全路径。
SafeEnvExecResult runProcess(const fs::path& program, const std::vector<std::string>& args,
                             const SafeEnvProcessEnv& env, const fs::path& cwd = {})
{
    ca::process::Command command(program.string());
    command.args(args);
    for (const auto& [key, value] : env) command.env(key, value);
    if (!cwd.empty()) command.current_dir(cwd.string());
    command.stdout(ca::process::Stdio::piped());
    command.stderr(ca::process::Stdio::piped());
    auto result = command.output();
    if (result.is_err()) {
        return {1, "spawn failed: " + result.unwrap_err().to_string()};
    }
    auto output = std::move(result).unwrap();
    return {output.status.code, output.stdout_data + output.stderr_data};
}

// 在 vcvars 环境里按 PATH 顺序找 cl.exe/link.exe（CreateProcessW 裸名搜索
// 不查子进程环境块，见 runProcess 注释；与 CLI loadMsvcEnvironment 的
// 全路径解析同模式）。
fs::path findToolInPath(const SafeEnvProcessEnv& env, const char* exeName)
{
    for (const auto& [key, value] : env) {
        if (key != "PATH" && key != "Path") continue;
        std::istringstream pathStream(value);
        std::string        entry;
        while (std::getline(pathStream, entry, ';')) {
            if (entry.empty()) continue;
            const auto candidate = fs::path(entry) / exeName;
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate;
        }
    }
    return {};
}

// 编译 hook DLL（复刻 c2j 工具的原地改写 install）。
void buildHookDll(const Toolchain& tc, const fs::path& dir)
{
    fs::path hookC = dir / "hook_dll.c";
    {
        std::ofstream out(hookC);
        out << kSafeEnvHookC;
    }
    std::string vcvarsError;
    auto        env = loadVcvarsEnv(tc, vcvarsError);
    ASSERT_TRUE(!env.empty()) << vcvarsError;
    const auto clPath   = findToolInPath(env, "cl.exe");
    const auto linkPath = findToolInPath(env, "link.exe");
    ASSERT_FALSE(clPath.empty()) << "cl.exe not found in vcvars PATH";
    ASSERT_FALSE(linkPath.empty()) << "link.exe not found in vcvars PATH";

    std::vector<std::string> includes = {"/I" + tc.jdkInclude.string(),
                                         "/I" + tc.jdkIncludeWin.string()};
    std::vector<std::string> clArgs = {"/nologo", "/O2", "/TC", "/utf-8", "/c",
                                       "/Fo" + (dir / "hook_dll.obj").string(),
                                       hookC.string()};
    clArgs.insert(clArgs.end(), includes.begin(), includes.end());
    auto clResult = runProcess(clPath, clArgs, env, dir);
    ASSERT_EQ(clResult.exitCode, 0) << clResult.out;
    auto linkResult = runProcess(linkPath,
                                 {"/nologo", "/DLL", "/OUT:" + (dir / "hook_dll.dll").string(),
                                  (dir / "hook_dll.obj").string()},
                                 env, dir);
    ASSERT_EQ(linkResult.exitCode, 0) << linkResult.out;
    ASSERT_TRUE(fileExists(dir / "hook_dll.dll"));
}

// 生成并编译受保护 DLL；safeEnv 控制 RegCBackend::setSafeEnv / setSafeEnvDebug。
void buildProtectedDll(const Toolchain& tc, const fs::path& dir, bool safeEnv,
                       bool safeEnvDebug)
{
    fs::path javaFile = dir / "SafeEnvSample.java";
    {
        std::ofstream out(javaFile);
        out << kSafeEnvSampleJava;
    }
    auto javac = runProcess(tc.javac, {"-d", dir.string(), javaFile.string()}, {}, dir);
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    fs::path inClass  = dir / "SafeEnvSample.class";
    fs::path outClass = dir / "SafeEnvSample_protected.class";
    fs::path cFile    = dir / "SafeEnvSample.c";

    mj2x::PipelineConfig config;
    config.inputPath  = inClass.string();
    config.outputPath = outClass.string();
    mj2x::Pipeline pipeline(std::move(config));
    auto           backend = std::make_unique<mj2x::RegCBackend>();
    backend->setOutputPath(cFile.string());
    if (safeEnv) backend->setSafeEnv(true);
    if (safeEnvDebug) backend->setSafeEnvDebug(true);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::InitClinitMoverPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
    ASSERT_EQ(pipeline.run(), 0);

    std::string generatedC = readText(cFile);
    if (safeEnv) {
        EXPECT_NE(generatedC.find("mj2x_regc_safeenv_restore(env,"), std::string::npos)
            << generatedC;
        EXPECT_NE(generatedC.find("mj2x_regc_safeenv_capture(env,"), std::string::npos)
            << generatedC;
    }
    else {
        EXPECT_EQ(generatedC.find("mj2x_regc_safeenv_"), std::string::npos) << generatedC;
    }

    std::error_code ec;
    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::path dll    = dir / "protected.dll";
    fs::path objDir = dir / "obj";
    fs::create_directories(objDir, ec);
    fs::path crtSrc = fs::current_path() / "mj2x" / "mj2x-crt" / "src";

    std::string vcvarsError;
    auto        env = loadVcvarsEnv(tc, vcvarsError);
    ASSERT_TRUE(!env.empty()) << vcvarsError;
    const auto clPath   = findToolInPath(env, "cl.exe");
    const auto linkPath = findToolInPath(env, "link.exe");
    ASSERT_FALSE(clPath.empty()) << "cl.exe not found in vcvars PATH";
    ASSERT_FALSE(linkPath.empty()) << "link.exe not found in vcvars PATH";
    std::vector<std::string> includes = {"/I" + crtSrc.string(), "/I" + tc.jdkInclude.string(),
                                         "/I" + tc.jdkIncludeWin.string()};

    std::vector<std::string> genArgs = {"/nologo", "/O2", "/utf-8", "/c",
                                        "/Fo" + (objDir / "generated.obj").string(),
                                        cFile.string()};
    genArgs.insert(genArgs.end(), includes.begin(), includes.end());
    auto generated = runProcess(clPath, genArgs, env, dir);
    ASSERT_EQ(generated.exitCode, 0) << generated.out;

    std::vector<fs::path> objs;
    auto                  compileCrt = [&](const std::string& relPath, const std::string& objName) {
        fs::path src = crtSrc / relPath;
        ASSERT_TRUE(fileExists(src)) << src;
        fs::path obj = objDir / objName;
        objs.push_back(obj);
        std::vector<std::string> crtArgs = {"/nologo", "/O2", "/TC", "/utf-8", "/c",
                                            "/Fo" + obj.string(), src.string()};
        crtArgs.insert(crtArgs.end(), includes.begin(), includes.end());
        auto rc = runProcess(clPath, crtArgs, env, dir);
        ASSERT_EQ(rc.exitCode, 0) << rc.out;
    };
    compileCrt("regc/mj2x_regc_crt.c", "mj2x_regc_crt.obj");
    compileCrt("regc/mj2x_regc_safe_env.c", "mj2x_regc_safe_env.obj");
    compileCrt("regc/mj2x_regc_string.c", "mj2x_regc_string.obj");
    compileCrt("regc/mj2x_regc_string_builder.c", "mj2x_regc_string_builder.obj");

    std::vector<std::string> linkArgs = {"/nologo", "/DLL", "/OUT:" + dll.string(),
                                         (objDir / "generated.obj").string()};
    for (const auto& obj : objs) linkArgs.push_back(obj.string());
    auto linkResult = runProcess(linkPath, linkArgs, env, dir);
    ASSERT_EQ(linkResult.exitCode, 0) << linkResult.out;
    ASSERT_TRUE(fileExists(dll));
}

}   // namespace

class RegCSafeEnvE2E : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "RegC safe-env E2E toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/regc_e2e/" + name + "/.keep").parent_path();
    }
};

Toolchain RegCSafeEnvE2E::tc_;

// attach 场景：运行时原地改写（c2j 姿势），safe-env 入口恢复后不再被持续 hook；
// 对照组（无 safe-env）持续被 hook（delta > 0）。
TEST_F(RegCSafeEnvE2E, EntryRestoreDefeatsInPlaceHook)
{
    const std::string name = "SafeEnvRestore";
    fs::path          dir  = caseDir(name);
    std::error_code   ec;
    fs::create_directories(dir, ec);

    {
        std::ofstream out(dir / "Hook.java");
        out << kSafeEnvHookJava;
    }
    {
        std::ofstream out(dir / "SafeEnvDriver.java");
        out << kSafeEnvDriverJava;
    }

    buildHookDll(tc_, dir);
    buildProtectedDll(tc_, dir, /*safeEnv=*/true, /*safeEnvDebug=*/false);

    // SafeEnvSample.class 此时已存在（buildProtectedDll 内 javac 产出）。
    auto javac = runProcess(tc_.javac,
                            {"-cp", dir.string(), "-d", dir.string(),
                             (dir / "Hook.java").string(), (dir / "SafeEnvDriver.java").string()},
                            {}, dir);
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    auto safeRun = runProcess(tc_.java,
                              {"-cp", dir.string(), "SafeEnvDriver"},
                              {{"MORPHER_HOOK_DLL", (dir / "hook_dll.dll").string()},
                               {"MORPHER_DLL", (dir / "protected.dll").string()}},
                              dir);
    ASSERT_EQ(safeRun.exitCode, 0) << safeRun.out;
    auto safe = splitLines(safeRun.out);
    EXPECT_NE(std::find(safe.begin(), safe.end(), "w1=1225"), safe.end()) << safeRun.out;
    EXPECT_NE(std::find(safe.begin(), safe.end(), "r1=19900"), safe.end()) << safeRun.out;
    EXPECT_NE(std::find(safe.begin(), safe.end(), "r2=19900"), safe.end()) << safeRun.out;
    EXPECT_NE(std::find(safe.begin(), safe.end(), "delta1=0"), safe.end()) << safeRun.out;
    EXPECT_NE(std::find(safe.begin(), safe.end(), "delta2=0"), safe.end()) << safeRun.out;

    // 对照组：同一样例不开启 safe-env，install 后持续被 hook。
    buildProtectedDll(tc_, dir, /*safeEnv=*/false, /*safeEnvDebug=*/false);
    fs::rename(dir / "protected.dll", dir / "protected_off.dll", ec);
    ASSERT_FALSE(ec) << ec.message();

    auto offRun = runProcess(tc_.java,
                             {"-cp", dir.string(), "SafeEnvDriver"},
                             {{"MORPHER_HOOK_DLL", (dir / "hook_dll.dll").string()},
                              {"MORPHER_DLL", (dir / "protected_off.dll").string()}},
                             dir);
    ASSERT_EQ(offRun.exitCode, 0) << offRun.out;
    auto off = splitLines(offRun.out);
    bool sawHookedDelta = false;
    for (const auto& line : off) {
        if (line.rfind("delta1=", 0) == 0 && line != "delta1=0") sawHookedDelta = true;
    }
    EXPECT_TRUE(sawHookedDelta) << offRun.out;
}

// 启动期污染（HOOK_AT_LOAD）：硬模式 JNI_OnLoad fail closed → 库加载失败。
TEST_F(RegCSafeEnvE2E, HardModeFailsOnStartupPollution)
{
    const std::string name = "SafeEnvHardFail";
    fs::path          dir  = caseDir(name);
    std::error_code   ec;
    fs::create_directories(dir, ec);

    {
        std::ofstream out(dir / "SafeEnvLoadDriver.java");
        out << kSafeEnvLoadDriverJava;
    }
    auto javac = runProcess(tc_.javac,
                            {"-d", dir.string(), (dir / "SafeEnvLoadDriver.java").string()}, {},
                            dir);
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    buildHookDll(tc_, dir);
    buildProtectedDll(tc_, dir, /*safeEnv=*/true, /*safeEnvDebug=*/false);

    auto run = runProcess(tc_.java,
                          {"-cp", dir.string(), "SafeEnvLoadDriver"},
                          {{"MORPHER_HOOK_DLL", (dir / "hook_dll.dll").string()},
                           {"HOOK_AT_LOAD", "1"},
                           {"MORPHER_DLL", (dir / "protected.dll").string()}},
                          dir);
    ASSERT_EQ(run.exitCode, 0) << run.out;
    auto lines = splitLines(run.out);
    EXPECT_NE(std::find_if(lines.begin(), lines.end(), [](const std::string& l) {
                  return l.rfind("load=FAIL", 0) == 0;
              }),
              lines.end())
        << run.out;
}

// 启动期污染 + debug 模式：告警后继续加载，不 fail。
TEST_F(RegCSafeEnvE2E, DebugModeContinuesOnStartupPollution)
{
    const std::string name = "SafeEnvDebug";
    fs::path          dir  = caseDir(name);
    std::error_code   ec;
    fs::create_directories(dir, ec);

    {
        std::ofstream out(dir / "SafeEnvLoadDriver.java");
        out << kSafeEnvLoadDriverJava;
    }
    auto javac = runProcess(tc_.javac,
                            {"-d", dir.string(), (dir / "SafeEnvLoadDriver.java").string()}, {},
                            dir);
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    buildHookDll(tc_, dir);
    buildProtectedDll(tc_, dir, /*safeEnv=*/true, /*safeEnvDebug=*/true);

    auto run = runProcess(tc_.java,
                          {"-cp", dir.string(), "SafeEnvLoadDriver"},
                          {{"MORPHER_HOOK_DLL", (dir / "hook_dll.dll").string()},
                           {"HOOK_AT_LOAD", "1"},
                           {"MORPHER_DLL", (dir / "protected.dll").string()}},
                          dir);
    ASSERT_EQ(run.exitCode, 0) << run.out;
    auto lines = splitLines(run.out);
    EXPECT_NE(std::find(lines.begin(), lines.end(), "load=OK"), lines.end()) << run.out;
    EXPECT_NE(std::find_if(lines.begin(), lines.end(), [](const std::string& l) {
                  return l.find("safe-env] WARNING") != std::string::npos;
              }),
              lines.end())
        << run.out;
}

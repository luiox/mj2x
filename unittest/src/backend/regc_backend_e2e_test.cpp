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
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
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
        compileLines.push_back("cl /nologo /O2 /c /Fo" + q(generatedObj) + " " + q(cFile) + " /I" +
                               q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
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
            compileLines.push_back("cl /nologo /O2 /TC /c /Fo" + q(obj) + " " + q(src) + " /I" +
                                   q(crtSrc) + " /I" + q(tc_.jdkInclude) + " /I" +
                                   q(tc_.jdkIncludeWin));
        };
        compileCrt("regc/mj2x_regc_crt.c", "mj2x_regc_crt.obj");
        compileCrt("regc/mj2x_regc_string.c", "mj2x_regc_string.obj");
        compileCrt("regc/mj2x_regc_string_builder.c", "mj2x_regc_string_builder.obj");
        compileRuntime("intrinsic/intrinsic_wrapper.cpp", "intrinsic_wrapper.obj");
        compileRuntime("intrinsic/intrinsic_math.cpp", "intrinsic_math.obj");
        compileRuntime("intrinsic/intrinsic_random.cpp", "intrinsic_random.obj");

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
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_char_at("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_hash_code("), std::string::npos)
        << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_equals("), std::string::npos) << generatedC;
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
                  "{\"java/lang/Object\", \"toString\", \"()Ljava/lang/String;\", NULL, NULL}"),
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

// RegC native E2E harness: Java source -> javac -> RegCBackend -> .c -> DLL
// -> Java loads the DLL and calls the protected native methods.
//
// This is intentionally narrower than CBackendE2E.  It grows with the RegC
// runtime-helper surface and avoids call-helper cases until that ABI is wired.

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

struct ExecResult {
    int exitCode = -1;
    std::string out;
};

struct Toolchain {
    bool available = false;
    std::string reason;
    fs::path javac;
    fs::path java;
    fs::path jdkInclude;
    fs::path jdkIncludeWin;
    fs::path vcvars64;
};

struct JavaCheck {
    std::string label;
    std::string expression;
};

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string q(const fs::path& path) {
    std::string s = path.string();
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
    return "\"" + s + "\"";
}

ExecResult runBat(const std::vector<std::string>& lines) {
    ExecResult result;
    fs::path tmpDir = test::getTempOutPath("mj2x_regc_e2e_run.keep").parent_path();
    fs::path script = tmpDir / ("mj2x_regc_e2e_" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".bat");
    {
        std::ofstream out(script);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        for (const auto& line : lines) out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }

    std::string cmd = q(script) + " 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "rb");
    if (!pipe) {
        result.out = "failed to run " + script.string();
        return result;
    }
    char buffer[4096];
    while (size_t n = fread(buffer, 1, sizeof(buffer), pipe)) {
        result.out.append(buffer, n);
    }
    result.exitCode = _pclose(pipe);

    std::error_code ec;
    fs::remove(script, ec);
    return result;
}

fs::path findJdk() {
    if (const char* home = std::getenv("JAVA_HOME"); home && *home) {
        fs::path root(home);
        if (fileExists(root / "bin" / "javac.exe")) return root;
    }
    fs::path fallback = "C:/Users/Canrad/.jdks/corretto-17.0.5";
    if (fileExists(fallback / "bin" / "javac.exe")) return fallback;
    return {};
}

fs::path findVcvars() {
    fs::path vswhere = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
    if (!fileExists(vswhere)) return {};

    auto probe = runBat({q(vswhere) + " -latest -property installationPath"});
    if (probe.exitCode != 0) return {};
    std::string path = probe.out;
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' ')) {
        path.pop_back();
    }
    if (path.empty()) return {};

    fs::path vcvars = fs::path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    return fileExists(vcvars) ? vcvars : fs::path{};
}

Toolchain probeToolchain() {
    Toolchain tc;
    fs::path jdk = findJdk();
    if (jdk.empty()) {
        tc.reason = "javac not found; set JAVA_HOME";
        return tc;
    }
    tc.javac = jdk / "bin" / "javac.exe";
    tc.java = jdk / "bin" / "java.exe";
    tc.jdkInclude = jdk / "include";
    tc.jdkIncludeWin = tc.jdkInclude / "win32";
    if (!fileExists(tc.jdkInclude / "jni.h")) {
        tc.reason = "jni.h not found under " + tc.jdkInclude.string();
        return tc;
    }
    tc.vcvars64 = findVcvars();
    if (tc.vcvars64.empty()) {
        tc.reason = "vcvars64.bat not found; MSVC is required";
        return tc;
    }
    tc.available = true;
    return tc;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += "  ";
        out += line;
        out += "\n";
    }
    return out;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

class RegCBackendE2E : public ::testing::Test {
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() {
        tc_ = probeToolchain();
    }

    void SetUp() override {
        if (!tc_.available) {
            GTEST_SKIP() << "RegC native E2E toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const {
        return test::getOutPath("mj2x/regc_e2e/" + name + "/.keep").parent_path();
    }

    void runCase(const std::string& className,
                 const std::string& javaSource,
                 const std::vector<JavaCheck>& checks) {
        fs::path dir = caseDir(className);
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
        auto driverCompile = runBat({q(tc_.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
        ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

        auto baselineRun = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
        ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
        auto baseline = splitLines(baselineRun.out);

        fs::path inClass = dir / (className + ".class");
        fs::path outClass = dir / (className + "_protected.class");
        fs::path cFile = dir / (className + ".c");

        mj2x::PipelineConfig config;
        config.inputPath = inClass.string();
        config.outputPath = outClass.string();
        mj2x::Pipeline pipeline(std::move(config));
        auto backend = std::make_unique<mj2x::RegCBackend>();
        backend->setOutputPath(cFile.string());
        pipeline.registerBackend(std::move(backend));
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
        ASSERT_EQ(pipeline.run(), 0);
        ASSERT_TRUE(fileExists(cFile));
        std::string generatedC = readText(cFile);
        EXPECT_EQ(generatedC.find("static int32_t _mj2x_regc_div_i32"), std::string::npos)
            << generatedC;
        EXPECT_EQ(generatedC.find("static void _mj2x_regc_barrier"), std::string::npos)
            << generatedC;

        fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        fs::path dll = dir / (className + ".dll");
        fs::path objDir = dir / "obj";
        fs::create_directories(objDir, ec);
        ASSERT_FALSE(ec) << ec.message();
        fs::path generatedObj = objDir / "generated.obj";
        fs::path runtimeSrc = fs::current_path() / "mj2x" / "mj2x-runtime" / "src";

        // Compile every runtime .cpp the generated TU may reference.  The regc
        // ABI helper is always needed (div/rem/barrier); the intrinsic modules
        // back the mj2x_math_* / mj2x_intrinsic_* symbols the emitter lowers
        // JDK calls to.  Compiling all of them keeps the harness ready for new
        // intrinsic coverage without per-test obj wiring.
        std::vector<std::string> compileLines;
        compileLines.push_back("call " + q(tc_.vcvars64) + " >nul");
        compileLines.push_back(
            "cl /nologo /O2 /c /Fo" + q(generatedObj) + " " + q(cFile) +
            " /I" + q(runtimeSrc) +
            " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin));

        std::vector<fs::path> runtimeObjs;
        auto compileRuntime = [&](const std::string& relPath, const std::string& objName) {
            fs::path src = runtimeSrc / relPath;
            ASSERT_TRUE(fileExists(src)) << src;
            fs::path obj = objDir / objName;
            runtimeObjs.push_back(obj);
            compileLines.push_back(
                "cl /nologo /O2 /c /Fo" + q(obj) + " " + q(src) +
                " /I" + q(runtimeSrc) +
                " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin));
        };
        compileRuntime("regc/regc_runtime_abi.cpp", "regc_runtime_abi.obj");
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

        ASSERT_EQ(baseline.size(), actual.size())
            << "baseline:\n" << joinLines(baseline)
            << "protected:\n" << joinLines(actual);
        for (size_t i = 0; i < baseline.size(); ++i) {
            EXPECT_EQ(baseline[i], actual[i]);
        }
    }

private:
    static std::string javaString(const std::string& text) {
        std::ostringstream out;
        out << "\"";
        for (char c : text) {
            switch (c) {
            case '\\': out << "\\\\"; break;
            case '"':  out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:   out << c; break;
            }
        }
        out << "\"";
        return out.str();
    }

    static std::string buildDriver(const std::vector<JavaCheck>& checks) {
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

} // namespace

TEST_F(RegCBackendE2E, PureArithmeticLoopAndArrayRoundTrip) {
    runCase(
        "RegCNativePure",
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

TEST_F(RegCBackendE2E, FieldAndReferenceHelpersRoundTrip) {
    runCase(
        "RegCNativeHelpers",
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

TEST_F(RegCBackendE2E, MultiArrayHelpersRoundTrip) {
    runCase(
        "RegCMultiArrayHelpers",
        "public class RegCMultiArrayHelpers {\n"
        "  public static int cube(int a, int b, int c) {\n"
        "    int[][][] values = new int[a][b][c];\n"
        "    values[a - 1][b - 1][c - 1] = a + b + c;\n"
        "    return values.length + values[0].length + values[0][0].length + values[a - 1][b - 1][c - 1];\n"
        "  }\n"
        "}\n",
        {
            {"cube(2, 3, 4)", "RegCMultiArrayHelpers.cube(2, 3, 4)"},
        });
}

TEST_F(RegCBackendE2E, CallHelpersRoundTrip) {
    runCase(
        "RegCCallHelpers",
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

TEST_F(RegCBackendE2E, NarrowPrimitiveRoundTrip) {
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
            {"narrowFields", "new RegCNarrowHelpers().narrowFields((byte)-12, (short)18, (char)70, true)"},
            {"staticNarrowFields", "RegCNarrowHelpers.staticNarrowFields((byte)-10, (short)20, (char)71, false)"},
            {"narrowArrays", "RegCNarrowHelpers.narrowArrays((byte)-8, (short)22, (char)72, true)"},
            {"castNarrow", "RegCNarrowHelpers.castNarrow(7)"},
        });
}

TEST_F(RegCBackendE2E, StackJoinTernaryRoundTrip) {
    runCase(
        "RegCStackJoinTernary",
        "public class RegCStackJoinTernary {\n"
        "  public static int booleanTernary(boolean flag) { return flag ? 11 : 3; }\n"
        "  public static int intTernary(int value) { return (value & 1) != 0 ? value + 7 : value - 5; }\n"
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
            {"referenceTernary(false)", "RegCStackJoinTernary.referenceTernary(false, \"L\", \"R\")"},
        });
}

TEST_F(RegCBackendE2E, NullTrapHandlerRoundTrip) {
    runCase(
        "RegCNullTrapHandlers",
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

TEST_F(RegCBackendE2E, ThrowHelperRoundTrip) {
    runCase(
        "RegCThrowHelpers",
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

TEST_F(RegCBackendE2E, TryCatchDispatchRoundTrip) {
    runCase(
        "RegCExceptionDispatch",
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

TEST_F(RegCBackendE2E, IntegerDivisionTrapRoundTrip) {
    runCase(
        "RegCIntegerTraps",
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

// JDK intrinsic E2E: Integer.parseInt + Math.* run entirely in native code.
// The generated C must call mj2x_math_* / _mj2x_intrinsic_parse_int and must
// not dispatch Math/parseInt through the JNI call helper.  Results match the
// original Java baseline (parseInt "42" -> 42, Math.sin via the same IEEE 754
// sin both sides use).
TEST_F(RegCBackendE2E, IntrinsicMathAndParseIntRoundTrip) {
    runCase(
        "RegCIntrinsic",
        "public class RegCIntrinsic {\n"
        "  public static int parseInt42() { return Integer.parseInt(\"42\"); }\n"
        "  public static int parseIntArg(String s) { return Integer.parseInt(s); }\n"
        "  public static double sinDemo(double x) { return Math.sin(x); }\n"
        "  public static double sqrtDemo(double x) { return Math.sqrt(x); }\n"
        "  public static int mathChain(int a, int b) {\n"
        "    return Math.max(a, b) + Math.abs(a - b);\n"
        "  }\n"
        "  public static long roundDemo(double x) { return Math.round(x); }\n"
        "}\n",
        {
            {"parseInt42()", "RegCIntrinsic.parseInt42()"},
            {"parseIntArg(\"-123\")", "RegCIntrinsic.parseIntArg(\"-123\")"},
            {"sinDemo(1.0)", "RegCIntrinsic.sinDemo(1.0)"},
            {"sqrtDemo(2.0)", "RegCIntrinsic.sqrtDemo(2.0)"},
            {"mathChain(3, 9)", "RegCIntrinsic.mathChain(3, 9)"},
            {"mathChain(-5, -2)", "RegCIntrinsic.mathChain(-5, -2)"},
            {"roundDemo(2.6)", "RegCIntrinsic.roundDemo(2.6)"},
            {"roundDemo(2.5)", "RegCIntrinsic.roundDemo(2.5)"},
            {"roundDemo(-2.5)", "RegCIntrinsic.roundDemo(-2.5)"},
        });

    // After runCase succeeds, the generated C file is on disk.  Re-open it to
    // assert the intrinsic path was taken (no JNI dispatch at the matched call
    // sites).  This is the structural guarantee the task requires: only the
    // boundary wrappers carry JNI, intrinsic call sites stay native.
    fs::path dir = caseDir("RegCIntrinsic");
    fs::path cFile = dir / "RegCIntrinsic.c";
    ASSERT_TRUE(fileExists(cFile)) << cFile;
    std::string generatedC = readText(cFile);
    EXPECT_NE(generatedC.find("mj2x_math_sin("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_sqrt("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_max_i32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_abs_i32("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("mj2x_math_round("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_parse_int("), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("_mj2x_intrinsic_string_literal("), std::string::npos) << generatedC;
    // The matched JDK call sites must not dispatch through the JNI call helper.
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_f64(\"java/lang/Math\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i64(\"java/lang/Math\""), std::string::npos)
        << generatedC;
    EXPECT_EQ(generatedC.find("_mj2x_regc_call_i32(\"java/lang/Integer\", \"parseInt\""),
              std::string::npos)
        << generatedC;
}

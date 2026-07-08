// StackCBackend 真 E2E harness：Java 源 → javac → mj2x → .c → cl → java 调用对比。
//
// 与 stackc_backend_test.cpp 的子串匹配不同，这里把生成的 C 真编译成 DLL、在 JVM 里调用，
// 比对结果与原 Java 行为是否一致。这是 revival 文档定义的"端到端闭环"判据。
//
// 工具链探测放在 fixture SetUp：javac/java/cl/mj2x-cli 任一缺失即 GTEST_SKIP，
// 不污染 CI（无 JDK/C 编译器的环境会跳过，本机有则真跑）。
//
// 生成物写到 <project_root>/test/mj2x/e2e/<case_name>/（遵循 spec：测试输出走 test/）。

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

// ---- 工具：执行子进程，捕获 stdout+stderr 合并 ----
struct ExecResult {
    int exitCode = -1;
    std::string out;     // 合并的 stdout+stderr
};

// 通过临时 .bat 文件执行命令（绕开 cmd /C 引号解析的坑——含空格的绝对路径
// 在 cmd /C "..." 下解析规则特殊，极易出错；写 bat 文件则按字面执行）。
// bat 内容 = lines；返回合并输出。
ExecResult runBat(const std::vector<std::string>& lines) {
    ExecResult r;
    // 临时 bat 放到 test/tmp，避免 cwd 影响。
    fs::path tmpDir = test::getTempOutPath("mj2x_e2e_runbat.keep").parent_path();
    fs::path bat = tmpDir / ("mj2x_e2e_" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
        "_" + std::to_string(std::rand()) + ".bat");
    {
        std::ofstream f(bat);
        f << "@echo off\n";
        f << "chcp 65001 >nul 2>&1\n";   // UTF-8 输出，避免中文系统 GBK 乱码
        for (auto& l : lines) f << l << "\n";
        f << "exit /b %errorlevel%\n";
    }
    std::string full = "\"" + bat.string() + "\" 2>&1";
    FILE* pipe =
#ifdef _WIN32
        _popen(full.c_str(), "rb");
#else
        popen(full.c_str(), "r");
#endif
    if (!pipe) {
        r.out = "[runBat] _popen failed for: " + full;
        return r;
    }
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) r.out.append(buf, n);
#ifdef _WIN32
    r.exitCode = _pclose(pipe);
#else
    r.exitCode = pclose(pipe);
#endif
    std::error_code ec;
    fs::remove(bat, ec);
    return r;
}

// 兼容旧调用：单行命令直接跑（少用，路径含空格时不可靠）。
ExecResult runProc(const std::string& cmd) {
    return runBat({cmd});
}

// ---- 工具链探测 ----
struct Toolchain {
    bool available = false;
    std::string reason;          // 不可用原因（用于 GTEST_SKIP 说明）
    fs::path javac;
    fs::path java;
    fs::path mj2xCli;            // mj2x-cli.exe
    fs::path jdkInclude;         // <jdk>/include（含 jni.h）
    fs::path jdkIncludeWin;      // <jdk>/include/win32（含 jni_md.h）
    fs::path vcvars64;           // VS vcvars64.bat（设置 cl 的 INCLUDE/LIB 环境）
};

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

fs::path findJdk() {
    // 优先 JAVA_HOME；其次回落到 owner 提供的路径。
    if (const char* home = std::getenv("JAVA_HOME"); home && *home) {
        fs::path p = fs::path(home);
        if (fileExists(p / "bin" / "javac.exe") || fileExists(p / "bin" / "javac"))
            return p;
    }
    fs::path fallback = "C:/Users/Canrad/.jdks/corretto-17.0.5";
    if (fileExists(fallback / "bin" / "javac.exe"))
        return fallback;
    return {};
}

fs::path findVcvars() {
    // vswhere 定位 VS 安装根，再拼 vcvars64.bat。
    fs::path vswhere = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
    if (!fileExists(vswhere)) return {};
    auto r = runBat({"\"" + vswhere.string() + "\" -latest -property installationPath"});
    if (r.exitCode != 0) return {};
    // 去尾部换行/空白
    std::string path = r.out;
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' '))
        path.pop_back();
    if (path.empty()) return {};
    fs::path vcvars = fs::path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    return fileExists(vcvars) ? vcvars : fs::path{};
}

Toolchain probeToolchain() {
    Toolchain tc;
    tc.javac = findJdk();
    if (tc.javac.empty()) {
        tc.reason = "JAVA_HOME 未设置或 javac 不存在（探测 corretto-17.0.5 也失败）";
        return tc;
    }
    tc.java = tc.javac / "bin" / "java.exe";
    tc.javac = tc.javac / "bin" / "javac.exe";
    tc.jdkInclude = tc.javac.parent_path().parent_path() / "include";
    tc.jdkIncludeWin = tc.jdkInclude / "win32";
    if (!fileExists(tc.jdkInclude / "jni.h")) {
        tc.reason = "jni.h 不在 JDK include 下: " + tc.jdkInclude.string();
        return tc;
    }

    tc.mj2xCli = test::getTopProjectPath() / "build" / "windows" / "x64" / "release" / "mj2x-cli.exe";
    if (!fileExists(tc.mj2xCli)) {
        tc.reason = "mj2x-cli.exe 未构建（请先 xmake build mj2x-cli）。期望路径: " + tc.mj2xCli.string();
        return tc;
    }

    tc.vcvars64 = findVcvars();
    if (tc.vcvars64.empty()) {
        tc.reason = "未找到 VS vcvars64.bat（需要 MSVC 编译生成的 .c）";
        return tc;
    }

    tc.available = true;
    return tc;
}

// 把路径转成 cmd 友好的带引号字符串（Windows 路径含空格常见）。
// 用反斜杠（cmd 原生分隔符），避免 cmd 把 "C:/..." 的 / 当开关解析报"卷标语法不正确"。
std::string q(const fs::path& p) {
    std::string s = p.string();
    for (auto& c : s) if (c == '/') c = '\\';
    return "\"" + s + "\"";
}

// ---- E2E 框架 ----
class StackCBackendE2E : public ::testing::Test {
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() {
        tc_ = probeToolchain();
    }

    // 每个 case 独立目录：test/mj2x/e2e/<case>/
    fs::path caseDir(const std::string& name) const {
        return test::getOutPath("mj2x/e2e/" + name + "/.keep").parent_path();
    }

    void SetUp() override {
        if (!tc_.available) {
            GTEST_SKIP() << "E2E 工具链不可用，跳过: " << tc_.reason;
        }
    }

    // 一个 E2E 用例：编译 javaSrc（含若干 static 方法）→ mj2x → cl → 在 JVM 调用并对比。
    //   className: Java 类名（如 "Sample"），文件名同。
    //   javaSrc:   完整 Java 源（public class <className> { ... }）。
    //   checks:    {"<methodName>(<args>)", 期望输出行} —— driver 调用并打印 "<methodName>=<value>"。
    //              args 是 Java 字面量参数串（如 "3, 4"），value 是 driver 打印的返回值字符串。
    struct Check {
        std::string call;     // e.g. "add(2147483647, 1)"
        std::string expect;   // e.g. "-2147483648"（int wrap）
    };

    void runCase(const std::string& className,
                 const std::string& javaSrc,
                 const std::vector<Check>& checks) {
        fs::path dir = caseDir(className);
        fs::create_directories(dir);

        // 1. 写 .java
        fs::path javaFile = dir / (className + ".java");
        {
            std::ofstream f(javaFile);
            f << javaSrc;
        }

        // 2. javac 编译（先得到"原 Java 行为"的基准 .class）
        {
            auto r = runBat({q(tc_.javac) + " -d " + q(dir) + " " + q(javaFile)});
            ASSERT_EQ(r.exitCode, 0) << "javac 失败:\n" << r.out;
        }

        // 3. 跑原 Java 取基准值（驱动类 Driver，单独写避免被 mj2x 改写）。
        //    Driver 直接静态调用方法，打印 "<call>=<value>"。
        std::string driverSrc = buildDriver(className, checks);
        fs::path driverFile = dir / "Driver.java";
        {
            std::ofstream f(driverFile);
            f << driverSrc;
        }
        std::vector<std::string> baseline;
        {
            // Driver 引用 <className>，编译时需把已编译的 .class 加入 classpath。
            auto r = runBat({q(tc_.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " + q(driverFile)});
            ASSERT_EQ(r.exitCode, 0) << "Driver javac 失败:\n" << r.out;
            auto run = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
            ASSERT_EQ(run.exitCode, 0) << "基准 java 运行失败:\n" << run.out;
            baseline = splitLines(run.out);
        }

        // 4. mj2x：把 <className>.class 改写为 native + 生成 .c。
        fs::path inClass = dir / (className + ".class");
        fs::path outClass = dir / (className + "_protected.class");
        fs::path cFile = dir / (className + ".c");
        // 留存基准 .class 作对照（改写后类名仍是 <className>，用 out 覆盖 in）。
        fs::path baselineClass = dir / (className + "_baseline.class");
        fs::copy_file(inClass, baselineClass, fs::copy_options::overwrite_existing);
        {
            auto r = runBat({q(tc_.mj2xCli) + " -i " + q(inClass) + " -o " + q(outClass)
                             + " --emit-c " + q(cFile)});
            ASSERT_EQ(r.exitCode, 0) << "mj2x-cli 失败:\n" << r.out;
            ASSERT_TRUE(fileExists(cFile)) << "mj2x 未生成 .c";
        }
        // 用改写后的类替换原类（Driver 按 <className> 加载）。
        fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing);

        // 5. cl 编译 .c → <className>.dll（<clinit> 桥触发 RegisterNatives）。
        fs::path dll = dir / (className + ".dll");
        {
            // 通过 vcvars64.bat 设置 cl 需要的 INCLUDE/LIB 环境。
            // cl /LD 生成 DLL；/Fe 指定输出名；/Fo 指定 .obj 输出目录（避免污染 cwd）；
            // /I 加 jni.h 头路径。
            auto r = runBat({
                "call " + q(tc_.vcvars64) + " >nul",
                "cl /nologo /LD /O2 /EHsc /Fe" + q(dll) + " /Fo" + q(dir / "_") + " "
                + q(cFile) + " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin),
            });
            ASSERT_EQ(r.exitCode, 0) << "cl 编译失败:\n" << r.out;
            ASSERT_TRUE(fileExists(dll)) << "cl 未生成 .dll";
        }

        // 6. java 加载 DLL 调用 native 方法，对比基准。
        //    Driver 用环境变量 MORPHER_DLL 拿 DLL 路径并 System.load。
        std::vector<std::string> actual;
        {
            auto run = runBat({
                "set \"MORPHER_DLL=" + dll.string() + "\"",
                q(tc_.java) + " -cp " + q(dir) + " Driver",
            });
            ASSERT_EQ(run.exitCode, 0) << "protected java 运行失败:\n" << run.out;
            actual = splitLines(run.out);
        }

        // 7. 对比（基准 == 保护后）。
        ASSERT_EQ(baseline.size(), actual.size())
            << "输出行数不一致。\n基准:\n" << join(baseline)
            << "\n保护后:\n" << join(actual);
        for (size_t i = 0; i < baseline.size(); ++i) {
            EXPECT_EQ(baseline[i], actual[i])
                << "第 " << i << " 行不一致（应与原 Java 行为一致）";
        }
    }

private:
    // Driver：直接静态调用 <className> 的方法，打印 "<call>=<value>"。
    // 用反射会很麻烦（基本类型签名匹配），这里生成强类型直接调用——更可靠、更可读。
    // Driver 自身不被 mj2x 处理（它不在被保护的类列表里）。
    static std::string buildDriver(const std::string& className,
                                   const std::vector<Check>& checks) {
        std::ostringstream s;
        s << "public class Driver {\n";
        s << "  public static void main(String[] a) throws Exception {\n";
        s << "    String dll = System.getenv(\"MORPHER_DLL\");\n";
        s << "    if (dll != null) System.load(dll);\n";
        for (auto& ck : checks) {
            // call 形如 add(3, 4)。直接发 "<call>=<value>"。
            s << "    try {\n";
            s << "      System.out.println(" << javaStringLiteral(ck.call + "=") << " + "
              << className << "." << ck.call << ");\n";
            s << "    } catch (Throwable t) {\n";
            s << "      System.out.println(" << javaStringLiteral(ck.call + "=")
              << " + \"THROW:\" + t.getClass().getName());\n";
            s << "    }\n";
        }
        s << "  }\n";
        s << "}\n";
        return s.str();
    }

    static std::string javaStringLiteral(const std::string& value) {
        std::ostringstream s;
        s << "\"";
        for (char c : value) {
            switch (c) {
            case '\\': s << "\\\\"; break;
            case '"':  s << "\\\""; break;
            case '\n': s << "\\n"; break;
            case '\r': s << "\\r"; break;
            case '\t': s << "\\t"; break;
            default:   s << c; break;
            }
        }
        s << "\"";
        return s.str();
    }

    static std::vector<std::string> splitLines(const std::string& s) {
        std::vector<std::string> v;
        std::string cur;
        for (char c : s) {
            if (c == '\n') { v.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) v.push_back(cur);
        return v;
    }
    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (auto& l : v) { s += "  "; s += l; s += "\n"; }
        return s;
    }
};

Toolchain StackCBackendE2E::tc_;

} // namespace

// ===== Wave 1 E2E 用例：整数算术与局部变量 =====

TEST_F(StackCBackendE2E, IntAdd) {
    runCase("IntAdd",
        "public class IntAdd {\n"
        "  public static int add(int a, int b) { return a + b; }\n"
        "}\n",
        {
            {"add(3, 4)", "7"},
            {"add(2147483647, 1)", "-2147483648"},   // int wrap（JVM 二补码环绕）
            {"add(-1, 1)", "0"},
            {"add(-2147483648, -1)", "2147483647"}, // 负方向 wrap
        });
}

TEST_F(StackCBackendE2E, IntMulSubNeg) {
    runCase("IntArith",
        "public class IntArith {\n"
        "  public static int mul(int a, int b) { return a * b; }\n"
        "  public static int sub(int a, int b) { return a - b; }\n"
        "  public static int neg(int a) { return -a; }\n"
        "}\n",
        {
            {"mul(6, 7)", "42"},
            {"mul(65536, 65536)", "0"},            // wrap：2^32 溢出
            {"sub(10, 3)", "7"},
            {"sub(0, -2147483648)", "-2147483648"},// wrap
            {"neg(5)", "-5"},
            {"neg(-2147483648)", "-2147483648"},   // INT_MIN 取负 wrap 回自身
        });
}

TEST_F(StackCBackendE2E, IntIinc) {
    // iinc + 局部变量自增
    runCase("IntIinc",
        "public class IntIinc {\n"
        "  public static int inc5(int a) { a += 5; return a; }\n"
        "  public static int loopSum(int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 1; i <= n; i++) s += i;\n"
        "    return s;\n"
        "  }\n"
        "}\n",
        {
            {"inc5(10)", "15"},
            {"loopSum(10)", "55"},   // 1+2+...+10（提前验 Wave 3 控制流 if/goto）
        });
}

TEST_F(StackCBackendE2E, IntBitwiseShift) {
    runCase("IntBits",
        "public class IntBits {\n"
        "  public static int and(int a, int b) { return a & b; }\n"
        "  public static int or(int a, int b) { return a | b; }\n"
        "  public static int xor(int a, int b) { return a ^ b; }\n"
        "  public static int shl(int a, int b) { return a << b; }\n"
        "  public static int shr(int a, int b) { return a >> b; }\n"
        "  public static int ushr(int a, int b) { return a >>> b; }\n"
        "}\n",
        {
            {"and(12, 10)", "8"},
            {"or(12, 10)", "14"},
            {"xor(12, 10)", "6"},
            {"shl(1, 31)", "-2147483648"},         // wrap
            {"shr(-1, 1)", "-1"},                  // 算术右移
            {"ushr(-1, 1)", "2147483647"},         // 无符号右移（验 USHR 正确性）
        });
}

TEST_F(StackCBackendE2E, IntControlFlow) {
    // if/else + 比较分支（提前验 Wave 3 的 if/goto 基础）
    runCase("IntCmp",
        "public class IntCmp {\n"
        "  public static int max(int a, int b) { return a > b ? a : b; }\n"
        "  public static int sign(int a) {\n"
        "    if (a > 0) return 1;\n"
        "    else if (a < 0) return -1;\n"
        "    else return 0;\n"
        "  }\n"
        "}\n",
        {
            {"max(3, 5)", "5"},
            {"max(5, 3)", "5"},
            {"sign(7)", "1"},
            {"sign(-7)", "-1"},
            {"sign(0)", "0"},
        });
}

// ===== Wave 2 E2E 用例：long/float/double 算术与类型转换 =====

TEST_F(StackCBackendE2E, LongArith) {
    runCase("LongArith",
        "public class LongArith {\n"
        "  public static long add(long a, long b) { return a + b; }\n"
        "  public static long mul(long a, long b) { return a * b; }\n"
        "  public static long shl(long a, int b) { return a << b; }\n"
        "  public static long ushr(long a, int b) { return a >>> b; }\n"
        "  public static long div(long a, long b) { return a / b; }\n"
        "  public static long rem(long a, long b) { return a % b; }\n"
        "}\n",
        {
            {"add(9223372036854775807L, 1L)", "-9223372036854775808"},
            {"mul(4294967296L, 4294967296L)", "0"},
            {"shl(1L, 63)", "-9223372036854775808"},
            {"ushr(-1L, 1)", "9223372036854775807"},
            {"div(-9223372036854775808L, -1L)", "-9223372036854775808"},
            {"rem(-9223372036854775808L, -1L)", "0"},
        });
}

TEST_F(StackCBackendE2E, IntegerDivisionTrap) {
    runCase("IntegerDivisionTrap",
        "public class IntegerDivisionTrap {\n"
        "  public static int idiv(int a, int b) { return a / b; }\n"
        "  public static int irem(int a, int b) { return a % b; }\n"
        "  public static long ldiv(long a, long b) { return a / b; }\n"
        "  public static long lrem(long a, long b) { return a % b; }\n"
        "  public static int catchIntDiv(int a, int b) {\n"
        "    try { return a / b; } catch (ArithmeticException e) { return 77; }\n"
        "  }\n"
        "  public static int catchLongRem(long a, long b) {\n"
        "    try { long v = a % b; return (int)v; } catch (ArithmeticException e) { return 88; }\n"
        "  }\n"
        "}\n",
        {
            {"idiv(10, 2)", "5"},
            {"irem(10, 3)", "1"},
            {"ldiv(81L, 9L)", "9"},
            {"lrem(81L, 7L)", "4"},
            {"idiv(-2147483648, -1)", "-2147483648"},
            {"irem(-2147483648, -1)", "0"},
            {"ldiv(-9223372036854775808L, -1L)", "-9223372036854775808"},
            {"lrem(-9223372036854775808L, -1L)", "0"},
            {"catchIntDiv(1, 0)", "77"},
            {"catchLongRem(1L, 0L)", "88"},
            {"idiv(1, 0)", "THROW:java.lang.ArithmeticException"},
        });
}

TEST_F(StackCBackendE2E, FloatDouble) {
    runCase("FloatDouble",
        "public class FloatDouble {\n"
        "  public static float fadd(float a, float b) { return a + b; }\n"
        "  public static float frem(float a, float b) { return a % b; }\n"
        "  public static double ddiv(double a, double b) { return a / b; }\n"
        "  public static int dcmp(double a, double b) { return a < b ? -1 : (a == b ? 0 : 1); }\n"
        "  public static int nanNe() { double x = 0.0d; x = x / x; return x != x ? 1 : 0; }\n"
        "  public static int nanGt() { double x = 0.0d; x = x / x; return x > 0.0d ? 1 : 0; }\n"
        "}\n",
        {
            {"fadd(1.5f, 2.5f)", "4.0"},
            {"frem(5.5f, 2.0f)", "1.5"},
            {"ddiv(1.0d, 4.0d)", "0.25"},
            {"dcmp(1.0d, 2.0d)", "-1"},
            {"dcmp(2.0d, 1.0d)", "1"},
            {"nanNe()", "1"},
            {"nanGt()", "0"},
        });
}

TEST_F(StackCBackendE2E, Conversions) {
    runCase("Conversions",
        "public class Conversions {\n"
        "  public static long i2l(int a) { return (long)a; }\n"
        "  public static int l2i(long a) { return (int)a; }\n"
        "  public static int d2i(double a) { return (int)a; }\n"
        "  public static long f2l(float a) { return (long)a; }\n"
        "  public static int i2b(int a) { return (byte)a; }\n"
        "  public static int i2c(int a) { return (char)a; }\n"
        "  public static int i2s(int a) { return (short)a; }\n"
        "  public static int nanToInt() { double x = 0.0d; x = x / x; return (int)x; }\n"
        "}\n",
        {
            {"i2l(2147483647)", "2147483647"},
            {"l2i(68719476735L)", "-1"},
            {"d2i(2.7d)", "2"},
            {"f2l(2.7f)", "2"},
            {"i2b(128)", "-128"},
            {"i2c(-1)", "65535"},
            {"i2s(65535)", "-1"},
            {"nanToInt()", "0"},
        });
}

// ===== Wave 3 E2E 用例：控制流完整性 =====

TEST_F(StackCBackendE2E, DenseSwitch) {
    runCase("DenseSwitch",
        "public class DenseSwitch {\n"
        "  public static int classify(int x) {\n"
        "    switch (x) {\n"
        "      case -1: return 7;\n"
        "      case 0: return 11;\n"
        "      case 1: return 13;\n"
        "      case 2: return 17;\n"
        "      default: return 19;\n"
        "    }\n"
        "  }\n"
        "}\n",
        {
            {"classify(-1)", "7"},
            {"classify(0)", "11"},
            {"classify(2)", "17"},
            {"classify(3)", "19"},
        });
}

TEST_F(StackCBackendE2E, SparseSwitch) {
    runCase("SparseSwitch",
        "public class SparseSwitch {\n"
        "  public static int pick(int x) {\n"
        "    switch (x) {\n"
        "      case -100: return 1;\n"
        "      case 7: return 2;\n"
        "      case 2048: return 3;\n"
        "      default: return 4;\n"
        "    }\n"
        "  }\n"
        "}\n",
        {
            {"pick(-100)", "1"},
            {"pick(7)", "2"},
            {"pick(2048)", "3"},
            {"pick(8)", "4"},
        });
}

TEST_F(StackCBackendE2E, NestedLoopsAndContinue) {
    runCase("NestedLoops",
        "public class NestedLoops {\n"
        "  public static int nested(int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < n; i++) {\n"
        "      for (int j = 0; j <= i; j++) {\n"
        "        if (((i + j) & 1) == 0) continue;\n"
        "        s += i * 10 + j;\n"
        "      }\n"
        "    }\n"
        "    return s;\n"
        "  }\n"
        "  public static int whileCount(int n) {\n"
        "    int r = 0;\n"
        "    while (n > 0) {\n"
        "      r = r * 3 + n;\n"
        "      n--;\n"
        "    }\n"
        "    return r;\n"
        "  }\n"
        "}\n",
        {
            {"nested(0)", "0"},
            {"nested(5)", "177"},
            {"whileCount(4)", "142"},
        });
}

TEST_F(StackCBackendE2E, ReferenceBranches) {
    runCase("ReferenceBranches",
        "public class ReferenceBranches {\n"
        "  public static int isNullConst() { Object o = null; return o == null ? 1 : 0; }\n"
        "  public static int isNonNullConst() { Object o = null; return o != null ? 1 : 0; }\n"
        "  public static int sameNull() { Object a = null; Object b = a; return a == b ? 1 : 0; }\n"
        "  public static int diffNull() { Object a = null; Object b = null; return a != b ? 1 : 0; }\n"
        "}\n",
        {
            {"isNullConst()", "1"},
            {"isNonNullConst()", "0"},
            {"sameNull()", "1"},
            {"diffNull()", "0"},
        });
}

// ===== Wave 4 E2E 用例：方法调用、构造、接口与 String LDC =====

TEST_F(StackCBackendE2E, MethodCalls) {
    runCase("MethodCalls",
        "public class MethodCalls {\n"
        "  public MethodCalls() {}\n"
        "  public int inc(int x) { return x + 3; }\n"
        "  public static int twice(int x) { return x * 2; }\n"
        "  public static long mix(long a, double b) { return a + (long)b; }\n"
        "  public static int callStatic(int x) { return twice(x) + 1; }\n"
        "  public static int callInstance(int x) { return new MethodCalls().inc(x); }\n"
        "  public static long callWide(long a, double b) { return mix(a, b); }\n"
        "}\n",
        {
            {"callStatic(20)", "41"},
            {"callInstance(9)", "12"},
            {"callWide(40L, 2.75d)", "42"},
        });
}

TEST_F(StackCBackendE2E, RecursionCalls) {
    runCase("RecursionCalls",
        "public class RecursionCalls {\n"
        "  public static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }\n"
        "  public static int fib(int n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }\n"
        "}\n",
        {
            {"fact(1)", "1"},
            {"fact(6)", "720"},
            {"fib(7)", "13"},
        });
}

TEST_F(StackCBackendE2E, StringLiterals) {
    runCase("StringLiterals",
        "public class StringLiterals {\n"
        "  public static String literal() { return \"hello jni\"; }\n"
        "  public static int literalLength() { return \"hello jni\".length(); }\n"
        "  public static String escaped() { return \"quote=\\\" slash=\\\\\"; }\n"
        "}\n",
        {
            {"literal()", "hello jni"},
            {"literalLength()", "9"},
            {"escaped()", "quote=\" slash=\\"},
        });
}

TEST_F(StackCBackendE2E, InterfaceCalls) {
    runCase("InterfaceCalls",
        "public class InterfaceCalls {\n"
        "  public interface Op { int apply(int x); }\n"
        "  public static class AddOne implements Op {\n"
        "    public AddOne() {}\n"
        "    public int apply(int x) { return x + 1; }\n"
        "  }\n"
        "  public static int viaInterface(int x) { Op op = new AddOne(); return op.apply(x); }\n"
        "}\n",
        {
            {"viaInterface(9)", "10"},
            {"viaInterface(-1)", "0"},
        });
}

TEST_F(StackCBackendE2E, InterfaceDefaultMethodProxy) {
    fs::path dir = caseDir("DefaultIface");
    fs::create_directories(dir);

    fs::path javaFile = dir / "DefaultIface.java";
    {
        std::ofstream f(javaFile);
        f << "public interface DefaultIface {\n"
          << "  default int inc(int x) { return x + 1; }\n"
          << "}\n"
          << "class DefaultIfaceImpl implements DefaultIface {}\n";
    }
    fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream f(driverFile);
        f << "public class Driver {\n"
          << "  public static void main(String[] args) throws Exception {\n"
          << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
          << "    if (dll != null) System.load(dll);\n"
          << "    DefaultIface op = new DefaultIfaceImpl();\n"
          << "    System.out.println(\"inc=\" + op.inc(41));\n"
          << "  }\n"
          << "}\n";
    }

    auto javac = runBat({q(tc_.javac) + " -d " + q(dir) + " " + q(javaFile) + " " + q(driverFile)});
    ASSERT_EQ(javac.exitCode, 0) << "javac 失败:\n" << javac.out;
    auto baseline = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baseline.exitCode, 0) << "基准 java 运行失败:\n" << baseline.out;
    ASSERT_NE(baseline.out.find("inc=42"), std::string::npos) << baseline.out;

    fs::path inClass = dir / "DefaultIface.class";
    fs::path outClass = dir / "DefaultIface_protected.class";
    fs::path cFile = dir / "DefaultIface.c";
    auto mj2x = runBat({q(tc_.mj2xCli) + " -i " + q(inClass) + " -o " + q(outClass)
                        + " --emit-c " + q(cFile)});
    ASSERT_EQ(mj2x.exitCode, 0) << "mj2x-cli 失败:\n" << mj2x.out;
    ASSERT_TRUE(fileExists(dir / "mj2x" / "cloader" / "Loader.class"));
    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing);

    fs::path dll = dir / "DefaultIface.dll";
    auto cl = runBat({
        "call " + q(tc_.vcvars64) + " >nul",
        "cl /nologo /LD /O2 /EHsc /Fe" + q(dll) + " /Fo" + q(dir / "_") + " "
            + q(cFile) + " /I" + q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin),
    });
    ASSERT_EQ(cl.exitCode, 0) << "cl 编译失败:\n" << cl.out;
    ASSERT_TRUE(fileExists(dll));

    auto actual = runBat({
        "set \"MORPHER_DLL=" + dll.string() + "\"",
        q(tc_.java) + " -cp " + q(dir) + " Driver",
    });
    ASSERT_EQ(actual.exitCode, 0) << "protected java 运行失败:\n" << actual.out;
    EXPECT_NE(actual.out.find("inc=42"), std::string::npos) << actual.out;
}

TEST_F(StackCBackendE2E, InvokeDynamicStringConcat) {
    runCase("InvokeDynamicStringConcat",
        "public class InvokeDynamicStringConcat {\n"
        "  public static String join(int a, long b, String s) {\n"
        "    return \"a=\" + a + \",b=\" + b + \",s=\" + s;\n"
        "  }\n"
        "  public static String withNull(String s) { return \"n=\" + s; }\n"
        "}\n",
        {
            {"join(7, 42L, \"ok\")", "a=7,b=42,s=ok"},
            {"join(-1, 9223372036854775807L, \"x\")", "a=-1,b=9223372036854775807,s=x"},
            {"withNull(null)", "n=null"},
        });
}

TEST_F(StackCBackendE2E, InvokeDynamicLambda) {
    runCase("InvokeDynamicLambda",
        "import java.util.function.IntUnaryOperator;\n"
        "public class InvokeDynamicLambda {\n"
        "  private static int base(int x) { return x + 3; }\n"
        "  public static int stateless(int x) {\n"
        "    IntUnaryOperator op = InvokeDynamicLambda::base;\n"
        "    return op.applyAsInt(x);\n"
        "  }\n"
        "  public static int capturing(int x) {\n"
        "    int delta = 5;\n"
        "    IntUnaryOperator op = v -> v + delta;\n"
        "    return op.applyAsInt(x);\n"
        "  }\n"
        "}\n",
        {
            {"stateless(9)", "12"},
            {"capturing(10)", "15"},
        });
}

// ===== Final StackCBackend E2E 用例：对象/字段/数组 =====

TEST_F(StackCBackendE2E, ObjectFieldsAndTypeChecks) {
    runCase("ObjectFields",
        "public class ObjectFields {\n"
        "  int value;\n"
        "  static int total = 5;\n"
        "  public ObjectFields(int v) { value = v; }\n"
        "  public int bump(int d) { value += d; return value; }\n"
        "  public static int instanceFlow(int x) {\n"
        "    ObjectFields f = new ObjectFields(x);\n"
        "    return f.bump(3) + f.value;\n"
        "  }\n"
        "  public static int staticFlow(int x) { total += x; return total; }\n"
        "  public static int isString(Object o) { return o instanceof String ? 1 : 0; }\n"
        "  public static int castLen(Object o) { return ((String)o).length(); }\n"
        "  public static int castNull() { String s = (String)null; return s == null ? 1 : 0; }\n"
        "}\n",
        {
            {"instanceFlow(10)", "26"},
            {"staticFlow(4)", "9"},
            {"staticFlow(-2)", "7"},
            {"isString(\"x\")", "1"},
            {"isString(new Object())", "0"},
            {"castLen(\"abcd\")", "4"},
            {"castNull()", "1"},
        });
}

TEST_F(StackCBackendE2E, ExplicitNullPointerTraps) {
    runCase("NullTraps",
        "public class NullTraps {\n"
        "  int value;\n"
        "  int get() { return value; }\n"
        "  public static int fieldGet() { try { NullTraps n = null; return n.value; } catch (NullPointerException e) { return 11; } }\n"
        "  public static int fieldPut() { try { NullTraps n = null; n.value = 3; return 0; } catch (NullPointerException e) { return 12; } }\n"
        "  public static int arrayLength() { try { int[] a = null; return a.length; } catch (NullPointerException e) { return 13; } }\n"
        "  public static int arrayLoad() { try { int[] a = null; return a[0]; } catch (NullPointerException e) { return 14; } }\n"
        "  public static int arrayStore() { try { int[] a = null; a[0] = 1; return 0; } catch (NullPointerException e) { return 15; } }\n"
        "  public static int invoke() { try { NullTraps n = null; return n.get(); } catch (NullPointerException e) { return 16; } }\n"
        "}\n",
        {
            {"fieldGet()", "11"},
            {"fieldPut()", "12"},
            {"arrayLength()", "13"},
            {"arrayLoad()", "14"},
            {"arrayStore()", "15"},
            {"invoke()", "16"},
        });
}

TEST_F(StackCBackendE2E, PrimitiveArrays) {
    runCase("PrimitiveArrays",
        "public class PrimitiveArrays {\n"
        "  public static int intSum(int n) {\n"
        "    int[] a = new int[n];\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < a.length; i++) { a[i] = i * 3; s += a[i]; }\n"
        "    return s + a.length;\n"
        "  }\n"
        "  public static long longMix() { long[] a = new long[3]; a[0] = 40L; a[1] = 2L; return a[0] + a[1] + a.length; }\n"
        "  public static double doubleMix() { double[] a = new double[2]; a[0] = 1.5d; a[1] = 2.25d; return a[0] + a[1]; }\n"
        "  public static int narrowMix() { byte[] b = new byte[2]; short[] s = new short[2]; char[] c = new char[1]; b[0] = (byte)130; s[1] = (short)40000; c[0] = (char)65535; return b[0] + s[1] + c[0]; }\n"
        "  public static int booleanMix(boolean x) { boolean[] z = new boolean[3]; z[0] = x; z[1] = !x; z[2] = true; return (z[0] ? 1 : 0) + (z[1] ? 10 : 0) + (z[2] ? 100 : 0); }\n"
        "}\n",
        {
            {"intSum(5)", "35"},
            {"longMix()", "45"},
            {"doubleMix()", "3.75"},
            {"narrowMix()", "40063"},
            {"booleanMix(true)", "101"},
            {"booleanMix(false)", "110"},
        });
}

TEST_F(StackCBackendE2E, ObjectArrays) {
    runCase("ObjectArrays",
        "public class ObjectArrays {\n"
        "  public static int stringArray() {\n"
        "    String[] a = new String[3];\n"
        "    a[0] = \"aa\"; a[1] = \"bbbb\"; a[2] = \"c\";\n"
        "    return a.length + a[0].length() + a[1].length() + a[2].length();\n"
        "  }\n"
        "  public static int objectArrayNulls() {\n"
        "    Object[] a = new Object[2];\n"
        "    a[0] = null; a[1] = \"x\";\n"
        "    return (a[0] == null ? 10 : 0) + (a[1] instanceof String ? 1 : 0);\n"
        "  }\n"
        "}\n",
        {
            {"stringArray()", "10"},
            {"objectArrayNulls()", "11"},
        });
}

TEST_F(StackCBackendE2E, MultiDimArrays) {
    runCase("MultiDimArrays",
        "public class MultiDimArrays {\n"
        "  public static int matrix() {\n"
        "    int[][] m = new int[2][3];\n"
        "    m[1][2] = 7;\n"
        "    return m.length + m[1].length + m[1][2];\n"
        "  }\n"
        "  public static int cube() {\n"
        "    String[][][] s = new String[2][1][2];\n"
        "    s[1][0][1] = \"ok\";\n"
        "    return s.length + s[1].length + s[1][0].length + s[1][0][1].length();\n"
        "  }\n"
        "}\n",
        {
            {"matrix()", "12"},
            {"cube()", "7"},
        });
}

TEST_F(StackCBackendE2E, ThrowPropagation) {
    runCase("ThrowPropagation",
        "public class ThrowPropagation {\n"
        "  public static int raise(int x) {\n"
        "    if (x > 0) throw new IllegalStateException();\n"
        "    return 7;\n"
        "  }\n"
        "}\n",
        {
            {"raise(0)", "7"},
            {"raise(1)", "THROW:java.lang.IllegalStateException"},
        });
}

TEST_F(StackCBackendE2E, TryCatchExceptions) {
    runCase("TryCatchExceptions",
        "public class TryCatchExceptions {\n"
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
        "}\n",
        {
            {"catchOwn(0)", "1"},
            {"catchOwn(1)", "2"},
            {"missCatch()", "THROW:java.lang.IllegalStateException"},
        });
}

TEST_F(StackCBackendE2E, MultiCatchAndFinally) {
    runCase("MultiCatchAndFinally",
        "public class MultiCatchAndFinally {\n"
        "  public static int multi(int x) {\n"
        "    try {\n"
        "      if (x == 0) throw new IllegalArgumentException();\n"
        "      if (x == 1) throw new NullPointerException();\n"
        "      return 3;\n"
        "    } catch (IllegalArgumentException e) {\n"
        "      return 10;\n"
        "    } catch (RuntimeException e) {\n"
        "      return 20;\n"
        "    }\n"
        "  }\n"
        "  public static int finallyNormal(int x) {\n"
        "    int r = 0;\n"
        "    try { r = x + 1; } finally { r += 10; }\n"
        "    return r;\n"
        "  }\n"
        "  public static int finallyCatch() {\n"
        "    int r = 0;\n"
        "    try { throw new IllegalStateException(); }\n"
        "    catch (RuntimeException e) { r = 5; }\n"
        "    finally { r += 10; }\n"
        "    return r;\n"
        "  }\n"
        "}\n",
        {
            {"multi(0)", "10"},
            {"multi(1)", "20"},
            {"multi(2)", "3"},
            {"finallyNormal(7)", "18"},
            {"finallyCatch()", "15"},
        });
}

TEST_F(StackCBackendE2E, ExceptionFromNativeCallAndMonitor) {
    runCase("ExceptionFromNativeCallAndMonitor",
        "public class ExceptionFromNativeCallAndMonitor {\n"
        "  public static void fail() { throw new IllegalArgumentException(); }\n"
        "  public static int catchCall() {\n"
        "    try { fail(); return 1; }\n"
        "    catch (IllegalArgumentException e) { return 9; }\n"
        "  }\n"
        "  public static int syncBlock(int x) {\n"
        "    Object lock = new Object();\n"
        "    synchronized (lock) { return x + 1; }\n"
        "  }\n"
        "}\n",
        {
            {"catchCall()", "9"},
            {"syncBlock(41)", "42"},
        });
}

TEST_F(StackCBackendE2E, IntegratedSample) {
    runCase("IntegratedSample",
        "public class IntegratedSample {\n"
        "  int seed;\n"
        "  static int bias = 2;\n"
        "  public IntegratedSample(int s) { seed = s; }\n"
        "  public int apply(int x) {\n"
        "    int[] data = new int[4];\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < data.length; i++) {\n"
        "      data[i] = seed + x + i;\n"
        "      total += data[i];\n"
        "    }\n"
        "    switch (total & 3) {\n"
        "      case 0: return total + bias;\n"
        "      case 1: return total - bias;\n"
        "      default: return total ^ bias;\n"
        "    }\n"
        "  }\n"
        "  public static int recur(int n) { return n <= 1 ? 1 : n + recur(n - 2); }\n"
        "  public static int run(int a, int b) {\n"
        "    IntegratedSample s = new IntegratedSample(a);\n"
        "    String[][] names = new String[1][2];\n"
        "    names[0][0] = \"ab\";\n"
        "    names[0][1] = \"cde\";\n"
        "    return s.apply(b) + recur(5) + names[0][0].length() + names[0][1].length();\n"
        "  }\n"
        "}\n",
        {
            {"run(3, 4)", "43"},
            {"run(-2, 5)", "15"},
        });
}

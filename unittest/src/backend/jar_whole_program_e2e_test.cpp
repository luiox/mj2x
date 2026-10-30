// jar 级整体程序 E2E（#820 补"整体测试"缺口）。
//
// 现有 E2E 大多以单类/单方法为单位（mtest_set 矩阵、backend e2e 的 inline case）。
// 本文件验证**完整多类 jar** 的端到端流水线：
//   javac 多类程序 → jar 打包 → 基线运行（纯 Java）→ mj2x-cli jar→jar + --compile
//   （改写类 + 生成 C + 编译 DLL + 内嵌 META-INF/mj2x/native/）→ 运行输出与基线
//   逐行一致。
//
// 程序覆盖语义核心面：算术循环 / 位运算 / 递归 / 对象字段 / 数组 / 字符串拼接
// （invokedynamic）/ try-catch 异常 / 反射 / 集合接口调用 / 枚举语义（#1188
// EnumSemCheck 收编：values/valueOf round-trip + try 块内 valueOf 非法名 catch）。
// 三后端参数化：stackc / regc / vmc4（vmc4 额外覆盖 --vmc4-granularity program）。

#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / probeJdk

namespace {

// ---------------------------------------------------------------------------
// 工具链：javac + java + jar + mj2x-cli + vcvars64。
// ---------------------------------------------------------------------------
struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    jarTool;      // 与 javac 同目录（真实 JDK bin）
    fs::path    mj2xCli;
    fs::path    vcvars64;
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
    tc.javac  = jdk.javac;
    tc.java   = jdk.java;
    tc.jarTool = jdk.javac.parent_path() / "jar";
#ifdef _WIN32
    tc.jarTool += ".exe";
#endif

    tc.mj2xCli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.mj2xCli)) {
        tc.reason = "mj2x-cli.exe 未构建（请先 xmake build mj2x-cli）。期望路径: " +
                    tc.mj2xCli.string();
        return tc;
    }

    tc.vcvars64 = jdk_finder::find_vcvars_all();
    if (tc.vcvars64.empty()) {
        tc.reason = "未找到 VS vcvars64.bat（需要 MSVC 编译生成的 .c）";
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
        out += line;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
// 整体程序：app 包 6 类，覆盖语义核心面。
// ---------------------------------------------------------------------------
struct WholeProgram
{
    // 类名 → Java 源码（包 app）。
    std::vector<std::pair<std::string, std::string>> classes;
};

WholeProgram makeWholeProgram()
{
    WholeProgram program;
    program.classes = {
        {"Main",
         "package app;\n"
         "public class Main {\n"
         "  public static void main(String[] args) {\n"
         "    System.out.println(\"calc=\" + Calc.sum(10));\n"
         "    System.out.println(\"bits=\" + Calc.bits(0x5A5A5A5A));\n"
         "    System.out.println(\"fib=\" + Calc.fib(8));\n"
         "    Store s = new Store(7); s.bump();\n"
         "    System.out.println(\"store=\" + s.get());\n"
         "    System.out.println(\"arr=\" + Calc.arrSum(4));\n"
         "    System.out.println(\"str=\" + Str.wrap(\"hi\"));\n"
         "    System.out.println(\"ex=\" + Ex.guard(5));\n"
         "    System.out.println(\"ex2=\" + Ex.guard(-2));\n"
         "    System.out.println(\"refl=\" + Refl.name());\n"
         "    System.out.println(\"enumsem=\" + EnumSem.check());\n"
         "    java.util.List<Integer> l = new java.util.ArrayList<>();\n"
         "    l.add(3); l.add(4);\n"
         "    System.out.println(\"col=\" + (l.get(0) + l.get(1)));\n"
         "  }\n"
         "}\n"},
        {"Calc",
         "package app;\n"
         "public class Calc {\n"
         "  public static int sum(int n) { int acc = 0; for (int i = 0; i < n; i++) acc += i; return acc; }\n"
         "  public static int bits(int v) { return (v << 3) | ((v >>> 1) ^ (v & 0x7F)); }\n"
         "  public static int fib(int n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }\n"
         "  public static int arrSum(int n) {\n"
         "    int[] a = new int[n]; for (int i = 0; i < n; i++) a[i] = i;\n"
         "    int t = 0; for (int i = 0; i < n; i++) t += a[i]; return t;\n"
         "  }\n"
         "}\n"},
        {"Store",
         "package app;\n"
         "public class Store {\n"
         "  private int value;\n"
         "  public Store(int v) { value = v; }\n"
         "  public void bump() { value++; }\n"
         "  public int get() { return value; }\n"
         "}\n"},
        {"Str",
         "package app;\n"
         "public class Str { public static String wrap(String s) { return s + \"!\"; } }\n"},
        {"Ex",
         "package app;\n"
         "public class Ex {\n"
         "  public static int guard(int x) {\n"
         "    try {\n"
         "      if (x < 0) throw new IllegalArgumentException(\"neg\");\n"
         "      return x * 2;\n"
         "    } catch (IllegalArgumentException e) { return x + 4; }\n"
         "  }\n"
         "}\n"},
        {"Refl",
         "package app;\n"
         "public class Refl {\n"
         "  public static String name() {\n"
         "    try { return Class.forName(\"app.Refl\").getName(); }\n"
         "    catch (Exception e) { return \"err\"; }\n"
         "  }\n"
         "}\n"},
        // #1188 EnumSemCheck 驱动收编（评审 N7）：枚举语义面在仓内可复核——
        // values() 遍历 / valueOf 合法名 round-trip（引用恒等）/ name/ordinal /
        // try 块内 valueOf 非法名必须 catch IAE（13-probe 外部门禁盲区：
        // EnumProbe 仅合法名、ExceptionProbe 无 valueOf-in-try）。
        // 门禁力（2026-10-29 实证，替代 #1188 评审 P2-1 旧局限注记）：#1193
        // 内联缓存（beforeCompile 预解析，与类序解耦）使三处 Color.valueOf
        // 调用点照常内联（CLI 日志 inline 3 calls in EnumSem.check；产物为
        // 内联进来的 javac valueOf 体——Enum.valueOf 2 参调用形态），其中
        // "NOPE"/"" 两处位于 try 内。本 E2E 对 #1188"内联受保护边"缺陷有真
        // 门禁力：变异测试（临时禁用 ssa_inline_passes 的保护继承登记）
        // regc 案例红、失败形态即预言的"挂起异常越过 try 边界"（NPE at
        // EnumSem.check(Native Method)），stackc/vmc4 不受 SSA 内联影响、
        // 回滚后复绿。SSA 层 InlinedBlocksInheritCallerExceptionProtection
        // 单测仍是第一道门，本 E2E 是端到端第二道。
        {"Color",
         "package app;\n"
         "public enum Color {\n"
         "  RED, GREEN, BLUE;\n"
         "}\n"},
        {"EnumSem",
         "package app;\n"
         "public class EnumSem {\n"
         "  public static String check() {\n"
         "    StringBuilder sb = new StringBuilder();\n"
         "    int ord = 0;\n"
         "    for (Color c : Color.values()) {\n"
         "      ord += c.ordinal();\n"
         "      if (Color.valueOf(c.name()) != c) sb.append(\"MISMATCH;\");\n"
         "    }\n"
         "    sb.append(\"ord=\").append(ord).append(\";\");\n"
         "    try {\n"
         "      Color bad = Color.valueOf(\"NOPE\");\n"
         "      sb.append(\"NO-CATCH:\").append(bad).append(\";\");\n"
         "    } catch (IllegalArgumentException e) {\n"
         "      sb.append(\"caught-IAE;\");\n"
         "    }\n"
         "    try {\n"
         "      sb.append(Color.valueOf(\"\").name());\n"
         "    } catch (IllegalArgumentException e) {\n"
         "      sb.append(\"caught-2\");\n"
         "    }\n"
         "    return sb.toString();\n"
         "  }\n"
         "}\n"},
    };
    return program;
}

class JarWholeProgramE2E : public ::testing::TestWithParam<const char*>
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "E2E 工具链不可用，跳过: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/jar_whole_program/" + name + "/.keep").parent_path();
    }

    void writeFile(const fs::path& path, const std::string& content) const
    {
        std::ofstream out(path);
        ASSERT_TRUE(out) << path;
        out << content;
    }

    void runCase(const char* backendTag)
    {
        const std::string backend(backendTag);
        fs::path          dir = caseDir("whole_program_" + backend);
        std::error_code   ec;
        fs::create_directories(dir, ec);
        ASSERT_FALSE(ec) << ec.message();

        // 1. 写 Java 源并编译。
        fs::path srcDir = dir / "src";
        fs::create_directories(srcDir, ec);
        const auto program = makeWholeProgram();
        std::string javacCmd = q(tc_.javac) + " -d " + q(dir / "classes") + " ";
        for (const auto& [name, source] : program.classes) {
            writeFile(srcDir / (name + ".java"), source);
            javacCmd += q(srcDir / (name + ".java")) + " ";
        }
        auto javac = runBat({javacCmd});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        // 2. jar 打包。
        fs::path inJar = dir / "app.jar";
        auto jarPack = runBat({q(tc_.jarTool) + " cf " + q(inJar) + " -C " + q(dir / "classes") +
                               " ."});
        ASSERT_EQ(jarPack.exitCode, 0) << jarPack.out;
        ASSERT_TRUE(fileExists(inJar));

        // 3. 基线运行（纯 Java）。
        auto baselineRun = runBat({q(tc_.java) + " -cp " + q(inJar) + " app.Main"});
        ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
        const auto baseline = splitLines(baselineRun.out);
        ASSERT_GE(baseline.size(), 9u) << baselineRun.out;

        // 4. mj2x jar → jar（改写 + 生成 C + 编译 DLL + 内嵌）。
        fs::path outJar = dir / "app_protected.jar";
        auto cli = runBat({q(tc_.mj2xCli) + " -i " + q(inJar) + " -o " + q(outJar) +
                           " --emit-c " + q(dir / "native.c") + " --backend " + backend +
                           " --compile --c-compiler msvc"});
        ASSERT_EQ(cli.exitCode, 0) << "mj2x-cli 失败:\n" << cli.out;
        ASSERT_TRUE(fileExists(outJar)) << cli.out;

        // 5. 运行改写 jar（NativeLoader 自动加载内嵌 DLL）并与基线对比。
        auto protectedRun = runBat({q(tc_.java) + " -cp " + q(outJar) + " app.Main"});
        ASSERT_EQ(protectedRun.exitCode, 0) << "protected java 运行失败:\n" << protectedRun.out;
        const auto actual = splitLines(protectedRun.out);

        ASSERT_EQ(baseline.size(), actual.size())
            << "基线:\n" << joinLines(baseline) << "\n保护后:\n" << joinLines(actual);
        for (size_t i = 0; i < baseline.size(); ++i) {
            EXPECT_EQ(baseline[i], actual[i]) << "第 " << i << " 行不一致";
        }
    }
};

Toolchain JarWholeProgramE2E::tc_;

}   // namespace

TEST_P(JarWholeProgramE2E, WholeProgramJarRoundTripPreservesSemantics)
{
    runCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(BackendMatrix, JarWholeProgramE2E,
                         ::testing::Values("stackc", "regc", "vmc4"),
                         [](const testing::TestParamInfo<const char*>& info) {
                             return std::string(info.param);
                         });

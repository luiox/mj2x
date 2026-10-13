// stackc_mtest_set_test.cpp — 消费 mtest_set 样本做 StackC native 转换 E2E
//
// 遍历 mtest_set::nativeFixtureManifest() 中 backends 含 StackC 的 case：
//   生成器产 .class bytes（基准）→ mj2x StackC 改写 + 生成 .c → cl 编 DLL
//   → java 加载 DLL 调用 native，对比与原 JVM 行为一致。
//
// 与 stackc_backend_e2e_test.cpp 的区别：样本来自 mtest_set 统一清单（跨产品共享），
// 而非每个测试手写 inline Java。工具链探测复用 test::jdk + libjdk_finder。
//
// 生成物写到 <project_root>/test/mj2x/mtest_set/<case_id>/。
// JDK/MSVC/mj2x-cli 任一缺失即 GTEST_SKIP，不污染 CI。

#include <gmock/gmock.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "manifest/native_fixture_manifest.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / probeJdk

/// 让 gtest 在失败时打印可读的 case id（默认会打印"72-byte object"）。
void PrintTo(const mtest_set::NativeFixtureCase& fixture, std::ostream* os)
{
    *os << "{" << fixture.id << "}";
}

namespace {

// mj2x-rt 源包容器根(#894 起 StackC 运行时 ABI 在 src/stackc,链接消费)。
fs::path rtSrcDir() { return fs::current_path() / "mj2x-rt" / "src"; }



/// StackC E2E 工具链：JdkToolchain + mj2x-cli + vcvars64（同 stackc_backend_e2e）。
struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    mj2xCli;
    fs::path    jdkInclude;
    fs::path    jdkIncludeWin;
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
    tc.javac         = jdk.javac;
    tc.java          = jdk.java;
    tc.jdkInclude    = jdk.jdkInclude;
    tc.jdkIncludeWin = jdk.jdkIncludeWin;

    tc.mj2xCli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.mj2xCli)) {
        tc.reason = "mj2x-cli 未构建（请先 xmake build mj2x-cli）: " + tc.mj2xCli.string();
        return tc;
    }
    tc.vcvars64 = jdk_finder::findVcvarsAll();
    if (tc.vcvars64.empty()) {
        tc.reason = "未找到 vcvarsall.bat（需要 MSVC）";
        return tc;
    }
    tc.available = true;
    return tc;
}

/// 从 generatedClass 路径（如 "arith/ArithAdd.class"）提取简单类名（"ArithAdd"）。
std::string simpleClassName(const std::string& generatedClass)
{
    std::string name = generatedClass;
    auto        slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

}   // namespace

class StackCMtestSetE2E : public ::testing::TestWithParam<mtest_set::NativeFixtureCase>
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) GTEST_SKIP() << "E2E 工具链不可用，跳过: " << tc_.reason;
    }
};

Toolchain StackCMtestSetE2E::tc_;

TEST_P(StackCMtestSetE2E, StackCNativeTransformMatchesJvm)
{
    const auto& fixture = GetParam();
    const auto  className = simpleClassName(fixture.generatedClass);

    fs::path dir = test::getOutPath("mj2x/mtest_set/" + std::string(fixture.id) + "/.keep")
                       .parent_path();
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_FALSE(ec) << "无法清理测试目录: " << dir << " 原因: " << ec.message();
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << "无法创建测试目录: " << dir << " 原因: " << ec.message();

    // 1. 用 mtest_set 生成器产基准 .class bytes（不走 javac 编译样本源）。
    ca::str::Utf8StringArena arena;
    auto bytes = fixture.generator(arena);
    ASSERT_FALSE(bytes.empty()) << "生成器未产出 bytes: " << fixture.id;
    fs::path inClass = dir / (className + ".class");
    {
        std::ofstream f(inClass, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    // 2. 写 Driver.java（调用被测方法，打印期望输出）。基准与 native 共用同一 Driver。
    fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream f(driverFile);
        f << "public class Driver {\n"
          << "  public static void main(String[] a) throws Exception {\n"
          << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
          << "    if (dll != null) System.load(dll);\n"
          << "    " << fixture.jvmRunnerBody << "\n"
          << "  }\n"
          << "}\n";
    }

    // 3. javac 编译 Driver（基准 .class 已在 classpath）。
    {
        auto r = runBat({q(tc_.javac) + " -encoding UTF-8 -cp " + q(dir) + " -d " + q(dir) +
                         " " + q(driverFile)});
        ASSERT_EQ(r.exitCode, 0) << "Driver javac 失败:\n" << r.out;
    }

    // 4. 跑基准 java 取期望输出。
    std::string baseline;
    {
        auto run = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
        ASSERT_EQ(run.exitCode, 0) << "基准 java 运行失败:\n" << run.out;
        baseline = run.out;
    }

    // 5. mj2x StackC 改写基准 .class + 生成 .c。
    fs::path outClass = dir / (className + "_protected.class");
    fs::path cFile    = dir / (className + ".c");
    {
        auto r = runBat({q(tc_.mj2xCli) + " -i " + q(inClass) + " -o " + q(outClass) +
                         " --emit-c " + q(cFile)});
        ASSERT_EQ(r.exitCode, 0) << "mj2x-cli 失败:\n" << r.out;
        ASSERT_TRUE(fileExists(cFile)) << "mj2x 未生成 .c";
    }
    // 用改写后的类替换原类（Driver 按 <className> 加载）。
    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing);

    // 6. cl 编译 .c → DLL(运行时源单独编译再链接,避开多源 /Fo 引号坑)。
    fs::path dll = dir / (className + ".dll");
    {
        auto r = runBat({
            "call " + q(tc_.vcvars64) + " x64 >nul",
            "cl /nologo /O2 /EHsc /utf-8 /c /Fo" + q(dir / "_rt.obj") + " " +
                q(rtSrcDir() / "stackc" / "mj2x_stackc_runtime.c") + " /I" + q(rtSrcDir()) + " /I" +
                q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin),
            "cl /nologo /LD /O2 /EHsc /utf-8 /Fe" + q(dll) + " /Fo" + q(dir / "_") + " " +
                q(cFile) + " " + q(dir / "_rt.obj") + " /I" + q(rtSrcDir()) + " /I" +
                q(tc_.jdkInclude) + " /I" + q(tc_.jdkIncludeWin),
        });
        ASSERT_EQ(r.exitCode, 0) << "cl 编译失败:\n" << r.out;
        ASSERT_TRUE(fileExists(dll)) << "cl 未生成 .dll";
    }

    // 7. java 加载 DLL 调用 native，对比基准。
    std::string actual;
    {
        auto run = runBat({
            "set \"MORPHER_DLL=" + dll.string() + "\"",
            q(tc_.java) + " -cp " + q(dir) + " Driver",
        });
        ASSERT_EQ(run.exitCode, 0) << "protected java 运行失败:\n" << run.out;
        actual = run.out;
    }

    // runBat 在 Windows 捕获 CRLF，基准与 native 都走同一捕获路径，故直接对比即可。
    EXPECT_EQ(baseline, actual) << "native 输出与 JVM 基准不一致 (case=" << fixture.id << ")";
}

INSTANTIATE_TEST_SUITE_P(
    MtestSetSamples,
    StackCMtestSetE2E,
    ::testing::ValuesIn(mtest_set::nativeFixtureManifest()));

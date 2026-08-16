// VMC4 protected-sample E2E：test_resource/protected_samples/vmc4 的提交样本经
// mj2x-cli（--backend vmc4）虚拟化后，native 运行输出必须与 JVM 基线逐字一致。
//
// 与 vmc4_backend_e2e_test（Pipeline 直连、内存样本）互补：这里走完整 CLI 链路
// + 提交的 protected sample（含 JNI helper surface），是 capabilities.md §4
// 「调用/字段/数组/分配/异常」面与红队 trace-surface 契约（TRACE_SURFACE.md）
// 的行为守门。产物落 test/mj2x/protected_samples/vmc4/。

#include <gmock/gmock.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / probeJdk

namespace {

std::string normalizeOutput(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

struct Toolchain
{
    bool         available = false;
    std::string  reason;
    JdkToolchain jdk;
    fs::path     cli;
};

Toolchain probeToolchain()
{
    Toolchain tc;
    ProbeOptions opts;
    opts.requireJniHeader = true;
    tc.jdk = probeJdk(opts);
    if (!tc.jdk.available) {
        tc.reason = tc.jdk.reason;
        return tc;
    }
    tc.cli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.cli)) {
        tc.reason = "mj2x-cli not built: " + tc.cli.string();
        return tc;
    }
    tc.available = true;
    return tc;
}

class Vmc4ProtectedSamples : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) GTEST_SKIP() << "vmc4 sample toolchain unavailable: " << tc_.reason;
    }

    fs::path sampleDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/protected_samples/vmc4/" + name + "/.keep").parent_path();
    }
};

Toolchain Vmc4ProtectedSamples::tc_;

// Vmc4JniSurfaceSample：JNI helper 面（字段/数组/分配/接口分派/异常）+ 纯计算
// 之外的全部 callsite helper 路径。基线输出与 TRACE_SURFACE.md 的契约一致。
TEST_F(Vmc4ProtectedSamples, JniSurfaceSampleRoundTrip)
{
    const fs::path dir = sampleDir("Vmc4JniSurfaceSample");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path src =
        test::getResourcePath("protected_samples/vmc4/Vmc4JniSurfaceSample.java");
    ASSERT_TRUE(fileExists(src)) << src.string();

    const auto javac = runBat({q(tc_.jdk.javac) + " -g -encoding UTF-8 -d " + q(dir) + " -cp " +
                               q(dir) + " " + q(src)});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;
    const fs::path mainClass =
        dir / "protected_samples" / "vmc4" / "Vmc4JniSurfaceSample.class";
    ASSERT_TRUE(fileExists(mainClass)) << mainClass.string();

    const std::string mainFqcn = "protected_samples.vmc4.Vmc4JniSurfaceSample";
    // Driver 预装载 DLL（MORPHER_DLL）：RegisterNatives 桥的 JNI_OnLoad 必须在
    // 样本 clinit 之前执行。基线与 native 共用同一 Driver（基线不设 env）。
    const fs::path driver = dir / "Driver.java";
    {
        std::ofstream out(driver);
        out << "public class Driver {\n"
               "  public static void main(String[] args) throws Exception {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    "
            << mainFqcn << ".main(args);\n"
               "  }\n"
               "}\n";
    }
    const auto driverCompile = runBat({q(tc_.jdk.javac) + " -encoding UTF-8 -cp " + q(dir) +
                                       " -d " + q(dir) + " " + q(driver)});
    ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

    const auto baselineRun = runBat({q(tc_.jdk.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
    const std::string expected =
        "scale=26\n"
        "sumArray=6\n"
        "fillSum=33\n"
        "throwCatch(neg)=-1\n"
        "throwCatch(pos)=14\n"
        "greet=hi vmc4!\n"
        "dispatch=7\n";
    ASSERT_EQ(normalizeOutput(baselineRun.out), expected) << baselineRun.out;

    // protected 输出放包目录内：cloader/Loader sidecar 按 classpath 根写出，
    // 输出类带包名时 sidecar 必须落回 dir/mj2x/cloader/（见 output_writer
    // sidecarClassPath）。
    const fs::path protectedClass =
        dir / "protected_samples" / "vmc4" / "Vmc4JniSurfaceSample_protected.class";
    const fs::path cFile = dir / "Vmc4JniSurfaceSample.c";
    const auto     transform =
        runBat({q(tc_.cli) + " -i " + q(mainClass) + " -o " + q(protectedClass) +
                " --backend vmc4 --emit-c " + q(cFile) + " --compile"});
    ASSERT_EQ(transform.exitCode, 0) << transform.out;
    ASSERT_TRUE(fileExists(protectedClass)) << protectedClass.string();

    // 结构断言：VM 解释器 + RegisterNatives 桥 + per-callsite helper 表。
    std::ifstream cIn(cFile, std::ios::binary);
    std::ostringstream cBuf;
    cBuf << cIn.rdbuf();
    const std::string generatedC = cBuf.str();
    EXPECT_NE(generatedC.find("mj2x_vm_exec_"), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("registerNativesForClass"), std::string::npos) << generatedC;
    EXPECT_NE(generatedC.find("JNI_OnLoad"), std::string::npos) << generatedC;

    const fs::path dll = protectedClass.parent_path() /
                         (protectedClass.stem().string() + ".dll");
    ASSERT_TRUE(fileExists(dll)) << dll.string();
    fs::copy_file(protectedClass, mainClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto nativeRun = runBat({std::string("set \"MORPHER_DLL=") + dll.string() + "\"",
                                   q(tc_.jdk.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(nativeRun.exitCode, 0) << nativeRun.out;
    EXPECT_EQ(normalizeOutput(nativeRun.out), expected) << nativeRun.out;
}

}   // namespace

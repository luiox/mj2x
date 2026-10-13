// regc_perf_regression_test.cpp - #820 步骤 5：RegC 13-probe 性能回归自动化。
//
// 门控：设 MJ2X_PERF_REGRESSION=1 才执行（性能测试 = 全量转换 + native 编译 +
// 3 轮取样，约 2-3 分钟，不进默认回归）。输入 jar 默认取
// D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar（可经 MJ2X_PERF_INPUT_JAR
// 覆盖）；缺失时 SKIP。断言委托 benchmark 脚本的 -Regression 模式：13-probe 全过
// + native 覆盖率 ≥ 基线×0.9 + RegC total ≤ 基线×2.0，失败退出非零。

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "test_helper.hpp"

namespace {

namespace fs = std::filesystem;

TEST(RegcPerfRegression, ThirteenProbesWithinBaseline)
{
    const char* gate = std::getenv("MJ2X_PERF_REGRESSION");
    if (gate == nullptr || std::string(gate) != "1") {
        GTEST_SKIP() << "性能回归门控：设 MJ2X_PERF_REGRESSION=1 才执行（耗时 2-3 分钟）";
    }

    const auto jdk = test::jdk::probeJdk();
    ASSERT_TRUE(jdk.available) << jdk.reason;

    const fs::path repo = test::getTopProjectPath();
    const fs::path script = repo / "benchmark" / "jvm_obfuscator_test_benchmark.ps1";
    ASSERT_TRUE(test::jdk::fileExists(script)) << script.string();

    const fs::path cli = test::getBuildTargetPath("mj2x-cli");
    ASSERT_TRUE(test::jdk::fileExists(cli)) << cli.string() << "（先构建 mj2x-cli）";

    const char* inputEnv = std::getenv("MJ2X_PERF_INPUT_JAR");
    fs::path    inputJar = inputEnv != nullptr ? fs::path(inputEnv)
                                               : fs::path("D:\\WorkSpace\\jvm-obf-tester\\target\\jvm-obf-tester.jar");
    ASSERT_TRUE(test::jdk::fileExists(inputJar)) << inputJar.string() << "（13-probe 输入 jar 缺失）";

    std::string cmd = "pwsh -NoProfile -ExecutionPolicy Bypass -File " + test::jdk::q(script) +
                      " -Runs 3 -Regression -SkipBuild -InputJar " + test::jdk::q(inputJar) +
                      " -JdkHome " + test::jdk::q(jdk.javaHome);
    const auto result = test::jdk::runBat({cmd});
    EXPECT_EQ(result.exitCode, 0) << "性能回归失败（13-probe 全过 + 覆盖率 + 总耗时阈值），输出:\n"
                                  << result.out;
    if (result.exitCode != 0) {
        // 输出过长时截断到最近 4KB。
        constexpr std::size_t kTail = 4096;
        if (result.out.size() > kTail) {
            std::cerr << result.out.substr(result.out.size() - kTail) << "\n";
        } else {
            std::cerr << result.out << "\n";
        }
    }
}

} // namespace

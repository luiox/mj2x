// VMC4 转化率报告（Phase 2.E 收口）：用真实 fixture（java_obfuscator_style 四
// 类：Array/Calc/String kernel + Main）经 VMC4 后端处理，统计业务方法转 VM 的
// 比例与 fail-closed 原因分布，写报告到 test/mj2x/vmc4_conversion_report.md。
//
// 报告输出遵循 AGENTS.md 测试输出规范（<project_root>/test/ 下，经 test_helper）。
// 转化率数值不在此断言（fixture 演化会变），断言只保证报告生成 + 结构字段齐全。

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// fd 级 stderr 捕获：MSVC CRT 用下划线前缀，POSIX 无前缀
// （与 mjt/unittest/commands_test.cpp 同一口径）。
#ifdef _WIN32
#include <io.h>
#define TEST_DUP(fd) _dup(fd)
#define TEST_DUP2(from, to) _dup2((from), (to))
#define TEST_CLOSE(fd) _close(fd)
#define TEST_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define TEST_DUP(fd) dup(fd)
#define TEST_DUP2(from, to) dup2((from), (to))
#define TEST_CLOSE(fd) close(fd)
#define TEST_FILENO(f) fileno(f)
#endif

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/logging.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / probeJdk / JdkToolchain

namespace {

constexpr const char* kFixtureSrc = "test_resource/benchmark/java_obfuscator_style/src";

struct ConversionCounts
{
    int    total       = 0;   ///< MarkAll 标记的业务方法数
    int    virtualized = 0;   ///< 成功转 VM
    int    skipped     = 0;   ///< fail-closed（保留字节码）
    std::vector<std::pair<std::string, std::string>> details;   ///< (方法签名, 状态/原因)
};

/// 解析 pipeline 日志（stderr，经 libca log 门面）的 Compiling / Skipping 行。
///
/// 日志语义（pipeline.cpp compileMethods）：每个标记方法先打
/// "  Compiling <类>.<名><描述>"（pipeline target，INFO），VMC4 编码失败再打
/// "<...> Skipping <类>.<名><描述>: <原因>"（vmc4 target，WARN，SimpleLogBackend
/// 自带 [LEVEL] [target] 前缀）。故：方法总数 = Compiling 数；转 VM = Compiling - Skipping。
ConversionCounts parsePipelineLog(const std::string& log)
{
    ConversionCounts counts;
    std::istringstream in(log);
    std::string        line;
    while (std::getline(in, line)) {
        if (line.find("Compiling ") != std::string::npos) {
            counts.total++;
            const std::string sig = line.substr(line.find("Compiling ") + 10);
            counts.details.emplace_back(sig, "");
        } else if (line.find("Skipping ") != std::string::npos) {
            const size_t skipPos = line.find("Skipping ") + 9;
            const size_t colon   = line.find(": ", skipPos);
            const std::string sig =
                colon == std::string::npos ? line.substr(skipPos) : line.substr(skipPos, colon - skipPos);
            const std::string reason = colon == std::string::npos ? "?" : line.substr(colon + 2);
            counts.skipped++;
            // 回填该方法的 fail-closed 原因（按签名匹配最后一条 Compiling）。
            for (auto it = counts.details.rbegin(); it != counts.details.rend(); ++it) {
                if (it->first == sig && it->second.empty()) {
                    it->second = "fail-closed: " + reason;
                    break;
                }
            }
        }
    }
    // 剩余空状态的方法 = 成功转 VM。
    for (auto& [sig, status] : counts.details) {
        if (status.empty()) {
            status = "virtualized";
            counts.virtualized++;
        }
    }
    return counts;
}

class Vmc4ConversionTest : public ::testing::Test
{
protected:
    static JdkToolchain tc_;

    static void SetUpTestSuite()
    {
        ProbeOptions opts;
        opts.requireJniHeader = false;
        tc_ = probeJdk(opts);
    }

    void SetUp() override
    {
        if (!tc_.available) GTEST_SKIP() << "VMC4 conversion report requires a JDK: " << tc_.reason;
        // 日志走 libca log 门面（stderr），测试捕获 stderr；未初始化时宏调用静默丢弃。
        mj2x::initLogging(ca::log::Level::Debug);
    }
};

JdkToolchain Vmc4ConversionTest::tc_;

TEST_F(Vmc4ConversionTest, FixtureConversionReport)
{
    const fs::path outDir = test::getOutPath("mj2x/vmc4_conversion").parent_path();
    std::error_code ec;
    fs::create_directories(outDir, ec);

    // 编译 fixture 源（pack/bench/ArrayKernel + CalcKernel + StringKernel + pack/Main）。
    const fs::path fixtureRoot =
        fs::current_path() / kFixtureSrc;
    std::vector<std::string> sources;
    for (const auto& entry : fs::recursive_directory_iterator(fixtureRoot)) {
        if (entry.path().extension() == ".java") sources.push_back(q(entry.path().string()));
    }
    ASSERT_FALSE(sources.empty()) << "no fixture sources found under " << fixtureRoot;
    std::string javacCmd = q(tc_.javac) + " -d " + q(outDir);
    for (const auto& s : sources) javacCmd += " " + s;
    auto javac = runBat({javacCmd});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    // 打包 TEST-fixture.jar（jar 输入更贴近真实业务 jar 的加载路径）。
    const fs::path jarTool = tc_.javaHome / "bin" / "jar";
    const fs::path jarPath = outDir / "TEST-fixture.jar";
    // cd /d 是 cmd 语法（跨盘）；POSIX 下普通 cd 即可，否则 sh 把 /d 当目标目录。
#ifdef _WIN32
    auto jar = runBat({"cd /d " + q(outDir) + " && " + q(jarTool) + " cf " + q(jarPath) +
                       " pack"});
#else
    auto jar = runBat({"cd " + q(outDir) + " && " + q(jarTool) + " cf " + q(jarPath) +
                       " pack"});
#endif
    ASSERT_EQ(jar.exitCode, 0) << jar.out;
    ASSERT_TRUE(fileExists(jarPath));

    // pipeline：VMC4 + MarkAll + CLoader（与 e2e 相同配置，drift 全开）。
    const fs::path outJar = outDir / "TEST-fixture_vmc4.jar";
    mj2x::PipelineConfig config;
    config.inputPath  = jarPath.string();
    config.outputPath = outJar.string();
    mj2x::Pipeline pipeline(std::move(config));
    auto backend = std::make_unique<mj2x::Vmc4Backend>();
    mj2x::Vmc4Options opts;
    opts.driftVariants = 2;
    opts.driftPeriod   = 16;
    backend->setOptions(opts);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());

    // 捕获 pipeline stderr（日志门面输出：Compiling/Skipping 行）。
    const fs::path logPath = outDir / "pipeline.log";
    fflush(stderr);
    const int saved = TEST_DUP(TEST_FILENO(stderr));
    ASSERT_GE(saved, 0);
    FILE* logFile = freopen(logPath.string().c_str(), "w", stderr);
    ASSERT_NE(logFile, nullptr);
    const int runResult = pipeline.run();
    fflush(stderr);
    TEST_DUP2(saved, TEST_FILENO(stderr));
    TEST_CLOSE(saved);

    std::ifstream logIn(logPath);
    std::ostringstream logBuf;
    logBuf << logIn.rdbuf();
    const std::string log = logBuf.str();
    ASSERT_EQ(runResult, 0) << "pipeline failed:\n" << log;

    const ConversionCounts counts = parsePipelineLog(log);
    ASSERT_GT(counts.total, 0) << "fixture must have business methods:\n" << log;

    // 写报告（test/mj2x/vmc4_conversion_report.md）。
    const fs::path reportPath = outDir / "vmc4_conversion_report.md";
    {
        std::ofstream out(reportPath);
        out << "# VMC4 Conversion Report\n\n";
        out << "- fixture: `" << kFixtureSrc << "`\n";
        out << "- jdk home: " << tc_.javaHome.string() << "\n";
        out << "- generated: pipeline（VMC4, drift 2 变体 × 16 周期, MarkAll + CLoader）\n\n";
        out << "## 统计\n\n";
        out << "| 指标 | 数值 |\n|---|---|\n";
        out << "| 业务方法总数 | " << counts.total << " |\n";
        out << "| 转 VM | " << counts.virtualized << " ("
            << (counts.total ? (counts.virtualized * 100 / counts.total) : 0) << "%) |\n";
        out << "| fail-closed | " << counts.skipped << " ("
            << (counts.total ? (counts.skipped * 100 / counts.total) : 0) << "%) |\n\n";
        // 原因分布（按 reason 前缀聚合）。
        out << "## fail-closed 原因分布\n\n";
        std::map<std::string, int> reasonBuckets;
        for (const auto& [sig, status] : counts.details) {
            if (status.rfind("fail-closed:", 0) == 0) {
                std::string reason = status.substr(std::string("fail-closed: ").size());
                reasonBuckets[reason]++;
            }
        }
        out << "| 原因 | 方法数 |\n|---|---|\n";
        for (const auto& [reason, n] : reasonBuckets) {
            out << "| `" << reason << "` | " << n << " |\n";
        }
        // 方法明细。
        out << "\n## 方法明细\n\n";
        out << "| 方法 | 状态 |\n|---|---|\n";
        for (const auto& [sig, status] : counts.details) {
            out << "| `" << sig << "` | " << status << " |\n";
        }
    }
    ASSERT_TRUE(fileExists(reportPath));

    // 结构断言：报告含统计表 + 明细 + 转化率数值。
    std::ifstream repIn(reportPath);
    std::ostringstream repBuf;
    repBuf << repIn.rdbuf();
    const std::string report = repBuf.str();
    EXPECT_NE(report.find("业务方法总数"), std::string::npos) << report;
    EXPECT_NE(report.find("转 VM"), std::string::npos) << report;
    EXPECT_NE(report.find("fail-closed 原因分布"), std::string::npos) << report;
    EXPECT_NE(report.find("方法明细"), std::string::npos) << report;
}

}   // namespace

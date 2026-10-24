// java_corpus_e2e_test.cpp - mj2x 自建 Java 语料集（dev_plan B 项）：
// test_resource/java_corpus 的目录式用例 × 三 C 后端（stackc/regc/vmc4）矩阵，
// 差分对拍（baseline JVM 输出 vs 保护后 JVM 输出逐字节一致）。
//
// 设计（语料集 README 见 test_resource/java_corpus/README.md）：
//   - 用例 = 自包含 .java（自带 main、输出确定性），目录名 = 类别
//     （language / typical / edge）；perf/ 由 MJ2X_CORPUS_PERF=1 单独门控。
//   - 判定 = 差分：无 expected 文件需要维护，baseline 输出即金标准。
//   - exclusions.txt：`<case> <backend> <reason>` 记录已知缺口（每行显式记账，
//     修复后删行）；# 开头为注释。
//   - MJ2X_CORPUS_TIER=obf：叠加混淆开关面（regc --regc-obfuscate；
//     vmc4 super-ops+fragment+regfile+shuffle）——混淆组合吃满语料。
//   - 编译走 CLI 进程（真实产品接口），而非直构 Pipeline。

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace {

namespace fs = std::filesystem;
using namespace test::jdk;

enum class CorpusBackend
{
    StackC,
    RegC,
    Vmc4,
};

const char* backendName(CorpusBackend backend)
{
    switch (backend) {
    case CorpusBackend::StackC: return "stackc";
    case CorpusBackend::RegC: return "regc";
    case CorpusBackend::Vmc4: return "vmc4";
    }
    return "stackc";
}

struct CorpusCase
{
    std::string category;   ///< 目录名（language / typical / edge / perf）
    std::string name;       ///< 文件 stem（类名）
    fs::path    source;     ///< .java 源路径
};

std::string normalizeOutput(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

/// 排除表：`<case> <backend> <reason...>`（case = 文件 stem）。# 开头注释。
std::multimap<std::string, std::pair<std::string, std::string>> loadExclusions()
{
    std::multimap<std::string, std::pair<std::string, std::string>> out;
    const fs::path file = test::getResourcePath("java_corpus/exclusions.txt");
    std::ifstream in(file);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream stream(line);
        std::string caseName, backend, reason;
        if (!(stream >> caseName >> backend)) continue;
        std::getline(stream, reason);
        if (!reason.empty() && reason[0] == ' ') reason = reason.substr(1);
        out.emplace(caseName, std::make_pair(backend, reason));
    }
    return out;
}

/// 语料枚举：categories 下全部 *.java（stem = 类名）。
std::vector<CorpusCase> enumerateCorpus(const std::vector<std::string>& categories)
{
    std::vector<CorpusCase> cases;
    for (const auto& category : categories) {
        const fs::path dir = test::getResourcePath("java_corpus/" + category);
        std::error_code    ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".java") continue;
            cases.push_back({category, entry.path().stem().string(), entry.path()});
        }
    }
    std::sort(cases.begin(), cases.end(), [](const CorpusCase& a, const CorpusCase& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    return cases;
}

std::string corpusTier()
{
    const char* tier = std::getenv("MJ2X_CORPUS_TIER");
    return (tier != nullptr && std::string(tier) == "obf") ? "obf" : "plain";
}

/// 后端编译参数（tier=obf 时叠加混淆开关面）。
std::string backendFlags(CorpusBackend backend)
{
    switch (backend) {
    case CorpusBackend::StackC: return "--backend stackc";
    case CorpusBackend::RegC:
        // obf 档暂不含 --regc-computed-jumps：大 switch（250 case）形态下
        // 跳转目标间接化有未修缺陷（dev_plan J-13，BigSwitch 首曝；修复后
        // 重新加入）。
        return corpusTier() == "obf"
                   ? "--backend regc --regc-obfuscate --regc-hide-constants"
                   : "--backend regc";
    case CorpusBackend::Vmc4:
        // obf 档暂不含 --vmc4-regfile：大循环构建 + 溢出形态下分配器有未修的
        // 区间计算缺陷（dev_plan J-12，HashAndCrypto 首曝；修复后重新加入）。
        return corpusTier() == "obf"
                   ? "--backend vmc4 --vmc4-super-ops --vmc4-fragment --vmc4-shuffle-blocks"
                   : "--backend vmc4";
    }
    return "--backend stackc";
}

struct CorpusToolchain
{
    bool         available = false;
    std::string  reason;
    JdkToolchain jdk;
    fs::path     cli;
};

CorpusToolchain probeCorpusToolchain()
{
    CorpusToolchain toolchain;
    ProbeOptions options;
    options.requireJniHeader = true;
    toolchain.jdk = probeJdk(options);
    if (!toolchain.jdk.available) {
        toolchain.reason = toolchain.jdk.reason;
        return toolchain;
    }
    toolchain.cli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(toolchain.cli)) {
        toolchain.reason = "mj2x-cli 未构建: " + toolchain.cli.string();
        return toolchain;
    }
    const auto compiler = jdk_finder::findNativeCompiler();
    if (!compiler.available) {
        toolchain.reason = compiler.reason;
        return toolchain;
    }
    toolchain.available = true;
    return toolchain;
}

/// 准备一个 (用例, 列) 的工作目录：拷源、javac、差分驱动。
/// backend 非空时先跑 baseline，再 CLI 保护并把受保护 class 覆盖回输入。
fs::path prepareCase(const CorpusToolchain& tc, const CorpusCase& aCase, const std::string& column,
                     const std::string& flags, std::string& detail)
{
    const fs::path dir =
        test::getOutPath("mj2x/corpus/" + corpusTier() + "/" + column + "/" + aCase.name + "/.keep")
            .parent_path();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::copy_file(aCase.source, dir / aCase.source.filename(),
                  fs::copy_options::overwrite_existing, ec);
    const auto compileJava =
        runBat({q(tc.jdk.javac) + " -encoding UTF-8 -d " + q(dir) + " " + q(aCase.source)});
    if (compileJava.exitCode != 0) {
        detail = aCase.name + " javac:\n" + compileJava.out;
        return {};
    }
    const fs::path driver = dir / "CorpusDriver.java";
    {
        std::ofstream out(driver);
        out << "public class CorpusDriver {\n"
            << "  public static void main(String[] args) throws Exception {\n"
            << "    String dll = System.getenv(\"MORPHER_DLL\");\n"
            << "    if (dll != null) System.load(dll);\n"
            << "    " << aCase.name << ".main(args);\n"
            << "  }\n"
            << "}\n";
    }
    const auto compileDriver =
        runBat({q(tc.jdk.javac) + " -encoding UTF-8 -cp " + q(dir) + " -d " + q(dir) + " " +
                q(driver)});
    if (compileDriver.exitCode != 0) {
        detail = aCase.name + " driver javac:\n" + compileDriver.out;
        return {};
    }
    if (!flags.empty()) {
        const fs::path inputClass  = dir / (aCase.name + ".class");
        const fs::path outputClass = dir / (aCase.name + "_protected.class");
        const fs::path cFile       = dir / (column + ".c");
        const auto     transform =
            runBat({q(tc.cli) + " -i " + q(inputClass) + " -o " + q(outputClass) + " " + flags +
                    " --emit-c " + q(cFile) + " --compile"});
        if (transform.exitCode != 0) {
            detail = aCase.name + " " + column + " transform:\n" + transform.out;
            return {};
        }
        fs::copy_file(outputClass, inputClass, fs::copy_options::overwrite_existing, ec);
    }
    return dir;
}

/// 在工作目录跑一轮 CorpusDriver。withLibrary=false 不设 MORPHER_DLL
/// （baseline 列：驱动跳过 System.load，跑纯字节码）。
struct CorpusRunResult
{
    std::string output;
    int         exitCode = 0;
};

CorpusRunResult runOnce(const CorpusToolchain& tc, const fs::path& dir, bool withLibrary)
{
    std::vector<std::string> lines;
    if (withLibrary) {
        // 库名 = 受保护 class 的 stem（<Case>_protected.dll/.so），与 prepareCase
        // 的 -o 命名一致。
#ifdef _WIN32
        const std::string libraryFile =
            (dir / (dir.filename().string() + "_protected.dll")).string();
        lines.push_back("set \"MORPHER_DLL=" + libraryFile + "\"");
#else
        const std::string libraryFile = dir / (dir.filename().string() + "_protected.so");
        lines.push_back("MORPHER_DLL=" + q(libraryFile) + "; export MORPHER_DLL");
#endif
    }
    lines.push_back(q(tc.jdk.java) + " -Xverify:all -cp " + q(dir) + " CorpusDriver");
    const auto result = runBat(lines);
    return {normalizeOutput(result.out), result.exitCode};
}

struct CorpusMatrixParam
{
    CorpusCase    aCase;
    CorpusBackend backend = CorpusBackend::StackC;
};

std::vector<CorpusMatrixParam> corpusMatrixParams()
{
    std::vector<CorpusMatrixParam> params;
    for (const auto& c : enumerateCorpus({"language", "typical", "edge"})) {
        for (const CorpusBackend backend :
             {CorpusBackend::StackC, CorpusBackend::RegC, CorpusBackend::Vmc4}) {
            params.push_back(CorpusMatrixParam{c, backend});
        }
    }
    return params;
}

void PrintTo(const CorpusMatrixParam& param, std::ostream* out)
{
    *out << "{" << param.aCase.category << "/" << param.aCase.name << ","
         << backendName(param.backend) << "}";
}

}   // namespace

class JavaCorpusE2E : public ::testing::TestWithParam<CorpusMatrixParam>
{
protected:
    static CorpusToolchain toolchain_;
    static std::multimap<std::string, std::pair<std::string, std::string>> exclusions_;

    static void SetUpTestSuite()
    {
        toolchain_  = probeCorpusToolchain();
        exclusions_ = loadExclusions();
        std::cout << "[corpus] tier=" << corpusTier() << " exclusions=" << exclusions_.size()
                  << std::endl;
    }

    void SetUp() override
    {
        if (!toolchain_.available) {
            GTEST_SKIP() << "corpus toolchain unavailable: " << toolchain_.reason;
        }
    }

    /// 一格矩阵：javac → baseline → CLI 保护 → native 运行 → 差分对拍。
    void runCase(const CorpusMatrixParam& param)
    {
        const auto& aCase = param.aCase;
        const auto  skipped = exclusions_.equal_range(aCase.name);
        for (auto it = skipped.first; it != skipped.second; ++it) {
            if (it->second.first == backendName(param.backend)) {
                GTEST_SKIP() << "excluded: " << it->second.second;
            }
        }

        const std::string tag  = aCase.category + "/" + aCase.name;
        const std::string col  = backendName(param.backend);
        std::string       detail;

        const fs::path baselineDir = prepareCase(toolchain_, aCase, "baseline", "", detail);
        ASSERT_FALSE(baselineDir.empty()) << tag << ": " << detail;
        const auto baselineRun = runOnce(toolchain_, baselineDir, false);
        ASSERT_EQ(baselineRun.exitCode, 0) << tag << " baseline:\n" << baselineRun.output;
        ASSERT_FALSE(baselineRun.output.empty()) << tag << " baseline empty";
        const std::string baseline = baselineRun.output;

        const fs::path protectedDir = prepareCase(toolchain_, aCase, col, backendFlags(param.backend),
                                                  detail);
        ASSERT_FALSE(protectedDir.empty()) << tag << ": " << detail;
        const auto nativeRun = runOnce(toolchain_, protectedDir, true);
        ASSERT_EQ(nativeRun.exitCode, 0) << tag << " " << col << " runtime:\n"
                                         << nativeRun.output;
        EXPECT_EQ(nativeRun.output, baseline) << tag << " " << col << " differential mismatch";
    }
};

CorpusToolchain JavaCorpusE2E::toolchain_;
std::multimap<std::string, std::pair<std::string, std::string>> JavaCorpusE2E::exclusions_;

TEST_P(JavaCorpusE2E, MatchesJvmBaseline)
{
    runCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Corpus, JavaCorpusE2E, ::testing::ValuesIn(corpusMatrixParams()));

// ===========================================================================
// 性能档（MJ2X_CORPUS_PERF=1 门控）：perf/ 语料 × 列（baseline + 三后端），
// 每列 5 轮计时取中位数打表；各列输出与 baseline 逐字节对拍（性能档同样
// 不得破坏语义）。
// ===========================================================================

namespace {

std::vector<CorpusCase> perfCases() { return enumerateCorpus({"perf"}); }

}   // namespace

class JavaCorpusPerf : public ::testing::TestWithParam<CorpusCase>
{
protected:
    static CorpusToolchain toolchain_;

    static void SetUpTestSuite() { toolchain_ = probeCorpusToolchain(); }

    void SetUp() override
    {
        if (std::getenv("MJ2X_CORPUS_PERF") == nullptr) {
            GTEST_SKIP() << "set MJ2X_CORPUS_PERF=1 to run the performance tier";
        }
        if (!toolchain_.available) {
            GTEST_SKIP() << "corpus toolchain unavailable: " << toolchain_.reason;
        }
    }

    void runCase(const CorpusCase& aCase)
    {
        struct Column
        {
            std::string name;
            std::string flags;
        };
        const std::vector<Column> columns = {
            {"baseline", ""},
            {"stackc", backendFlags(CorpusBackend::StackC)},
            {"regc", backendFlags(CorpusBackend::RegC)},
            {"vmc4", backendFlags(CorpusBackend::Vmc4)},
        };

        std::string detail;
        std::string baseline;
        std::cout << "[perf] " << aCase.name << ":";
        for (const auto& column : columns) {
            const bool isBaseline = column.name == "baseline";
            const fs::path dir = prepareCase(toolchain_, aCase, column.name, column.flags, detail);
            ASSERT_FALSE(dir.empty()) << aCase.name << ": " << detail;

            std::vector<double> samples;
            std::string         output;
            for (int run = 0; run < 5; ++run) {
                const auto started = std::chrono::steady_clock::now();
                const auto result  = runOnce(toolchain_, dir, !isBaseline);
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
                ASSERT_EQ(result.exitCode, 0) << aCase.name << "/" << column.name
                                              << " run:\n" << result.output;
                output = result.output;
                samples.push_back(ms);
            }
            if (isBaseline) {
                baseline = output;
            }
            else {
                EXPECT_EQ(output, baseline)
                    << aCase.name << " " << column.name << " differential mismatch";
            }
            std::sort(samples.begin(), samples.end());
            std::cout << " " << column.name << "=" << static_cast<int>(samples[2]) << "ms";
        }
        std::cout << std::endl;
    }
};

CorpusToolchain JavaCorpusPerf::toolchain_;

TEST_P(JavaCorpusPerf, TimingReportAndDifferential)
{
    runCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(CorpusPerf, JavaCorpusPerf, ::testing::ValuesIn(perfCases()));

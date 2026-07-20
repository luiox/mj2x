// mtest_set_native_matrix_test.cpp - mtest_set -> StackC/RegC -> native -> JVM matrix.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "manifest/semantic_fixture_manifest.hpp"
#include "test_helper.hpp"

namespace {

namespace fs = std::filesystem;
using namespace test::jdk;

enum class MatrixBackend
{
    StackC,
    RegC,
};

struct MatrixParam
{
    const mtest_set::SemanticFixtureCase* fixture = nullptr;
    MatrixBackend                         backend = MatrixBackend::StackC;
};

const char* backendName(MatrixBackend backend)
{
    return backend == MatrixBackend::StackC ? "stackc" : "regc";
}

void PrintTo(const MatrixParam& param, std::ostream* out)
{
    *out << "{" << param.fixture->id << "," << backendName(param.backend) << "}";
}

std::string normalizeOutput(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

fs::path mainClassPath(const fs::path& root, const char* mainClass)
{
    std::string relative(mainClass);
    std::replace(relative.begin(), relative.end(), '.', '/');
    return root / (relative + ".class");
}

std::vector<MatrixParam> matrixParams()
{
    std::vector<MatrixParam> params;
    for (const auto& fixture : mtest_set::semanticFixtureManifest()) {
        params.push_back(MatrixParam{&fixture, MatrixBackend::StackC});
        params.push_back(MatrixParam{&fixture, MatrixBackend::RegC});
    }
    return params;
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
    Toolchain toolchain;
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

class MtestSetNativeMatrix : public ::testing::TestWithParam<MatrixParam>
{
protected:
    static Toolchain toolchain_;

    static void SetUpTestSuite() { toolchain_ = probeToolchain(); }

    void SetUp() override
    {
        if (!toolchain_.available) {
            GTEST_SKIP() << "native integration toolchain unavailable: " << toolchain_.reason;
        }
    }

    void runCase(const MatrixParam& param)
    {
        const auto& fixture = *param.fixture;
        const fs::path dir =
            test::getOutPath("mj2x/integration/" + std::string(backendName(param.backend)) + "/" +
                             fixture.id + "/.keep")
                .parent_path();
        std::error_code ec;
        fs::remove_all(dir, ec);
        ASSERT_FALSE(ec) << ec.message();
        fs::create_directories(dir, ec);
        ASSERT_FALSE(ec) << ec.message();

        ca::str::Utf8StringArena arena;
        const auto                bundle = fixture.generator(arena);
        ASSERT_FALSE(bundle.artifacts.empty()) << fixture.id;
        for (const auto& artifact : bundle.artifacts) {
            const fs::path path = dir / artifact.relativePath;
            fs::create_directories(path.parent_path(), ec);
            ASSERT_FALSE(ec) << ec.message();
            std::ofstream out(path, std::ios::binary);
            ASSERT_TRUE(out) << path.string();
            out.write(reinterpret_cast<const char*>(artifact.bytes.data()),
                      static_cast<std::streamsize>(artifact.bytes.size()));
            ASSERT_TRUE(out.good()) << path.string();
        }

        const fs::path driver = dir / "Driver.java";
        {
            std::ofstream out(driver);
            ASSERT_TRUE(out) << driver.string();
            out << "public class Driver {\n"
                   "  public static void main(String[] args) throws Exception {\n"
                   "    String dll = System.getenv(\"MORPHER_DLL\");\n"
                   "    if (dll != null) System.load(dll);\n"
                   "    "
                << fixture.runnerBody << "\n"
                   "  }\n"
                   "}\n";
        }
        const auto compileDriver =
            runBat({q(toolchain_.jdk.javac) + " -encoding UTF-8 -cp " + q(dir) + " -d " +
                    q(dir) + " " + q(driver)});
        ASSERT_EQ(compileDriver.exitCode, 0) << fixture.id << " javac:\n" << compileDriver.out;

        const auto baselineRun =
            runBat({q(toolchain_.jdk.java) + " -Xverify:all -cp " + q(dir) + " Driver"});
        ASSERT_EQ(baselineRun.exitCode, 0) << fixture.id << " baseline:\n" << baselineRun.out;
        const std::string baseline = normalizeOutput(baselineRun.out);
        ASSERT_EQ(baseline, std::string(fixture.expectedStdout) + "\n") << fixture.id;

        const fs::path inputClass = mainClassPath(dir, fixture.mainClass);
        ASSERT_TRUE(fileExists(inputClass)) << inputClass.string();
        const fs::path outputClass =
            inputClass.parent_path() / (inputClass.stem().string() + "_protected.class");
        const fs::path cFile = dir / (std::string(backendName(param.backend)) + ".c");
        const auto transform = runBat({q(toolchain_.cli) + " -i " + q(inputClass) + " -o " +
                                       q(outputClass) + " --backend " + backendName(param.backend) +
                                       " --emit-c " + q(cFile) + " --compile"});
        ASSERT_EQ(transform.exitCode, 0)
            << fixture.id << " " << backendName(param.backend) << " transform:\n"
            << transform.out;
        ASSERT_TRUE(fileExists(outputClass)) << outputClass.string();
        ASSERT_TRUE(fileExists(cFile)) << cFile.string();

        const fs::path nativeLibrary = outputClass.parent_path() /
#ifdef _WIN32
                                       (outputClass.stem().string() + ".dll");
#elif defined(__APPLE__)
                                       (outputClass.stem().string() + ".dylib");
#else
                                       (outputClass.stem().string() + ".so");
#endif
        ASSERT_TRUE(fileExists(nativeLibrary)) << nativeLibrary.string();
        fs::copy_file(outputClass, inputClass, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

#ifdef _WIN32
        const std::string setLibrary = "set \"MORPHER_DLL=" + nativeLibrary.string() + "\"";
#else
        const std::string setLibrary = "MORPHER_DLL=" + q(nativeLibrary) + "; export MORPHER_DLL";
#endif
        const auto nativeRun = runBat({setLibrary,
                                       q(toolchain_.jdk.java) + " -Xverify:all -cp " + q(dir) +
                                           " Driver"});
        ASSERT_EQ(nativeRun.exitCode, 0)
            << fixture.id << " " << backendName(param.backend) << " runtime:\n"
            << nativeRun.out;
        EXPECT_EQ(normalizeOutput(nativeRun.out), baseline)
            << fixture.id << " " << backendName(param.backend);
    }
};

Toolchain MtestSetNativeMatrix::toolchain_;

TEST_P(MtestSetNativeMatrix, MatchesJvmBaseline)
{
    runCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(SemanticFixtures,
                         MtestSetNativeMatrix,
                         ::testing::ValuesIn(matrixParams()));

} // namespace


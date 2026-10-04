// mtest_set_native_matrix_test.cpp - mtest_set -> StackC/RegC -> native -> JVM matrix.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    Llvm,
};

struct MatrixParam
{
    const mtest_set::SemanticFixtureCase* fixture = nullptr;
    MatrixBackend                         backend = MatrixBackend::StackC;
};

const char* backendName(MatrixBackend backend)
{
    switch (backend) {
    case MatrixBackend::StackC: return "stackc";
    case MatrixBackend::RegC: return "regc";
    case MatrixBackend::Llvm: return "llvm";
    }
    return "stackc";
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

/// LLVM 列允许清单：LLVM 后端 fail-closed，仅覆盖其子集能力（算术/数组/字段/
/// 调用/异常/监视器/对象/反射/字符串/集合）。新增语义 fixture 若 LLVM 支持应
/// 显式加入（不支持的在清单外自动跳过）。
bool llvmSupports(const char* fixtureId)
{
    static const std::vector<const char*> supported = {
        "g1.const_ints",    "g1.ldc_string",     "g2.add",     "g5.abs",
        "g8.sum_array",     "g3.integral_narrowing", "g3.numeric_widening",
        "g3.numeric_narrowing", "g6.static_special", "g6.virtual_interface",
        "g6.recursion",     "g7.static_fields",  "g7.instance_fields",
        "g9.object_lifecycle", "g9.type_checks", "g10.throw_catch",
        "g10.monitor",      "g11.reflection_access", "g12.string_ops",
        "g12.string_compare", "g13.bit_ops",     "g13.shift_out_of_range",
        "g14.deep_recursion", "g15.collection_ops",
        // ---- G17（#1117）：LLVM 支持面入列；g17.string_intern 暂缓——LLVM ldc
        // String 物化（NewStringUTF）尚未走 intern（#1117 修复仅覆盖 RegC
        // literal intrinsic 与 StackC 池物化），恒等断言会失败。
        "g17.math_nan_zero", "g17.array_store_check", "g17.sibling_methods",
        "g17.annotation_visibility",
    };
    for (const char* id : supported) {
        if (std::strcmp(id, fixtureId) == 0) return true;
    }
    return false;
}

std::vector<MatrixParam> matrixParams()
{
    std::vector<MatrixParam> params;
    for (const auto& fixture : mtest_set::semanticFixtureManifest()) {
        params.push_back(MatrixParam{&fixture, MatrixBackend::StackC});
        params.push_back(MatrixParam{&fixture, MatrixBackend::RegC});
        if (llvmSupports(fixture.id)) {
            params.push_back(MatrixParam{&fixture, MatrixBackend::Llvm});
        }
    }
    return params;
}

struct Toolchain
{
    bool         available = false;
    std::string  reason;
    JdkToolchain jdk;
    fs::path     cli;
    ClangProbe   clang;   // LLVM 列需要 clang（非 cl/gcc）；探测面 PATH+常见位置（#1122）
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
    toolchain.clang = probeClang();
    toolchain.available = true;
    return toolchain;
}

class MtestSetNativeMatrix : public ::testing::TestWithParam<MatrixParam>
{
protected:
    static Toolchain toolchain_;

    static void SetUpTestSuite()
    {
        toolchain_ = probeToolchain();
        // #1122：探测结果可见化——找到/未找到 + 版本 + 来源，一行打测试输出，
        // 使 llvm 列的 SKIP 有因可查（此前无 clang 机器整列静默跳过）。
        std::cout << "[clang-probe] MtestSetNativeMatrix: " << formatClangProbe(toolchain_.clang)
                  << std::endl;
    }

    void SetUp() override
    {
        if (!toolchain_.available) {
            GTEST_SKIP() << "native integration toolchain unavailable: " << toolchain_.reason;
        }
        if (GetParam().backend == MatrixBackend::Llvm && !toolchain_.clang.available) {
            GTEST_SKIP() << "clang not found (probed PATH + common install locations): "
                         << toolchain_.clang.reason << " (LLVM matrix column)";
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
        std::string transformCommand;
        if (param.backend == MatrixBackend::Llvm) {
            // LLVM：pipe 编译（.ll 内存态 → clang -x ir -）；无需 --emit-c。
            transformCommand = q(toolchain_.cli) + " -i " + q(inputClass) + " -o " +
                               q(outputClass) + " --backend llvm --compile --compile-mode pipe "
                               "--llvm-tool clang";
        }
        else {
            const fs::path cFile = dir / (std::string(backendName(param.backend)) + ".c");
            transformCommand = q(toolchain_.cli) + " -i " + q(inputClass) + " -o " +
                               q(outputClass) + " --backend " + backendName(param.backend) +
                               " --emit-c " + q(cFile) + " --compile";
        }
        const auto transform = runBat(clangPathPrelude(toolchain_.clang, transformCommand));
        ASSERT_EQ(transform.exitCode, 0)
            << fixture.id << " " << backendName(param.backend) << " transform:\n"
            << transform.out;
        ASSERT_TRUE(fileExists(outputClass)) << outputClass.string();

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


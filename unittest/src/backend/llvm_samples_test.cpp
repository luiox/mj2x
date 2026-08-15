// LLVM IR protected-sample fixtures: javac -> mj2x-cli (llvm backend) -> clang
// -> JVM native run, mirroring the RegC sample contract for mjdbg.
//
// The committed Java sources live under test_resource/protected_samples/llvm.
// Generated .class + native.ll + dll artifacts go under test/mj2x/protected_samples/llvm.

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jdk_fixture.hpp"
#include "test_helper.hpp"

namespace {

namespace fs = std::filesystem;

struct ExecResult
{
    int         exitCode = -1;
    std::string out;
};

struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    cli;
    bool        clangAvailable = false;
};

struct SampleSpec
{
    const char*             className;
    const char*             sourceResource;
    const char*             traceFocus;
    std::vector<const char*> requiredMarkers;   // native.ll 内必须出现的片段
};

std::string quotePath(const fs::path& path)
{
    return "\"" + path.string() + "\"";
}

bool fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string readTextFile(const fs::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

size_t countJavaSources(const fs::path& dir)
{
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".java") {
            ++count;
        }
    }
    return count;
}

ExecResult runScript(const std::vector<std::string>& lines)
{
    ExecResult      result;
    fs::path        tmpDir = test::getTempOutPath("mj2x_llvm_sample.keep").parent_path();
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    fs::path script =
        tmpDir / ("mj2x_llvm_sample_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
#ifdef _WIN32
                  ".bat");
#else
                  ".sh");
#endif
    {
        std::ofstream out(script);
#ifdef _WIN32
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
#else
        out << "#!/bin/sh\n";
#endif
        for (const auto& line : lines) out << line << "\n";
#ifdef _WIN32
        out << "exit /b %errorlevel%\n";
#endif
    }

#ifdef _WIN32
    std::string cmd = quotePath(script) + " 2>&1";
#else
    std::string cmd = "sh " + quotePath(script) + " 2>&1";
#endif
    FILE* pipe =
#ifdef _WIN32
        _popen(cmd.c_str(), "rb");
#else
        popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        result.out = "failed to run script: " + script.string();
        return result;
    }
    char buffer[4096];
    while (size_t n = fread(buffer, 1, sizeof(buffer), pipe)) {
        result.out.append(buffer, n);
    }
#ifdef _WIN32
    result.exitCode = _pclose(pipe);
#else
    result.exitCode = pclose(pipe);
#endif
    fs::remove(script, ec);
    return result;
}

Toolchain probeToolchain()
{
    Toolchain tc;
    const auto jdk = test::jdk::probeJdk();
    tc.javac = jdk.javac;
    tc.java  = jdk.java;
    if (tc.javac.empty() || tc.java.empty()) {
        tc.reason = "JDK not found; set JAVA_HOME";
        return tc;
    }
    tc.cli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.cli)) {
        tc.reason = "mj2x-cli 未构建: " + tc.cli.string();
        return tc;
    }
    const auto clangProbe = runScript({quotePath("clang") + " --version"});
    tc.clangAvailable     = clangProbe.exitCode == 0;
    tc.available          = true;
    return tc;
}

void writeTraceNote(const fs::path& outDir, const SampleSpec& sample)
{
    std::ofstream out(outDir / "TRACE_SURFACE.md");
    out << "# " << sample.className << " LLVM IR Trace Surface\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "Focus: " << sample.traceFocus << "\n\n";
    out << "Expected mjdbg use:\n\n";
    out << "- Compare this LLVM textual IR with StackC stack-shaped C and RegC\n";
    out << "  virtual-register helper output.\n";
    out << "- Confirm `define internal ... @mj2x_llvm_Java_<Class>_<method>__<args>`\n";
    out << "  helpers plus `define dllexport ... @Java_<Class>_<method>__<args>`\n";
    out << "  JNI wrappers, and `mj2x_llvm_*` JNI dispatch helpers.\n";
}

void writeMetricsNote(const fs::path& outDir, const SampleSpec& sample, const std::string& ll)
{
    std::ofstream out(outDir / "LLVM_METRICS.md");
    out << "# " << sample.className << " LLVM IR Metrics\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "| Metric | Value |\n";
    out << "|---|---:|\n";
    out << "| native.ll bytes | " << ll.size() << " |\n";
    out << "| `mj2x_llvm_call_` symbol occurrences | "
        << (ll.find("mj2x_llvm_call_") == std::string::npos ? 0 : 1) << " (present) |\n";
    out << "| `mj2x_llvm_throw_` symbol occurrences | "
        << (ll.find("mj2x_llvm_throw_") == std::string::npos ? 0 : 1) << " (present) |\n";
}

class LlvmSamples : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "LLVM sample toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path sampleDir(const std::string& className) const
    {
        return test::getOutPath("mj2x/protected_samples/llvm/" + className + "/.keep")
            .parent_path();
    }

    // javac -> cli(llvm, emit native.ll + compile with clang) -> assert IR markers
    // -> Driver native run (expected output). clang 缺失时仅做发射断言并跳过运行。
    void generateAndRunSample(const SampleSpec& sample, const std::string& expectedOutput)
    {
        const fs::path outDir = sampleDir(sample.className);
        std::error_code ec;
        fs::remove_all(outDir, ec);
        fs::create_directories(outDir, ec);
        ASSERT_FALSE(ec) << ec.message();

        const fs::path sourcePath = test::getResourcePath(sample.sourceResource);
        ASSERT_TRUE(fileExists(sourcePath)) << sourcePath;
        const fs::path copiedSource = outDir / (std::string(sample.className) + ".java");
        fs::copy_file(sourcePath, copiedSource, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        const auto javac =
            runScript({quotePath(tc_.javac) + " -g:none -d " + quotePath(outDir) + " " +
                       quotePath(copiedSource)});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        const fs::path classFile =
            outDir / "protected_samples" / "llvm" / (std::string(sample.className) + ".class");
        ASSERT_TRUE(fileExists(classFile)) << classFile;

        const fs::path protectedClass = outDir / (std::string(sample.className) + "_protected.class");
        const fs::path llFile         = outDir / "native.ll";
        std::string    transformCommand =
            quotePath(tc_.cli) + " -i " + quotePath(classFile) + " -o " +
            quotePath(protectedClass) + " --backend llvm --emit-llvm " + quotePath(llFile) +
            " --compile --llvm-tool clang";
        if (!tc_.clangAvailable) {
            // 无 clang：退化为仅发射（无 --compile，避免 cl 路径差异）。
            transformCommand =
                quotePath(tc_.cli) + " -i " + quotePath(classFile) + " -o " +
                quotePath(protectedClass) + " --backend llvm --emit-llvm " + quotePath(llFile);
        }
        const auto transform = runScript({transformCommand});
        ASSERT_EQ(transform.exitCode, 0) << sample.className << " transform:\n" << transform.out;
        ASSERT_TRUE(fileExists(llFile)) << llFile;

        const std::string ll = readTextFile(llFile);
        for (const char* marker : sample.requiredMarkers) {
            EXPECT_NE(ll.find(marker), std::string::npos)
                << sample.className << " missing marker: " << marker << "\n"
                << ll;
        }

        writeTraceNote(outDir, sample);
        writeMetricsNote(outDir, sample, ll);
        EXPECT_TRUE(fileExists(outDir / "TRACE_SURFACE.md"));
        EXPECT_TRUE(fileExists(outDir / "LLVM_METRICS.md"));

        if (!tc_.clangAvailable) {
            GTEST_SKIP() << "clang is not available on PATH (LLVM sample native run)";
        }

        // Driver（字节码侧）加载 dll 后调用样本 check()（native）。
        const fs::path driver = outDir / "Driver.java";
        {
            std::ofstream out(driver);
            ASSERT_TRUE(out) << driver.string();
            out << "public class Driver {\n"
                   "  public static void main(String[] args) throws Exception {\n"
                   "    String dll = System.getenv(\"MORPHER_DLL\");\n"
                   "    if (dll != null) System.load(dll);\n"
                   "    System.out.println(protected_samples.llvm."
                << sample.className << ".check());\n"
                   "  }\n"
                   "}\n";
        }
        const auto compileDriver =
            runScript({quotePath(tc_.javac) + " -encoding UTF-8 -cp " + quotePath(outDir) +
                       " -d " + quotePath(outDir) + " " + quotePath(driver)});
        ASSERT_EQ(compileDriver.exitCode, 0) << sample.className << " Driver javac:\n"
                                             << compileDriver.out;

        const fs::path nativeLibrary = outDir / (std::string(sample.className) + "_protected.dll");
#ifdef _WIN32
        const std::string setLibrary = "set \"MORPHER_DLL=" + nativeLibrary.string() + "\"";
#else
        const std::string setLibrary =
            "MORPHER_DLL=" + quotePath(nativeLibrary) + "; export MORPHER_DLL";
#endif
        fs::copy_file(protectedClass, classFile, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        const auto nativeRun =
            runScript({setLibrary, quotePath(tc_.java) + " -Xverify:all -cp " + quotePath(outDir) +
                                        " Driver"});
        ASSERT_EQ(nativeRun.exitCode, 0) << sample.className << " native run:\n"
                                         << nativeRun.out;
        std::string normalized = nativeRun.out;
        while (!normalized.empty() && (normalized.back() == '\n' || normalized.back() == '\r')) {
            normalized.pop_back();
        }
        EXPECT_EQ(normalized, expectedOutput) << sample.className;
    }
};

Toolchain LlvmSamples::tc_;

}   // namespace

TEST_F(LlvmSamples, GenerateMjdbgFixtures)
{
    const std::vector<SampleSpec> samples = {
        {
            "LlvmTinySample",
            "protected_samples/llvm/LlvmTinySample.java",
            "register-style arithmetic, loops, branches, and LLVM value slots",
            {
                "define internal i32 @mj2x_llvm_Java_protected_1samples_llvm_LlvmTinySample_addMul__II",
                "define internal i32 @mj2x_llvm_Java_protected_1samples_llvm_LlvmTinySample_loopSum__I",
                "define internal i32 @mj2x_llvm_Java_protected_1samples_llvm_LlvmTinySample_maxOf__II",
                "define internal i32 @mj2x_llvm_Java_protected_1samples_llvm_LlvmTinySample_pick__I",
                // Math intrinsic 直调（mj2x-crt runtime 符号 + 模块级 declare）。
                "call i32 @mj2x_math_abs_i32",
                "declare i32 @mj2x_math_abs_i32(i32)",
            },
        },
        {
            "LlvmIndySurfaceSample",
            "protected_samples/llvm/LlvmIndySurfaceSample.java",
            "string-concat indy expansion, class literals, and JNI static calls",
            {
                "define internal ptr @mj2x_llvm_Java_protected_1samples_llvm_LlvmIndySurfaceSample_concat__L",
                "define internal ptr @mj2x_llvm_Java_protected_1samples_llvm_LlvmIndySurfaceSample_check",
                "mj2x_llvm_class_const",
                "mj2x_llvm_string_const",
            },
        },
        {
            "LlvmExceptionMonitorSample",
            "protected_samples/llvm/LlvmExceptionMonitorSample.java",
            "exception-region dispatch, handler matching, and monitor helpers",
            {
                "define internal i32 @mj2x_llvm_Java_protected_1samples_llvm_LlvmExceptionMonitorSample_catchOwn__I",
                "mj2x_llvm_throw_arith",
                "mj2x_llvm_monitor_enter",
                "mj2x_llvm_monitor_exit",
            },
        },
    };
    ASSERT_EQ(countJavaSources(test::getResourcePath("protected_samples/llvm")), samples.size())
        << "protected_samples/llvm 样本数量与测试清单不一致";

    for (const auto& sample : samples) {
        generateAndRunSample(sample,
                             sample.className == "LlvmTinySample"
                                 ? "addMul=21 loopSum=45 maxOf=9 pick=4 absDiff=8"
                                 : sample.className == "LlvmIndySurfaceSample"
                                       ? "[k]=7 slice=5 types=true"
                                       : "catch0=7 catch10=10 caught locked=10");
    }
}

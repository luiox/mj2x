// CBackend protected sample generator for mjdbg development.
//
// The committed Java sources live under test_resource/protected_samples/cbackend.
// This test compiles them and runs mj2x-cli so mjdbg can consume deterministic
// protected .class + generated .c artifacts from test/mj2x/protected_samples.

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

struct ExecResult {
    int exitCode = -1;
    std::string out;
};

struct Toolchain {
    bool available = false;
    std::string reason;
    fs::path javac;
    fs::path mj2xCli;
};

struct SampleSpec {
    const char* className;
    const char* sourceResource;
    const char* traceFocus;
};

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string q(const fs::path& path) {
    std::string s = path.string();
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
    return "\"" + s + "\"";
}

ExecResult runBat(const std::vector<std::string>& lines) {
    ExecResult result;
    fs::path tmpDir = test::getTempOutPath("mj2x_protected_sample.keep").parent_path();
    fs::path bat = tmpDir / ("mj2x_protected_sample_" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
        "_" + std::to_string(std::rand()) + ".bat");
    {
        std::ofstream out(bat);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        for (const auto& line : lines) out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }

    std::string command = q(bat) + " 2>&1";
    FILE* pipe =
#ifdef _WIN32
        _popen(command.c_str(), "rb");
#else
        popen(command.c_str(), "r");
#endif
    if (!pipe) {
        result.out = "failed to run " + bat.string();
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
    std::error_code ec;
    fs::remove(bat, ec);
    return result;
}

fs::path findJdk() {
    if (const char* home = std::getenv("JAVA_HOME"); home && *home) {
        fs::path root(home);
        if (fileExists(root / "bin" / "javac.exe") || fileExists(root / "bin" / "javac")) {
            return root;
        }
    }
    fs::path fallback = "C:/Users/Canrad/.jdks/corretto-17.0.5";
    if (fileExists(fallback / "bin" / "javac.exe")) return fallback;
    return {};
}

Toolchain probeToolchain() {
    Toolchain tc;
    fs::path jdk = findJdk();
    if (jdk.empty()) {
        tc.reason = "JAVA_HOME not set and fallback JDK not found";
        return tc;
    }
#ifdef _WIN32
    tc.javac = jdk / "bin" / "javac.exe";
    tc.mj2xCli = test::getTopProjectPath() / "build" / "windows" / "x64" / "release" / "mj2x-cli.exe";
#else
    tc.javac = jdk / "bin" / "javac";
    tc.mj2xCli = test::getTopProjectPath() / "build" / "linux" / "x86_64" / "release" / "mj2x-cli";
#endif
    if (!fileExists(tc.javac)) {
        tc.reason = "javac not found: " + tc.javac.string();
        return tc;
    }
    if (!fileExists(tc.mj2xCli)) {
        tc.reason = "mj2x-cli not built: " + tc.mj2xCli.string();
        return tc;
    }
    tc.available = true;
    return tc;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeTraceNote(const fs::path& outDir, const SampleSpec& sample) {
    std::ofstream out(outDir / "TRACE_SURFACE.md");
    out << "# " << sample.className << " Trace Surface\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "Focus: " << sample.traceFocus << "\n\n";
    out << "Expected mjdbg use:\n\n";
    out << "- Load or attach to a JVM that runs the protected class.\n";
    out << "- Confirm the selected methods are native after CBackend processing.\n";
    out << "- Observe the `<clinit>` call into `mj2x-cloader` and the native `RegisterNatives` dispatch table; protected method bodies remain `static __mj2x_c_*` functions rather than per-method or per-class `Java_*` exports.\n";
    out << "- Capture Java MethodEntry/MethodExit around the public entry methods.\n";
    out << "- Capture native/JNI activity inside the generated CBackend implementation.\n";
    out << "- Use observations to draft weakness reports for mj2x hardening.\n";
}

class CBackendProtectedSamples : public ::testing::Test {
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() {
        tc_ = probeToolchain();
    }

    void SetUp() override {
        if (!tc_.available) {
            GTEST_SKIP() << "protected sample toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path sampleDir(const std::string& className) const {
        return test::getOutPath("mj2x/protected_samples/cbackend/" + className + "/.keep").parent_path();
    }

    void generateSample(const SampleSpec& sample) {
        fs::path outDir = sampleDir(sample.className);
        std::error_code ec;
        fs::create_directories(outDir, ec);

        fs::path src = test::getResourcePath(sample.sourceResource);
        ASSERT_TRUE(fileExists(src)) << src;

        fs::path copiedSource = outDir / (std::string(sample.className) + ".java");
        fs::copy_file(src, copiedSource, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        auto javac = runBat({q(tc_.javac) + " -g -d " + q(outDir) + " " + q(copiedSource)});
        ASSERT_EQ(javac.exitCode, 0) << "javac failed:\n" << javac.out;

        fs::path originalClass = outDir / "protected_samples" / "cbackend" /
                                 (std::string(sample.className) + ".class");
        ASSERT_TRUE(fileExists(originalClass)) << originalClass;

        fs::path protectedClass = outDir / (std::string(sample.className) + "_protected.class");
        fs::path cFile = outDir / (std::string(sample.className) + ".c");
        auto mj2x = runBat({
            q(tc_.mj2xCli) + " -i " + q(originalClass) + " -o " + q(protectedClass)
                + " --emit-c " + q(cFile),
        });
        ASSERT_EQ(mj2x.exitCode, 0) << "mj2x-cli failed:\n" << mj2x.out;
        ASSERT_TRUE(fileExists(protectedClass)) << protectedClass;
        ASSERT_TRUE(fileExists(cFile)) << cFile;

        std::string generatedC = readTextFile(cFile);
        EXPECT_NE(generatedC.find("JNIEXPORT jint JNICALL JNI_OnLoad"), std::string::npos);
        EXPECT_NE(generatedC.find("RegisterNatives"), std::string::npos) << generatedC;
        EXPECT_NE(generatedC.find("registerNativesForClass"), std::string::npos)
            << generatedC;
        EXPECT_EQ(generatedC.find("_1_1mj2x_1registerClass__I"), std::string::npos)
            << generatedC;
        EXPECT_NE(generatedC.find("__mj2x_c_"), std::string::npos) << generatedC;
        EXPECT_EQ(generatedC.find(std::string("Java_protected_1samples_cbackend_") +
                      sample.className + "_arithmetic"),
                  std::string::npos) << generatedC;
        EXPECT_EQ(generatedC.find(std::string("Java_protected_1samples_cbackend_") +
                      sample.className + "_throwCatch"),
                  std::string::npos) << generatedC;

        writeTraceNote(outDir, sample);
        ASSERT_TRUE(fileExists(outDir / "TRACE_SURFACE.md"));
    }
};

Toolchain CBackendProtectedSamples::tc_;

} // namespace

TEST_F(CBackendProtectedSamples, GenerateMjdbgFixtures) {
    const std::vector<SampleSpec> samples = {
        {
            "CBackendTinySample",
            "protected_samples/cbackend/CBackendTinySample.java",
            "integer arithmetic, branches, loops, switch, long/double conversion",
        },
        {
            "CBackendObjectArraySample",
            "protected_samples/cbackend/CBackendObjectArraySample.java",
            "object allocation, constructor calls, fields, arrays, type checks, String LDC",
        },
        {
            "CBackendExceptionMonitorSample",
            "protected_samples/cbackend/CBackendExceptionMonitorSample.java",
            "exception creation/catch paths and monitor enter/exit",
        },
    };

    for (const auto& sample : samples) {
        generateSample(sample);
    }
}

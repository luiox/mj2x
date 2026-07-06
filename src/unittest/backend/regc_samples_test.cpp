#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "test_helper.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

namespace fs = std::filesystem;

struct ExecResult {
    int exitCode = -1;
    std::string out;
};

struct Toolchain {
    bool available = false;
    std::string reason;
    fs::path javac;
};

struct MethodSpec {
    const char* name;
    const char* desc;
    const char* requiredText;
};

struct SampleSpec {
    const char* className;
    const char* sourceResource;
    const char* traceFocus;
    std::vector<MethodSpec> methods;
};

std::string toStr(ca::str::Utf8StringRef ref) {
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string quotePath(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

ExecResult runScript(const std::vector<std::string>& lines) {
    ExecResult result;
    fs::path tmpDir = test::getTempOutPath("mj2x_regc_sample.keep").parent_path();
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    fs::path script = tmpDir / ("mj2x_regc_sample_" +
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

fs::path findJavac() {
    if (const char* home = std::getenv("JAVA_HOME"); home && *home) {
        fs::path root(home);
#ifdef _WIN32
        fs::path javac = root / "bin" / "javac.exe";
#else
        fs::path javac = root / "bin" / "javac";
#endif
        if (fileExists(javac)) return javac;
    }

    fs::path fallback = "C:/Users/Canrad/.jdks/corretto-17.0.5";
    fs::path fallbackJavac = fallback / "bin" / "javac.exe";
    if (fileExists(fallbackJavac)) return fallbackJavac;

    return {};
}

Toolchain probeToolchain() {
    Toolchain tc;
    tc.javac = findJavac();
    if (tc.javac.empty()) {
        tc.reason = "javac not found; set JAVA_HOME";
        return tc;
    }
    tc.available = true;
    return tc;
}

std::unique_ptr<ClassNode> loadClassFile(ca::str::Utf8StringArena& arena,
                                         const fs::path& classFile,
                                         std::vector<ca::u8>& bytesOut) {
    std::ifstream in(classFile, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) return nullptr;
    bytesOut.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytesOut.data()), static_cast<std::streamsize>(bytesOut.size()));

    auto cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytesOut.data(), static_cast<ca::i32>(bytesOut.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

MethodNode* findMethod(ClassNode* cn, const MethodSpec& method) {
    for (auto* m : cn->methods) {
        if (toStr(m->name) == method.name && toStr(m->desc) == method.desc) return m;
    }
    return nullptr;
}

std::string emitRegCForMethod(ClassNode* cn, MethodNode* method, std::string& reason) {
    CodeConverter converter;
    auto blocks = converter.convert(*method);

    SSABuilder builder;
    bool isStatic = (method->access & Opcodes::ACC_STATIC) != 0;
    auto ssa = builder.build(blocks, toStr(cn->name), toStr(method->name), toStr(method->desc),
                             isStatic, method->maxLocals, reason);
    if (!ssa) return {};

    RegLowering lowering;
    auto reg = lowering.lower(*ssa, reason);
    if (!reg) return {};

    RegCEmitter emitter;
    auto emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        reason = emitted.reason;
        return {};
    }
    return emitted.source;
}

void writeTraceNote(const fs::path& outDir, const SampleSpec& sample) {
    std::ofstream out(outDir / "TRACE_SURFACE.md");
    out << "# " << sample.className << " RegC Trace Surface\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "Focus: " << sample.traceFocus << "\n\n";
    out << "Expected mjdbg use:\n\n";
    out << "- Compare this RegC helper-style C with CBackend stack-style C.\n";
    out << "- Confirm virtual-register locals such as `r0` and labels such as `block_b0`.\n";
    out << "- Confirm helper calls use `_mj2x_regc_*` instead of `stack[]` / `sp` patterns.\n";
    out << "- Use observations to guide RegC runtime/JNI tracing requirements.\n";
}

class RegCTraceSamples : public ::testing::Test {
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() {
        tc_ = probeToolchain();
    }

    void SetUp() override {
        if (!tc_.available) {
            GTEST_SKIP() << "RegC sample toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path sampleDir(const std::string& className) const {
        return test::getOutPath("mj2x/protected_samples/regc/" + className + "/.keep").parent_path();
    }

    void generateSample(const SampleSpec& sample) {
        fs::path outDir = sampleDir(sample.className);
        std::error_code ec;
        fs::create_directories(outDir, ec);

        fs::path sourcePath = test::getResourcePath(sample.sourceResource);
        ASSERT_TRUE(fileExists(sourcePath)) << sourcePath;
        fs::path copiedSource = outDir / (std::string(sample.className) + ".java");
        fs::copy_file(sourcePath, copiedSource, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        auto javac = runScript({quotePath(tc_.javac) + " -g:none -d " + quotePath(outDir) + " " + quotePath(copiedSource)});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        fs::path classFile = outDir / "protected_samples" / "regc" /
                             (std::string(sample.className) + ".class");
        ASSERT_TRUE(fileExists(classFile)) << classFile;

        fs::path protectedClass = outDir / (std::string(sample.className) + "_protected.class");
        fs::path cFile = outDir / (std::string(sample.className) + ".c");

        PipelineConfig config;
        config.inputPath = classFile.string();
        config.outputPath = protectedClass.string();
        Pipeline pipeline(std::move(config));
        auto backend = std::make_unique<RegCBackend>();
        backend->setOutputPath(cFile.string());
        pipeline.registerBackend(std::move(backend));
        pipeline.registerPreprocessor(std::make_unique<MarkAllMethodsPreprocessor>());
        ASSERT_EQ(pipeline.run(), 0);
        ASSERT_TRUE(fileExists(cFile));
        ASSERT_TRUE(fileExists(protectedClass));

        std::string source = readTextFile(cFile);
        EXPECT_EQ(source.find("stack["), std::string::npos) << source;
        EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
        EXPECT_EQ(source.find("sp--"), std::string::npos) << source;
        for (const auto& method : sample.methods) {
            EXPECT_NE(source.find(method.requiredText), std::string::npos)
                << method.name << method.desc << "\n" << source;
        }

        writeTraceNote(outDir, sample);
        ASSERT_TRUE(fileExists(outDir / "TRACE_SURFACE.md"));
    }
};

Toolchain RegCTraceSamples::tc_;

} // namespace

TEST_F(RegCTraceSamples, GenerateMjdbgFixtures) {
    const std::vector<SampleSpec> samples = {
        {
            "RegCTinySample",
            "protected_samples/regc/RegCTinySample.java",
            "register locals, arithmetic, loop edge copies, and switch labels",
            {
                {"addMul", "(II)I", "uint32_t"},
                {"loopSum", "(I)I", "edge_"},
                {"choose", "(I)I", "switch"},
            },
        },
        {
            "RegCHelperSurfaceSample",
            "protected_samples/regc/RegCHelperSurfaceSample.java",
            "object allocation, fields, arrays, calls, and reference checks through RegC helpers",
            {
                {"fieldAndCall", "(I)I", "_mj2x_regc_call_i32"},
                {"arrays", "(I)I", "_mj2x_regc_store_i32_array"},
                {"referenceOps", "(Ljava/lang/Object;)I", "_mj2x_regc_instanceof"},
            },
        },
        {
            "RegCExceptionSurfaceSample",
            "protected_samples/regc/RegCExceptionSurfaceSample.java",
            "exception-table dispatch, handler object registers, and unmatched rethrow paths",
            {
                {"catchOwn", "(I)I", "_mj2x_regc_exception_matches"},
                {"missCatch", "()I", "_mj2x_regc_throw"},
                {"catchCall", "()I", "_mj2x_regc_exception_clear"},
            },
        },
    };

    for (const auto& sample : samples) {
        generateSample(sample);
    }
}

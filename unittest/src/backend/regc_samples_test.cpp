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
#include "jdk_fixture.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "test_helper.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

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
};

struct MethodSpec
{
    const char* name;
    const char* desc;
    const char* requiredText;
};

struct SampleSpec
{
    const char*             className;
    const char*             sourceResource;
    const char*             traceFocus;
    std::vector<MethodSpec> methods;
};

std::string toStr(ca::str::Utf8StringRef ref)
{
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

bool fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string quotePath(const fs::path& path)
{
    return "\"" + path.string() + "\"";
}

std::string readTextFile(const fs::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

size_t countSubstring(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
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
    fs::path        tmpDir = test::getTempOutPath("mj2x_regc_sample.keep").parent_path();
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    fs::path script =
        tmpDir / ("mj2x_regc_sample_" +
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

fs::path findJavac()
{
    // 复用共享 JDK 探测（底层 libjdk_finder），替代硬编码作者机器路径的回落。
    return test::jdk::probeJdk().javac;
}

Toolchain probeToolchain()
{
    Toolchain tc;
    tc.javac = findJavac();
    if (tc.javac.empty()) {
        tc.reason = "javac not found; set JAVA_HOME";
        return tc;
    }
    tc.available = true;
    return tc;
}

std::unique_ptr<ClassNode> loadClassFile(ca::str::Utf8StringArena& arena, const fs::path& classFile,
                                         std::vector<ca::u8>& bytesOut)
{
    std::ifstream in(classFile, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) return nullptr;
    bytesOut.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytesOut.data()),
            static_cast<std::streamsize>(bytesOut.size()));

    auto        cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytesOut.data(), static_cast<ca::i32>(bytesOut.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

MethodNode* findMethod(ClassNode* cn, const MethodSpec& method)
{
    for (auto* m : cn->methods) {
        if (toStr(m->name) == method.name && toStr(m->desc) == method.desc) return m;
    }
    return nullptr;
}

std::string emitRegCForMethod(ClassNode* cn, MethodNode* method, std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = test::convertMethodNode(converter, *method, arena);

    SSABuilder builder;
    bool       isStatic = (method->access & jbytecode::core::Opcodes::ACC_STATIC) != 0;
    auto       ssa      = builder.build(blocks,
                             toStr(cn->name),
                             toStr(method->name),
                             toStr(method->desc),
                             isStatic,
                             method->maxLocals,
                             arena,
                             reason);
    if (!ssa) return {};

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return {};

    RegCEmitter emitter;
    auto        emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        reason = emitted.reason;
        return {};
    }
    return emitted.source;
}

void writeTraceNote(const fs::path& outDir, const SampleSpec& sample)
{
    std::ofstream out(outDir / "TRACE_SURFACE.md");
    out << "# " << sample.className << " RegC Trace Surface\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "Focus: " << sample.traceFocus << "\n\n";
    out << "Expected mjdbg use:\n\n";
    out << "- Compare this RegC helper-style C with StackC stack-style C.\n";
    out << "- Confirm virtual-register locals such as `r0` and labels such as `block_b0`.\n";
    out << "- Confirm helper calls use RegC/R3 symbols such as `mj2x_regc_div_i32`\n";
    out << "  or transitional `_mj2x_regc_*` helpers instead of `stack[]` / `sp` patterns.\n";
    out << "- Use observations to guide RegC runtime/JNI tracing requirements.\n";
}

void writeMetricsNote(const fs::path& outDir, const SampleSpec& sample, const std::string& source)
{
    std::ofstream out(outDir / "REGC_METRICS.md");
    out << "# " << sample.className << " RegC Metrics\n\n";
    out << "Generated from `mj2x/test_resource/" << sample.sourceResource << "`.\n\n";
    out << "| Metric | Value |\n";
    out << "|---|---:|\n";
    out << "| generated C bytes | " << source.size() << " |\n";
    out << "| `_mj2x_regc_call_` symbol occurrences | "
        << countSubstring(source, "_mj2x_regc_call_") << " |\n";
    out << "| TU class cache declarations | "
        << countSubstring(source, "static mj2x_regc_class_cache ") << " |\n";
    out << "| TU static method cache declarations | "
        << countSubstring(source, "static mj2x_regc_static_method_cache ") << " |\n";
    out << "| TU instance method cache declarations | "
        << countSubstring(source, "static mj2x_regc_instance_method_cache ") << " |\n";
    out << "| TU static field cache declarations | "
        << countSubstring(source, "static mj2x_regc_static_field_cache ") << " |\n";
    out << "| TU instance field cache declarations | "
        << countSubstring(source, "static mj2x_regc_instance_field_cache ") << " |\n";
}

class RegCTraceSamples : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "RegC sample toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path sampleDir(const std::string& className) const
    {
        return test::getOutPath("mj2x/protected_samples/regc/" + className + "/.keep")
            .parent_path();
    }

    void generateSample(const SampleSpec& sample)
    {
        fs::path        outDir = sampleDir(sample.className);
        std::error_code ec;
        fs::create_directories(outDir, ec);

        fs::path sourcePath = test::getResourcePath(sample.sourceResource);
        ASSERT_TRUE(fileExists(sourcePath)) << sourcePath;
        fs::path copiedSource = outDir / (std::string(sample.className) + ".java");
        fs::copy_file(sourcePath, copiedSource, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        auto javac = runScript({quotePath(tc_.javac) + " -g:none -d " + quotePath(outDir) + " " +
                                quotePath(copiedSource)});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        fs::path classFile =
            outDir / "protected_samples" / "regc" / (std::string(sample.className) + ".class");
        ASSERT_TRUE(fileExists(classFile)) << classFile;

        fs::path protectedClass = outDir / (std::string(sample.className) + "_protected.class");
        fs::path cFile          = outDir / (std::string(sample.className) + ".c");

        PipelineConfig config;
        config.inputPath  = classFile.string();
        config.outputPath = protectedClass.string();
        Pipeline pipeline(std::move(config));
        auto     backend = std::make_unique<RegCBackend>();
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
                << method.name << method.desc << "\n"
                << source;
        }

        writeTraceNote(outDir, sample);
        writeMetricsNote(outDir, sample, source);
        fs::path traceNote = outDir / "TRACE_SURFACE.md";
        ASSERT_TRUE(fileExists(traceNote));
        std::string traceSurface = readTextFile(traceNote);
        EXPECT_NE(traceSurface.find(sample.className), std::string::npos);
        EXPECT_NE(traceSurface.find(sample.traceFocus), std::string::npos);

        fs::path metricsNote = outDir / "REGC_METRICS.md";
        ASSERT_TRUE(fileExists(metricsNote));
        std::string metrics = readTextFile(metricsNote);
        EXPECT_NE(metrics.find("generated C bytes"), std::string::npos);
        EXPECT_NE(metrics.find("TU class cache declarations"), std::string::npos);
    }
};

Toolchain RegCTraceSamples::tc_;

}   // namespace

TEST_F(RegCTraceSamples, GenerateMjdbgFixtures)
{
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
                {"fieldAndCall", "(I)I", "_mj2x_regc_load_cached_instance_i32_field"},
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
        {
            "RegCRuntimeSurfaceSample",
            "protected_samples/regc/RegCRuntimeSurfaceSample.java",
            "runtime-backed div/rem ABI calls, arithmetic trap routing, and register locals",
            {
                {"divOrCatch", "(II)I", "mj2x_regc_div_i32"},
                {"remOrCatch", "(II)I", "mj2x_regc_rem_i32"},
                {"longDiv", "(JJ)J", "mj2x_regc_div_i64"},
            },
        },
        {
            "RegCIntrinsicSurfaceSample",
            "protected_samples/regc/RegCIntrinsicSurfaceSample.java",
            "Math/parseInt/wrapper/Random intrinsic calls without JNI dispatch at matched call "
            "sites",
            {
                {"parseIntLiteral", "()I", "_mj2x_intrinsic_parse_int"},
                {"sinAndPow", "(D)D", "mj2x_math_pow"},
                {"boxedInt", "(I)I", "mj2x_int_int_value"},
                {"randomNextInt", "(J)I", "mj2x_random_next_int"},
            },
        },
        {
            "RegCIntrinsicBenchmarkSample",
            "protected_samples/regc/RegCIntrinsicBenchmarkSample.java",
            "committed intrinsic/cache benchmark fixture with structural generated-C metrics",
            {
                {"stringReadHotPath",
                 "(Ljava/lang/String;Ljava/lang/String;)I",
                 "_mj2x_intrinsic_string_contains"},
                {"builderChars",
                 "([CI)Ljava/lang/String;",
                 "mj2x_string_builder_append_char_array"},
                {"parseAndCompare",
                 "(Ljava/lang/String;Ljava/lang/String;)I",
                 "_mj2x_intrinsic_float_compare"},
                {"multiArray", "(II)Ljava/lang/Object;", "_mj2x_regc_alloc_multi_array_cached"},
            },
        },
    };

    ASSERT_EQ(countJavaSources(test::getResourcePath("protected_samples/regc")), samples.size())
        << "Every committed RegC protected sample needs a SampleSpec and TRACE_SURFACE generator.";

    for (const auto& sample : samples) {
        generateSample(sample);
    }
}

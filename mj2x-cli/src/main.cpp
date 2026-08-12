#include "mj2x/mj2x.hpp"
#include "mj2x/backend/llvmir/llvm_ir_backend.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/init_clinit_mover_preprocessor.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/multianewarray_preprocessor.hpp"
#include "mj2x/preprocessor/native_annotation_preprocessor.hpp"
#include "mj2x/preprocessor/native_method_prefix_preprocessor.hpp"
#include "mj2x/postprocessor/native_loader_postprocessor.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "config_file.hpp"
#include "cpp_source_dialect.hpp"
#include "native_compiler.hpp"
#include "native_project_generator.hpp"
#include "jzip/memory_segment.hpp"
#include "typed/cast.hpp"
#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static void printUsage()
{
    printf("Usage: mj2x-cli -i <input.class|input.jar> -o <output.class|output.jar> [options]\n");
    printf("Options:\n");
    printf("  --config <file>           Load JSON options; command line values override it\n");
    printf("  --emit-c <file>            Output C source to file\n");
    printf("  --emit-cpp <file>          Output RegC C++17 source to file\n");
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    printf("  --emit-llvm <file>         Output LLVM IR source to file\n");
#endif
#ifdef MJ2X_DISABLE_LLVM_BACKEND
    printf("  --backend <stackc|regc|vmc4> Select native emitter (default: stackc; "
           "cbackend is a legacy alias)\n");
#else
    printf("  --backend <stackc|regc|llvm|vmc4> Select native emitter (default: stackc; "
           "cbackend is a legacy alias)\n");
#endif
    printf("  --regc-trace-calls          RegC: 生成运行时调用日志（[CALL]/[DEEP]，stderr，排查用）\n");
    printf("  --regc-comment-level LEVEL  RegC: 生成注释级别 none|summary|detailed（默认 summary）\n");
    printf("  --regc-safe-env             RegC: 开启 safe-env（JNI 函数表快照 + 方法入口恢复；"
           "捕获时表已污染则 JNI_OnLoad fail closed）\n");
    printf("  --regc-safe-env-debug       RegC: safe-env 调试观察模式（检出 hook 只打告警不 fail，"
           "隐含开启 --regc-safe-env）\n");
    printf("  --stackc-exception-check <full|lite> StackC pending 异常检查密度（默认 full；"
           "lite 仅保留 handler 边界检查，对标 JNIC 稀疏检查）\n");
    printf("  --emit-native-dir <dir>     RegC: 多文件输出（mj2x_regc_api.h + runtime.c + 每类 "
           "m_<Class>.c + entry.c，库代码与用户代码分离）\n");
    printf(
        "  --native-annotation <desc> Only compile methods annotated with JVM descriptor desc\n");
    printf("  --native-method-prefix <p> Only compile methods whose owner.name starts with p; "
           "repeatable\n");
    printf(
        "  --compile                  Compile emitted C into a native library beside --output\n");
    printf("  --compile-mode <mode>      Select native compile input mode (file, pipe)\n");
    printf("  --c-compiler <name>        C compiler or build driver (msvc, gcc, clang, zig, xmake)\n");
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    printf("  --llvm-tool <clang|llc>    LLVM IR compiler path (default: clang)\n");
#endif
    printf("  --generate-cmake <dir>     Generate a standalone CMake project for emitted C\n");
    printf("  --generate-xmake <dir>     Generate a standalone xmake project with mj2x-crt\n");
    printf("  --vmc4-dispatcher <kind>   VMC4(测试性): dispatcher 形态 switch_table|funcptr_table|"
           "computed_goto|if_nest|state_dispatcher（默认 switch_table；computed_goto 需 "
           "GCC/clang 编译）\n");
    printf("  --vmc4-seed <hex>          VMC4: 主 seed，驱动每实例 opcode 映射/handler 顺序"
           "（同 seed 同输入 → 同产物，可复现构建）\n");
    printf("  --vmc4-granularity <g>     VMC4: 实例粒度 class|method（默认 class；method 每方法"
           "独立 VM 实例，最强可变性、体积膨胀最大）\n");
    printf("  --vmc4-alias <n>           VMC4: 每语义 ALT 别名字节数 0..3（默认 1；0 关闭别名）\n");
    printf("  --vmc4-bogus <n>           VMC4: 注入的不可达假 handler 数 0..4（默认 1；"
           "0 关闭，只污染静态分析不参与执行）\n");
    printf("  --vmc4-drift <n>           VMC4: 执行期 dispatch 变体数 0..4（默认 0 关闭；"
           "开启后语义标签按字节偏移周期轮转，unmap 表解码）\n");
    printf("  --vmc4-drift-period <n>    VMC4: drift 变体切换周期（字节偏移单位，默认 16）\n");
    printf("  --vmc4-trace-surface <f>   VMC4: 写 expected trace surface 文件（mjdbg 红队契约，"
           "同 seed+同输入 → 同内容）\n");
    printf("  --dump-mir                 Dump MIR intermediate representation\n");
    printf("  --help                     Print help\n");
}

enum class CEmitterBackend
{
    StackC,
    RegC,
    LlvmIr,
    Vmc4,
};

enum class NativeBuildDriver
{
    Direct,
    Xmake,
};

class TemporaryProjectCleanup
{
public:
    ~TemporaryProjectCleanup()
    {
        if (path_.empty()) return;
        if (removeAll(path_)) return;
        fprintf(stderr,
                "[mj2x] WARNING: cannot clean temporary xmake project: %s\n",
                path_.string().c_str());
    }

    void set(std::filesystem::path path) { path_ = std::move(path); }

private:
    static void makeWritable(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        }
#endif
    }

    static bool removeAll(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (!ec) return true;

        makeWritable(path);
        ec.clear();
        std::filesystem::recursive_directory_iterator iterator(
            path, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!ec && iterator != end) {
            makeWritable(iterator->path());
            iterator.increment(ec);
        }

        ec.clear();
        std::filesystem::remove_all(path, ec);
        return !ec;
    }

    std::filesystem::path path_;
};

struct CliOptions
{
    std::string              inputPath;
    std::string              outputPath;
    std::string              cOutputPath;
    std::string              cppOutputPath;
    std::string              llvmOutputPath;
    std::string              nativeDirOutput;
    std::string              cmakeProjectDir;
    std::string              xmakeProjectDir;
    std::string              nativeAnnotation;
    std::vector<std::string> nativeMethodPrefixes;
    CEmitterBackend          cBackendKind     = CEmitterBackend::StackC;
    mj2x::cli::CCompilerKind cCompilerKind    = mj2x::cli::CCompilerKind::Auto;
    mj2x::cli::CompileMode   compileMode      = mj2x::cli::CompileMode::File;
    mj2x::cli::LlvmTool      llvmTool         = mj2x::cli::LlvmTool::Clang;
    NativeBuildDriver        buildDriver      = NativeBuildDriver::Direct;
    bool                     backendSelected  = false;
    bool                     compileNative    = false;
    bool                     compileModeSelected = false;
    bool                     compilerSelected = false;
    bool                     llvmToolSelected = false;
    bool                     dumpMIR          = false;
    bool                     regcTraceCalls   = false;
    mj2x::RegCCommentLevel   regcCommentLevel = mj2x::RegCCommentLevel::Summary;
    bool                     regcSafeEnv      = false;
    bool                     regcSafeEnvDebug = false;
    std::string              stackcExceptionCheck = "full";
    std::string              vmc4Dispatcher      = "switch_table";
    uint64_t                 vmc4Seed            = 0;
    bool                     vmc4SeedSelected    = false;
    std::string              vmc4Granularity     = "class";
    int                      vmc4Alias           = 1;
    int                      vmc4Bogus           = 1;
    int                      vmc4Drift           = 0;
    int                      vmc4DriftPeriod     = 16;
    std::string              vmc4TraceSurface;   ///< 非空时写 expected trace surface 文件
};

static bool parseBackendName(const std::string& name, CEmitterBackend& backend)
{
    if (name == "stackc" || name == "cbackend" || name == "c") {
        backend = CEmitterBackend::StackC;
        return true;
    }
    if (name == "regc") {
        backend = CEmitterBackend::RegC;
        return true;
    }
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    if (name == "llvm") {
        backend = CEmitterBackend::LlvmIr;
        return true;
    }
#endif
    if (name == "vmc4") {
        backend = CEmitterBackend::Vmc4;
        return true;
    }
    return false;
}

static bool applyConfigOptions(const mj2x::cli::ConfigFileOptions& config, CliOptions& options,
                               std::string& error)
{
    if (config.input) options.inputPath = *config.input;
    if (config.output) options.outputPath = *config.output;
    if (config.emitC) options.cOutputPath = *config.emitC;
    if (config.emitCpp) options.cppOutputPath = *config.emitCpp;
    if (config.emitLlvm) options.llvmOutputPath = *config.emitLlvm;
    if (config.nativeAnnotation) options.nativeAnnotation = *config.nativeAnnotation;
    if (config.hasNativeMethodPrefixes) options.nativeMethodPrefixes = config.nativeMethodPrefixes;
    if (config.compile) options.compileNative = *config.compile;
    if (config.compileMode) {
        options.compileModeSelected = true;
        if (!mj2x::cli::parseCompileModeName(*config.compileMode, options.compileMode)) {
            error = "Unknown config compile_mode value. Expected file or pipe.";
            return false;
        }
    }
    if (config.llvmTool) {
        options.llvmToolSelected = true;
        if (!mj2x::cli::parseLlvmToolName(*config.llvmTool, options.llvmTool)) {
            error = "Unknown config llvm_tool value. Expected clang or llc.";
            return false;
        }
    }
    if (config.generateXmake) options.xmakeProjectDir = *config.generateXmake;
    if (config.generateCmake) options.cmakeProjectDir = *config.generateCmake;
    if (config.dumpMir) options.dumpMIR = *config.dumpMir;
    if (config.backend) {
        options.backendSelected = true;
        if (!parseBackendName(*config.backend, options.cBackendKind)) {
#ifdef MJ2X_DISABLE_LLVM_BACKEND
            error = "Unknown config backend value. Expected stackc, regc, or vmc4.";
#else
            error = "Unknown config backend value. Expected stackc, regc, or llvm.";
#endif
            return false;
        }
    }
    if (config.cCompiler) {
        options.compilerSelected = true;
        if (*config.cCompiler == "xmake") {
            options.buildDriver = NativeBuildDriver::Xmake;
        }
        else if (!mj2x::cli::parseCompilerName(*config.cCompiler, options.cCompilerKind)) {
            error = "Unknown config c_compiler value. Expected msvc, gcc, clang, zig, or xmake.";
            return false;
        }
    }
    return true;
}

static bool hasExtension(std::string path, const std::string& ext)
{
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    for (char& ch : suffix) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return suffix == ext;
}

static std::vector<ca::u8> readFileBytes(const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto                size = ifs.tellg();
    std::vector<ca::u8> bytes(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(const std::string&         name,
                                                              const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    zip::Crc32 crc;
    crc.update(data.data(), data.size());
    local->setFileName(name);
    local->setGeneralPurposeBitFlag(0x0800);
    local->setCompressionMethod(0);
    local->setCrc32(crc.value());
    local->setCompressedSize(static_cast<ca::i64>(data.size()));
    local->setUncompressedSize(static_cast<ca::i64>(data.size()));
    local->setFileData(data);
    local->setOffset(0);
    return local;
}

static int embedNativeLibraryInJar(const std::string& jarPath, const std::string& entryName,
                                   const std::filesystem::path& nativeLibraryPath)
{
    auto jarBytes    = readFileBytes(jarPath);
    auto nativeBytes = readFileBytes(nativeLibraryPath);
    if (jarBytes.empty() || nativeBytes.empty()) {
        fprintf(stderr, "[mj2x] ERROR: cannot embed native library into output JAR.\n");
        return 1;
    }

    auto             original = jzip::ZipIO::readJvm(jzip::MemorySegment(jarBytes));
    jzip::ZipArchive rewritten;
    bool             replaced = false;
    for (const auto* local : original.getLocalFiles()) {
        const auto& name = local->getFileName();
        auto data = name == entryName ? nativeBytes : jzip::ZipIO::decompressLocalFile(*local);
        if (name == entryName) replaced = true;
        rewritten.addPart(makeStoredEntry(name, data));
    }
    if (!replaced) rewritten.addPart(makeStoredEntry(entryName, nativeBytes));

    auto          outBytes = jzip::ZipIO::writeDirect(rewritten);
    std::ofstream ofs(jarPath, std::ios::binary);
    if (!ofs) {
        fprintf(stderr, "[mj2x] ERROR: cannot rewrite output JAR: %s\n", jarPath.c_str());
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(outBytes.data()),
              static_cast<std::streamsize>(outBytes.size()));
    printf("[mj2x] Embedded native library in JAR: %s\n", entryName.c_str());
    return 0;
}

static bool isNativeAnnotationDescriptor(const std::string& descriptor)
{
    return descriptor.size() >= 3 && descriptor.front() == 'L' && descriptor.back() == ';' &&
           descriptor.find('.') == std::string::npos;
}

static bool copyCrtForCMakeProject(const std::filesystem::path& projectDir,
                                   const std::filesystem::path& crtRoot)
{
    namespace fs = std::filesystem;
    const fs::path sourceRoot = crtRoot / "src";
    const fs::path destinationRoot = projectDir / "mj2x-crt";
    std::error_code ec;
    if (!fs::is_regular_file(sourceRoot / "regc" / "mj2x_regc_crt.h", ec)) return false;
    for (const auto& entry : fs::recursive_directory_iterator(sourceRoot, ec)) {
        if (ec) return false;
        const fs::path destination = destinationRoot / entry.path().lexically_relative(sourceRoot);
        if (entry.is_directory()) {
            fs::create_directories(destination, ec);
        }
        else if (entry.is_regular_file()) {
            fs::create_directories(destination.parent_path(), ec);
            if (!ec) fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) return false;
    }
    return true;
}

static int generateCMakeProject(const std::string& projectDirValue,
                                const std::string& nativeOutputPath,
                                const std::filesystem::path& crtRoot)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        projectDir(projectDirValue);
    fs::create_directories(projectDir, ec);
    if (ec) {
        fprintf(stderr,
                "[mj2x] ERROR: cannot create CMake project directory: %s\n",
                projectDir.string().c_str());
        return 1;
    }

    fs::path sourcePath(nativeOutputPath);
    if (!fs::exists(sourcePath, ec)) {
        fprintf(stderr,
                "[mj2x] ERROR: emitted native source does not exist: %s\n",
                sourcePath.string().c_str());
        return 1;
    }

    const bool isCppSource = hasExtension(sourcePath.string(), ".cpp");
    fs::path    expectedSource = projectDir / sourcePath.filename();
    if (fs::absolute(sourcePath, ec) != fs::absolute(expectedSource, ec)) {
        fs::copy_file(sourcePath, expectedSource, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot copy emitted C into CMake project: %s\n",
                    ec.message().c_str());
            return 1;
        }
    }

    const bool needsCrt = !crtRoot.empty();
    if (needsCrt && !copyCrtForCMakeProject(projectDir, crtRoot)) {
        fprintf(stderr, "[mj2x] ERROR: cannot copy mj2x-crt into CMake project.\n");
        return 1;
    }

    {
        std::ofstream out(projectDir / "CMakeLists.txt", std::ios::binary);
        if (!out) {
            fprintf(stderr, "[mj2x] ERROR: cannot write CMakeLists.txt\n");
            return 1;
        }
        out << "cmake_minimum_required(VERSION 3.16)\n";
        out << "project(mj2x_native C" << (isCppSource ? " CXX" : "") << ")\n\n";
        if (isCppSource) {
            out << "set(CMAKE_CXX_STANDARD 17)\n";
            out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
        }
        out << "find_package(JNI REQUIRED)\n\n";
        out << "if(MSVC)\n";
        out << "  add_compile_options(/utf-8)\n";
        out << "endif()\n\n";
        if (needsCrt) {
            out << "file(GLOB_RECURSE MJ2X_CRT_SOURCES CONFIGURE_DEPENDS "
                   "\"${CMAKE_CURRENT_SOURCE_DIR}/mj2x-crt/*.c\")\n";
        }
        out << "add_library(mj2x_native SHARED " << sourcePath.filename().string();
        if (needsCrt) out << " ${MJ2X_CRT_SOURCES}";
        out << ")\n";
        out << "target_include_directories(mj2x_native PRIVATE ${JNI_INCLUDE_DIRS})\n";
        if (needsCrt) {
            out << "target_include_directories(mj2x_native PRIVATE "
                   "${CMAKE_CURRENT_SOURCE_DIR}/mj2x-crt)\n";
        }
        out << "if(UNIX AND NOT APPLE)\n";
        out << "  target_link_libraries(mj2x_native PRIVATE pthread)\n";
        out << "endif()\n";
        out << "set_target_properties(mj2x_native PROPERTIES OUTPUT_NAME mj2x_native)\n";
    }

    {
        std::ofstream out(projectDir / ".gitignore", std::ios::binary);
        if (!out) {
            fprintf(stderr, "[mj2x] ERROR: cannot write .gitignore\n");
            return 1;
        }
        out << "build/\n";
        out << "cmake-build-*/\n";
        out << "*.obj\n*.o\n*.dll\n*.so\n*.dylib\n";
    }

    {
        std::ofstream out(projectDir / "README.md", std::ios::binary);
        if (!out) {
            fprintf(stderr, "[mj2x] ERROR: cannot write README.md\n");
            return 1;
        }
        out << "# mj2x native CMake project\n\n";
        out << "Build:\n\n";
        out << "```sh\n";
        out << "cmake -S . -B build\n";
        out << "cmake --build build --config Release\n";
        out << "```\n\n";
        out << "`JAVA_HOME` or CMake's JNI discovery must point at a JDK with JNI headers.\n";
    }

    printf("[mj2x] CMake project written: %s\n", projectDir.string().c_str());
    return 0;
}

static bool createTemporaryXmakeProject(const std::string& outputPath,
                                        std::filesystem::path& projectDir)
{
    namespace fs = std::filesystem;
    fs::path base = fs::path(outputPath).parent_path();
    if (base.empty()) base = ".";
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) return false;
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate =
            base / (".mj2x-xmake-" + std::to_string(token) + "-" + std::to_string(attempt));
        if (fs::create_directory(candidate, ec)) {
            projectDir = std::move(candidate);
            return true;
        }
        if (ec && ec != std::errc::file_exists) return false;
        ec.clear();
    }
    return false;
}

int main(int argc, char** argv)
{
    // 无缓冲 stdout/stderr：崩溃/异常退出时日志不丢（定位 pipeline 崩溃点）。
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    CliOptions               options;
    std::string              configPath;
    TemporaryProjectCleanup temporaryProjectCleanup;

    bool commandLineNativePrefixesSeen = false;
    bool commandLineNativeOutputSeen   = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                fprintf(stderr, "--config requires a JSON file path.\n");
                printUsage();
                return 1;
            }
            if (!configPath.empty()) {
                fprintf(stderr, "--config may only be specified once.\n");
                printUsage();
                return 1;
            }
            configPath = argv[++i];
        }
    }

    if (!configPath.empty()) {
        mj2x::cli::ConfigFileOptions configOptions;
        std::string                  error;
        if (!mj2x::cli::loadConfigFile(configPath, configOptions, error) ||
            !applyConfigOptions(configOptions, options, error)) {
            fprintf(stderr, "[mj2x] ERROR: %s\n", error.c_str());
            return 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ++i;
            continue;
        }
        if (arg == "-i" && i + 1 < argc)
            options.inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            options.outputPath = argv[++i];
        else if (arg == "--emit-c" && i + 1 < argc) {
            if (!commandLineNativeOutputSeen) {
                options.cOutputPath.clear();
                options.cppOutputPath.clear();
                options.llvmOutputPath.clear();
                commandLineNativeOutputSeen = true;
            }
            options.cOutputPath = argv[++i];
        }
        else if (arg == "--emit-cpp" && i + 1 < argc) {
            if (!commandLineNativeOutputSeen) {
                options.cOutputPath.clear();
                options.cppOutputPath.clear();
                options.llvmOutputPath.clear();
                options.nativeDirOutput.clear();
                commandLineNativeOutputSeen = true;
            }
            options.cppOutputPath = argv[++i];
        }
        else if (arg == "--emit-native-dir" && i + 1 < argc) {
            if (!commandLineNativeOutputSeen) {
                options.cOutputPath.clear();
                options.cppOutputPath.clear();
                options.llvmOutputPath.clear();
                options.nativeDirOutput.clear();
                commandLineNativeOutputSeen = true;
            }
            options.nativeDirOutput = argv[++i];
        }
#ifndef MJ2X_DISABLE_LLVM_BACKEND
        else if (arg == "--emit-llvm" && i + 1 < argc) {
            if (!commandLineNativeOutputSeen) {
                options.cOutputPath.clear();
                options.cppOutputPath.clear();
                options.llvmOutputPath.clear();
                commandLineNativeOutputSeen = true;
            }
            options.llvmOutputPath = argv[++i];
        }
#endif
        else if (arg == "--native-annotation" && i + 1 < argc)
            options.nativeAnnotation = argv[++i];
        else if (arg == "--native-method-prefix" && i + 1 < argc) {
            if (!commandLineNativePrefixesSeen) {
                options.nativeMethodPrefixes.clear();
                commandLineNativePrefixesSeen = true;
            }
            options.nativeMethodPrefixes.push_back(argv[++i]);
        }
        else if (arg == "--compile")
            options.compileNative = true;
        else if (arg == "--compile-mode" && i + 1 < argc) {
            options.compileModeSelected = true;
            if (!mj2x::cli::parseCompileModeName(argv[++i], options.compileMode)) {
                fprintf(stderr, "Unknown --compile-mode value. Expected file or pipe.\n");
                printUsage();
                return 1;
            }
        }
        else if (arg == "--generate-cmake" && i + 1 < argc)
            options.cmakeProjectDir = argv[++i];
        else if (arg == "--generate-xmake" && i + 1 < argc)
            options.xmakeProjectDir = argv[++i];
        else if (arg == "--c-compiler" && i + 1 < argc) {
            options.compilerSelected = true;
            const std::string compilerName = argv[++i];
            if (compilerName == "xmake") {
                options.buildDriver = NativeBuildDriver::Xmake;
            }
            else if (!mj2x::cli::parseCompilerName(compilerName, options.cCompilerKind)) {
                fprintf(stderr,
                        "Unknown --c-compiler value. Expected msvc, gcc, clang, zig, or xmake.\n");
                printUsage();
                return 1;
            }
        }
#ifndef MJ2X_DISABLE_LLVM_BACKEND
        else if (arg == "--llvm-tool" && i + 1 < argc) {
            options.llvmToolSelected = true;
            if (!mj2x::cli::parseLlvmToolName(argv[++i], options.llvmTool)) {
                fprintf(stderr, "Unknown --llvm-tool value. Expected clang or llc.\n");
                printUsage();
                return 1;
            }
        }
#endif
        else if (arg == "--backend" && i + 1 < argc) {
            options.backendSelected = true;
            if (!parseBackendName(argv[++i], options.cBackendKind)) {
#ifdef MJ2X_DISABLE_LLVM_BACKEND
                fprintf(stderr,
                        "Unknown --backend value. Expected stackc, regc, or vmc4. Legacy alias cbackend "
                        "is also accepted.\n");
#else
                fprintf(stderr,
                        "Unknown --backend value. Expected stackc, regc, llvm, or vmc4. Legacy alias cbackend "
                        "is also accepted.\n");
#endif
                printUsage();
                return 1;
            }
        }
        else if (arg == "--dump-mir")
            options.dumpMIR = true;
        else if (arg == "--regc-trace-calls")
            options.regcTraceCalls = true;
        else if (arg == "--regc-comment-level" && i + 1 < argc) {
            const std::string level = argv[++i];
            if (level == "none")
                options.regcCommentLevel = mj2x::RegCCommentLevel::None;
            else if (level == "summary")
                options.regcCommentLevel = mj2x::RegCCommentLevel::Summary;
            else if (level == "detailed")
                options.regcCommentLevel = mj2x::RegCCommentLevel::Detailed;
            else {
                fprintf(stderr,
                        "Unknown --regc-comment-level value. Expected none, summary, or detailed.\n");
                printUsage();
                return 1;
            }
        }
        else if (arg == "--regc-safe-env")
            options.regcSafeEnv = true;
        else if (arg == "--regc-safe-env-debug") {
            options.regcSafeEnv      = true;
            options.regcSafeEnvDebug = true;
        }
        else if (arg == "--vmc4-dispatcher" && i + 1 < argc) {
            options.vmc4Dispatcher = argv[++i];
        }
        else if (arg == "--vmc4-seed" && i + 1 < argc) {
            // 16 进制（0x...）或十进制均可，strtoull 自动识别。
            options.vmc4Seed = std::strtoull(argv[++i], nullptr, 0);
            options.vmc4SeedSelected = true;
        }
        else if (arg == "--vmc4-granularity" && i + 1 < argc) {
            options.vmc4Granularity = argv[++i];
        }
        else if (arg == "--vmc4-alias" && i + 1 < argc) {
            options.vmc4Alias = std::atoi(argv[++i]);
        }
        else if (arg == "--vmc4-bogus" && i + 1 < argc) {
            options.vmc4Bogus = std::atoi(argv[++i]);
        }
        else if (arg == "--vmc4-drift" && i + 1 < argc) {
            options.vmc4Drift = std::atoi(argv[++i]);
        }
        else if (arg == "--vmc4-drift-period" && i + 1 < argc) {
            options.vmc4DriftPeriod = std::atoi(argv[++i]);
        }
        else if (arg == "--vmc4-trace-surface" && i + 1 < argc) {
            options.vmc4TraceSurface = argv[++i];
        }
        else if (arg == "--stackc-exception-check" && i + 1 < argc)
            options.stackcExceptionCheck = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        else {
            printUsage();
            return 1;
        }
    }

    if (options.inputPath.empty() || options.outputPath.empty()) {
        printUsage();
        return 1;
    }

    const int nativeOutputCount = static_cast<int>(!options.cOutputPath.empty()) +
                                  static_cast<int>(!options.cppOutputPath.empty()) +
                                  static_cast<int>(!options.llvmOutputPath.empty());
    if (nativeOutputCount > 1) {
        fprintf(stderr, "--emit-c, --emit-cpp, and --emit-llvm are mutually exclusive.\n");
        return 1;
    }
    if (!options.cppOutputPath.empty() && !hasExtension(options.cppOutputPath, ".cpp")) {
        fprintf(stderr,
                "--emit-cpp requires a .cpp output path.\n");
        return 1;
    }
    if (!options.cppOutputPath.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        fprintf(stderr, "--emit-cpp currently requires --backend regc.\n");
        return 1;
    }
    if (!options.llvmOutputPath.empty() && !hasExtension(options.llvmOutputPath, ".ll")) {
        fprintf(stderr, "--emit-llvm requires a .ll output path.\n");
        return 1;
    }
    if (!options.llvmOutputPath.empty() && options.cBackendKind != CEmitterBackend::LlvmIr) {
        fprintf(stderr, "--emit-llvm requires --backend llvm.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.llvmOutputPath.empty() &&
        options.compileMode != mj2x::cli::CompileMode::Pipe) {
        fprintf(stderr, "--backend llvm requires --emit-llvm.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr &&
        options.compileMode == mj2x::cli::CompileMode::Pipe) {
        fprintf(stderr, "--backend llvm does not support --compile-mode pipe.\n");
        return 1;
    }
    if (options.llvmToolSelected && options.cBackendKind != CEmitterBackend::LlvmIr) {
        fprintf(stderr, "--llvm-tool requires --backend llvm.\n");
        return 1;
    }
    if (options.regcTraceCalls && options.cBackendKind != CEmitterBackend::RegC) {
        fprintf(stderr, "--regc-trace-calls requires --backend regc.\n");
        return 1;
    }
    if ((options.regcSafeEnv || options.regcSafeEnvDebug) &&
        options.cBackendKind != CEmitterBackend::RegC) {
        fprintf(stderr, "--regc-safe-env requires --backend regc.\n");
        return 1;
    }
    if (options.stackcExceptionCheck != "full" && options.stackcExceptionCheck != "lite") {
        fprintf(stderr, "--stackc-exception-check must be full or lite.\n");
        return 1;
    }
    if (!options.nativeDirOutput.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        fprintf(stderr, "--emit-native-dir requires --backend regc.\n");
        return 1;
    }
    if (options.llvmToolSelected && !options.compileNative) {
        fprintf(stderr, "--llvm-tool requires --compile.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.compilerSelected) {
        fprintf(stderr, "--backend llvm cannot be combined with --c-compiler.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr &&
        (!options.cmakeProjectDir.empty() || !options.xmakeProjectDir.empty())) {
        fprintf(stderr,
                "--backend llvm does not support --generate-cmake or --generate-xmake.\n");
        return 1;
    }

    if (!options.nativeAnnotation.empty() &&
        !isNativeAnnotationDescriptor(options.nativeAnnotation)) {
        fprintf(
            stderr,
            "--native-annotation must be a JVM annotation descriptor like Lcom/example/Native;.\n");
        printUsage();
        return 1;
    }

    if (!options.nativeAnnotation.empty() && !options.nativeMethodPrefixes.empty()) {
        fprintf(stderr, "--native-annotation and --native-method-prefix are mutually exclusive.\n");
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && !options.xmakeProjectDir.empty()) {
        fprintf(stderr, "--generate-cmake and --generate-xmake are mutually exclusive.\n");
        printUsage();
        return 1;
    }

    if (options.compileNative && !options.xmakeProjectDir.empty()) {
        if (options.compilerSelected && options.buildDriver == NativeBuildDriver::Direct) {
            fprintf(stderr,
                    "A direct --c-compiler cannot be combined with --generate-xmake and "
                    "--compile.\n");
            return 1;
        }
        options.buildDriver = NativeBuildDriver::Xmake;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        !options.cmakeProjectDir.empty()) {
        fprintf(stderr, "--c-compiler xmake cannot be combined with --generate-cmake.\n");
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.compileMode == mj2x::cli::CompileMode::Pipe) {
        fprintf(stderr, "xmake compilation does not support --compile-mode pipe.\n");
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.xmakeProjectDir.empty()) {
        std::filesystem::path temporaryProject;
        if (!createTemporaryXmakeProject(options.outputPath, temporaryProject)) {
            fprintf(stderr, "[mj2x] ERROR: cannot create temporary xmake project.\n");
            return 1;
        }
        temporaryProjectCleanup.set(temporaryProject);
        options.xmakeProjectDir = temporaryProject.string();
    }

    if (options.compileMode == mj2x::cli::CompileMode::Pipe &&
        (!options.cOutputPath.empty() || !options.cppOutputPath.empty() ||
         !options.llvmOutputPath.empty() ||
         !options.cmakeProjectDir.empty() ||
         !options.xmakeProjectDir.empty())) {
        fprintf(stderr,
                "--compile-mode pipe cannot be combined with --emit-c or generated native "
                "projects.\n");
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && options.cOutputPath.empty() &&
        options.cppOutputPath.empty() && options.llvmOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.cmakeProjectDir) / "native.c").string();
    }

    if (!options.xmakeProjectDir.empty() && options.cOutputPath.empty() &&
        options.cppOutputPath.empty() && options.llvmOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.xmakeProjectDir) / "src" / "generated" / "native.c")
                .string();
    }

    const bool pipeCompile =
        options.compileNative && options.compileMode == mj2x::cli::CompileMode::Pipe;
    const bool nativeDirMode = !options.nativeDirOutput.empty();
    const std::string nativeOutputPath = !options.llvmOutputPath.empty()
                                             ? options.llvmOutputPath
                                             : (options.cppOutputPath.empty()
                                                    ? options.cOutputPath
                                                    : options.cppOutputPath);

    if (options.backendSelected && nativeOutputPath.empty() && !nativeDirMode && !pipeCompile) {
        fprintf(stderr,
                "--backend requires --emit-c, --emit-cpp, --emit-llvm, --emit-native-dir, or a "
                "generated native project.\n");
        printUsage();
        return 1;
    }

    if (options.compileNative && nativeOutputPath.empty() && !nativeDirMode && !pipeCompile) {
        fprintf(stderr,
                "--compile requires --emit-c, --emit-cpp, --emit-llvm, --emit-native-dir, or a "
                "generated native project.\n");
        printUsage();
        return 1;
    }

    if (options.compileModeSelected && !options.compileNative) {
        fprintf(stderr, "--compile-mode requires --compile.\n");
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.cmakeProjectDir, ec);
        if (ec) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot create CMake project directory: %s\n",
                    options.cmakeProjectDir.c_str());
            return 1;
        }
    }

    if (!options.xmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(options.xmakeProjectDir) / "src" / "generated", ec);
        if (ec) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot create xmake project directory: %s\n",
                    options.xmakeProjectDir.c_str());
            return 1;
        }
    }

    if (options.compilerSelected && !options.compileNative) {
        fprintf(stderr, "--c-compiler requires --compile.\n");
        printUsage();
        return 1;
    }

    mj2x::PipelineConfig config;
    config.inputPath  = options.inputPath;
    config.outputPath = options.outputPath;
    config.dumpMIR    = options.dumpMIR;

    const bool jarOutput =
        hasExtension(options.inputPath, ".jar") && hasExtension(options.outputPath, ".jar");
    const bool jarNativeLoaderEnabled =
        (options.compileNative || !options.xmakeProjectDir.empty()) && jarOutput;
    std::filesystem::path nativeLibraryPath =
        mj2x::cli::nativeLibraryPathForOutput(options.outputPath);
    const std::string nativeResourceEntry =
        "META-INF/mj2x/native/" + nativeLibraryPath.filename().string();
    const std::string nativeResourcePath = nativeResourceEntry;
    const std::string nativeTempSuffix   = nativeLibraryPath.extension().string();

    mj2x::Pipeline pipeline(std::move(config));

    const bool needsNativeSource = !nativeOutputPath.empty() || pipeCompile || nativeDirMode;
    mj2x::Backend* nativeSourceBackend = nullptr;

    if (needsNativeSource) {
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            auto llvmBackend = std::make_unique<mj2x::LlvmIrBackend>();
            if (!options.llvmOutputPath.empty()) llvmBackend->setOutputPath(options.llvmOutputPath);
            nativeSourceBackend = llvmBackend.get();
            pipeline.registerBackend(std::move(llvmBackend));
        }
        else if (options.cBackendKind == CEmitterBackend::RegC) {
            auto regcBackend = std::make_unique<mj2x::RegCBackend>();
            if (!options.cOutputPath.empty()) regcBackend->setOutputPath(options.cOutputPath);
            if (options.regcTraceCalls) regcBackend->setTraceCallLog(true);
            regcBackend->setCommentLevel(options.regcCommentLevel);
            if (options.regcSafeEnv) regcBackend->setSafeEnv(true);
            if (options.regcSafeEnvDebug) regcBackend->setSafeEnvDebug(true);
            if (!options.nativeDirOutput.empty())
                regcBackend->setSplitOutputDir(options.nativeDirOutput);
            nativeSourceBackend = regcBackend.get();
            // RegC 的 native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责。
            pipeline.registerBackend(std::move(regcBackend));
        }
        else if (options.cBackendKind == CEmitterBackend::Vmc4) {
            auto vmc4Backend = std::make_unique<mj2x::Vmc4Backend>();
            if (!options.cOutputPath.empty()) vmc4Backend->setOutputPath(options.cOutputPath);
            // dispatcher 形态字符串 -> 枚举（默认 switch_table）。
            mj2x::Vmc4Options vmc4Opts;
            vmc4Opts.seed = options.vmc4Seed;
            const std::string& disp = options.vmc4Dispatcher;
            if (disp == "funcptr_table" || disp == "funcptr")
                vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::FuncPtrTable;
            else if (disp == "computed_goto" || disp == "goto")
                vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::ComputedGoto;
            else if (disp == "if_nest" || disp == "ifnest")
                vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::IfNest;
            else if (disp == "state_dispatcher" || disp == "state")
                vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::StateDispatcher;
            else
                vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::SwitchTable;
            // 实例粒度：per-class（默认，同类方法共享 VM 实例）/ per-method。
            if (options.vmc4Granularity == "method" || options.vmc4Granularity == "per-method")
                vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerMethod;
            else
                vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerClass;
            vmc4Opts.aliasCount = options.vmc4Alias;
            vmc4Opts.bogusHandlers = options.vmc4Bogus;
            vmc4Opts.driftVariants = options.vmc4Drift;
            vmc4Opts.driftPeriod = options.vmc4DriftPeriod;
            vmc4Backend->setOptions(vmc4Opts);
            // VMC4 复用同样的 native-loader clinit 注入（NativeLoaderPostprocessor）。
            nativeSourceBackend = vmc4Backend.get();
            pipeline.registerBackend(std::move(vmc4Backend));
        }
        else {
            auto stackCBackend = std::make_unique<mj2x::StackCBackend>();
            if (!options.cOutputPath.empty()) stackCBackend->setOutputPath(options.cOutputPath);
            if (options.stackcExceptionCheck == "lite") stackCBackend->setExceptionCheckLite(true);
            nativeSourceBackend = stackCBackend.get();
            pipeline.registerBackend(std::move(stackCBackend));
        }
    }

    // Bytecode-shaping passes run before MarkAll so every selected method sees
    // the same normalized L1 input regardless of the chosen native backend.
    pipeline.registerPreprocessor(std::make_unique<mj2x::LdcPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::MultiANewArrayPreprocessor>());
    if (!options.nativeAnnotation.empty()) {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::NativeAnnotationPreprocessor>(options.nativeAnnotation));
    }
    else if (!options.nativeMethodPrefixes.empty()) {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::NativeMethodPrefixPreprocessor>(options.nativeMethodPrefixes));
    }
    else {
        // 搬运法（regc_conversion_decisions.md §7.1）：MarkAll 之前先把 <init>/<clinit>
        // 的体搬进 __regc_* 静态方法，让构造逻辑能进入 native 编译流程（<init>/<clinit>
        // 本身保留字节码骨架，不标 ACC_NATIVE）。
        pipeline.registerPreprocessor(std::make_unique<mj2x::InitClinitMoverPreprocessor>());
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    }
    if (needsNativeSource &&
        (options.cBackendKind == CEmitterBackend::StackC ||
         options.cBackendKind == CEmitterBackend::RegC)) {
        // StackC 和 RegC 均使用 RegisterNatives 动态绑定，需要 mj2x/cloader/Loader
        // 桥接类（其 native 方法 registerNativesForClass 由本侧 _mj2x_register_for_class 实现）。
        // native 库加载注入到 Loader.<clinit>（只注入一次，转换类 clinit 不再重复 load）。
        auto cloaderPostprocessor = std::make_unique<mj2x::CLoaderPostprocessor>();
        if (jarNativeLoaderEnabled) {
            cloaderPostprocessor->setNativeLoaderResourcePath(
                nativeResourcePath, nativeTempSuffix.empty() ? ".mj2x" : nativeTempSuffix);
        }
        pipeline.registerPostprocessor(std::move(cloaderPostprocessor));
    }
    if (jarNativeLoaderEnabled) {
        // injectClinitCalls 决定是否往每个 modified 类的 <clinit> 前插 NativeLoader.load：
        //  - RegC / LLVM IR：需要 load（库通过 System.load 加载，再触发 JNI_OnLoad）。
        //  - StackC：自己重写整个 clinit（含 load），故此处只注入资源（injectClinitCalls=false）。
        const bool injectLoadIntoClinit = needsNativeSource &&
                                          (options.cBackendKind == CEmitterBackend::RegC ||
                                           options.cBackendKind == CEmitterBackend::LlvmIr);
        pipeline.registerPostprocessor(std::make_unique<mj2x::NativeLoaderPostprocessor>(
            nativeResourcePath,
            nativeTempSuffix.empty() ? ".mj2x" : nativeTempSuffix,
            injectLoadIntoClinit));
    }

    int rc = pipeline.run();
    if (rc != 0) return rc;

    // VMC4：写 expected trace surface 文件（mjdbg 红队契约，同 seed+同输入 → 同内容）。
    if (!options.vmc4TraceSurface.empty() &&
        options.cBackendKind == CEmitterBackend::Vmc4 && nativeSourceBackend != nullptr) {
        auto* vmc4 = typed::cast<mj2x::Vmc4Backend>(nativeSourceBackend);
        std::ofstream ts(options.vmc4TraceSurface, std::ios::binary | std::ios::trunc);
        if (ts) ts << vmc4->traceSurface();
    }

    std::string                  cppSource;
    mj2x::NativeSourceArtifact nativeArtifact{mj2x::NativeSourceLanguage::C99, {}, {}};
    if (needsNativeSource) {
        if (nativeSourceBackend == nullptr) {
            fprintf(stderr, "[mj2x] ERROR: native source backend is unavailable.\n");
            return 1;
        }
        nativeArtifact = nativeSourceBackend->generatedSourceArtifact();
    }
    if (!options.cppOutputPath.empty()) {
        if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
            fprintf(stderr,
                    "[mj2x] ERROR: --emit-cpp requires a C99 backend source artifact.\n");
            return 1;
        }
        cppSource = mj2x::cli::emitCpp17Dialect(nativeArtifact.content);
        std::ofstream output(options.cppOutputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot write emitted C++ source: %s\n",
                    options.cppOutputPath.c_str());
            return 1;
        }
        output.write(cppSource.data(), static_cast<std::streamsize>(cppSource.size()));
        if (!output) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot write emitted C++ source: %s\n",
                    options.cppOutputPath.c_str());
            return 1;
        }
        nativeArtifact = {mj2x::NativeSourceLanguage::Cpp17, cppSource, "native.cpp"};
    }
    if (!options.llvmOutputPath.empty() &&
        nativeArtifact.language != mj2x::NativeSourceLanguage::LlvmIr) {
        fprintf(stderr, "[mj2x] ERROR: --emit-llvm requires an LLVM IR backend source artifact.\n");
        return 1;
    }

    if (!options.cmakeProjectDir.empty()) {
        std::filesystem::path cmakeCrtRoot;
        if (options.cBackendKind == CEmitterBackend::RegC) {
            cmakeCrtRoot = mj2x::cli::findCrtRoot(argv[0]);
            if (cmakeCrtRoot.empty()) {
                fprintf(stderr,
                        "[mj2x] ERROR: RegC CMake project requires an mj2x-crt source pack; "
                        "set MJ2X_CRT_DIR.\n");
                return 1;
            }
        }
        int projectRc =
            generateCMakeProject(options.cmakeProjectDir, nativeOutputPath, cmakeCrtRoot);
        if (projectRc != 0) return projectRc;
    }

    if (!options.xmakeProjectDir.empty()) {
        mj2x::cli::XmakeProjectConfig projectConfig;
        projectConfig.projectDir          = options.xmakeProjectDir;
        projectConfig.generatedSourcePath = nativeOutputPath;
        projectConfig.crtRoot             = mj2x::cli::findCrtRoot(argv[0]);
        projectConfig.projectName         = "mj2x_native_project";
        int projectRc                     = mj2x::cli::generateXmakeProject(projectConfig);
        if (projectRc != 0) return projectRc;
    }

    if (options.compileNative) {
        int compileRc = 1;
        if (options.buildDriver == NativeBuildDriver::Xmake) {
            mj2x::cli::XmakeBuildConfig buildConfig;
            buildConfig.projectDir        = options.xmakeProjectDir;
            buildConfig.outputLibraryPath = nativeLibraryPath;
            compileRc                     = mj2x::cli::buildXmakeProject(buildConfig);
        }
        else {
            std::filesystem::path crtRoot;
            if (options.cBackendKind == CEmitterBackend::RegC) {
                crtRoot = mj2x::cli::findCrtRoot(argv[0]);
                if (crtRoot.empty()) {
                    fprintf(stderr,
                            "[mj2x] ERROR: RegC --compile requires an mj2x-crt source pack; set "
                            "MJ2X_CRT_DIR or use --generate-xmake.\n");
                    return 1;
                }
            }
            if (options.cBackendKind == CEmitterBackend::LlvmIr) {
                compileRc = mj2x::cli::compileLlvmIrSource(
                    nativeOutputPath, options.outputPath, options.llvmTool);
            }
            else if (options.compileMode == mj2x::cli::CompileMode::Pipe) {
                if (nativeSourceBackend == nullptr) {
                    fprintf(stderr, "[mj2x] ERROR: native source backend is unavailable.\n");
                    return 1;
                }
                if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
                    fprintf(stderr,
                            "[mj2x] ERROR: --compile-mode pipe only supports C99 native "
                            "source artifacts.\n");
                    return 1;
                }
                compileRc = mj2x::cli::compileCSourceFromPipe(
                    nativeArtifact.content, options.outputPath, options.cCompilerKind, crtRoot);
            }
            else {
                // 多文件模式（--emit-native-dir）：编译目录内全部 .c。
                const std::string compileSource =
                    nativeDirMode ? options.nativeDirOutput : nativeOutputPath;
                compileRc = nativeArtifact.language == mj2x::NativeSourceLanguage::Cpp17
                                ? mj2x::cli::compileCppSource(
                                      compileSource,
                                      options.outputPath,
                                      options.cCompilerKind,
                                      crtRoot)
                                : mj2x::cli::compileCSource(
                                      compileSource,
                                      options.outputPath,
                                      options.cCompilerKind,
                                      crtRoot);
            }
        }
        if (compileRc != 0) return compileRc;
        if (jarNativeLoaderEnabled)
            return embedNativeLibraryInJar(
                options.outputPath, nativeResourceEntry, nativeLibraryPath);
    }

    return 0;
}

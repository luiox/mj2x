#include "mj2x/mj2x.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/multianewarray_preprocessor.hpp"
#include "mj2x/preprocessor/native_annotation_preprocessor.hpp"
#include "mj2x/preprocessor/native_method_prefix_preprocessor.hpp"
#include "mj2x/postprocessor/native_loader_postprocessor.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "config_file.hpp"
#include "native_compiler.hpp"
#include "native_project_generator.hpp"
#include "jzip/memory_segment.hpp"
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
    printf("  --emit-cpp <file>          Reserved C++17 source output (planned)\n");
    printf("  --backend <stackc|regc>    Select C emitter when --emit-c is used (default: stackc; "
           "cbackend is a legacy alias)\n");
    printf(
        "  --native-annotation <desc> Only compile methods annotated with JVM descriptor desc\n");
    printf("  --native-method-prefix <p> Only compile methods whose owner.name starts with p; "
           "repeatable\n");
    printf(
        "  --compile                  Compile emitted C into a native library beside --output\n");
    printf("  --compile-mode <mode>      Select native compile input mode (file, pipe)\n");
    printf("  --c-compiler <name>        C compiler or build driver (msvc, gcc, clang, zig, xmake)\n");
    printf("  --generate-cmake <dir>     Generate a standalone CMake project for emitted C\n");
    printf("  --generate-xmake <dir>     Generate a standalone xmake project with mj2x-crt\n");
    printf("  --dump-mir                 Dump MIR intermediate representation\n");
    printf("  --help                     Print help\n");
}

enum class CEmitterBackend
{
    StackC,
    RegC,
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
    std::string              cmakeProjectDir;
    std::string              xmakeProjectDir;
    std::string              nativeAnnotation;
    std::vector<std::string> nativeMethodPrefixes;
    CEmitterBackend          cBackendKind     = CEmitterBackend::StackC;
    mj2x::cli::CCompilerKind cCompilerKind    = mj2x::cli::CCompilerKind::Auto;
    mj2x::cli::CompileMode   compileMode      = mj2x::cli::CompileMode::File;
    NativeBuildDriver        buildDriver      = NativeBuildDriver::Direct;
    bool                     backendSelected  = false;
    bool                     compileNative    = false;
    bool                     compileModeSelected = false;
    bool                     compilerSelected = false;
    bool                     dumpMIR          = false;
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
    return false;
}

static bool applyConfigOptions(const mj2x::cli::ConfigFileOptions& config, CliOptions& options,
                               std::string& error)
{
    if (config.input) options.inputPath = *config.input;
    if (config.output) options.outputPath = *config.output;
    if (config.emitC) options.cOutputPath = *config.emitC;
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
    if (config.generateXmake) options.xmakeProjectDir = *config.generateXmake;
    if (config.generateCmake) options.cmakeProjectDir = *config.generateCmake;
    if (config.dumpMir) options.dumpMIR = *config.dumpMir;
    if (config.backend) {
        options.backendSelected = true;
        if (!parseBackendName(*config.backend, options.cBackendKind)) {
            error = "Unknown config backend value. Expected stackc or regc.";
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

static int generateCMakeProject(const std::string& projectDirValue, const std::string& cOutputPath)
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

    fs::path sourcePath(cOutputPath);
    if (!fs::exists(sourcePath, ec)) {
        fprintf(stderr,
                "[mj2x] ERROR: emitted C source does not exist: %s\n",
                sourcePath.string().c_str());
        return 1;
    }

    fs::path expectedSource = projectDir / "native.c";
    if (fs::absolute(sourcePath, ec) != fs::absolute(expectedSource, ec)) {
        fs::copy_file(sourcePath, expectedSource, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr,
                    "[mj2x] ERROR: cannot copy emitted C into CMake project: %s\n",
                    ec.message().c_str());
            return 1;
        }
    }

    {
        std::ofstream out(projectDir / "CMakeLists.txt", std::ios::binary);
        if (!out) {
            fprintf(stderr, "[mj2x] ERROR: cannot write CMakeLists.txt\n");
            return 1;
        }
        out << "cmake_minimum_required(VERSION 3.16)\n";
        out << "project(mj2x_native C)\n\n";
        out << "find_package(JNI REQUIRED)\n\n";
        out << "add_library(mj2x_native SHARED native.c)\n";
        out << "target_include_directories(mj2x_native PRIVATE ${JNI_INCLUDE_DIRS})\n";
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
    CliOptions               options;
    std::string              configPath;
    TemporaryProjectCleanup temporaryProjectCleanup;

    bool commandLineNativePrefixesSeen = false;
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
        else if (arg == "--emit-c" && i + 1 < argc)
            options.cOutputPath = argv[++i];
        else if (arg == "--emit-cpp" && i + 1 < argc)
            options.cppOutputPath = argv[++i];
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
        else if (arg == "--backend" && i + 1 < argc) {
            options.backendSelected = true;
            if (!parseBackendName(argv[++i], options.cBackendKind)) {
                fprintf(stderr,
                        "Unknown --backend value. Expected stackc or regc. Legacy alias cbackend "
                        "is also accepted.\n");
                printUsage();
                return 1;
            }
        }
        else if (arg == "--dump-mir")
            options.dumpMIR = true;
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

    if (!options.cOutputPath.empty() && !options.cppOutputPath.empty()) {
        fprintf(stderr, "--emit-c and --emit-cpp are mutually exclusive.\n");
        return 1;
    }
    if (!options.cppOutputPath.empty()) {
        fprintf(stderr,
                "--emit-cpp is reserved for the planned C++17 backend and is not implemented yet.\n");
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
        (!options.cOutputPath.empty() || !options.cmakeProjectDir.empty() ||
         !options.xmakeProjectDir.empty())) {
        fprintf(stderr,
                "--compile-mode pipe cannot be combined with --emit-c or generated native "
                "projects.\n");
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && options.cOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.cmakeProjectDir) / "native.c").string();
    }

    if (!options.xmakeProjectDir.empty() && options.cOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.xmakeProjectDir) / "src" / "generated" / "native.c")
                .string();
    }

    const bool pipeCompile =
        options.compileNative && options.compileMode == mj2x::cli::CompileMode::Pipe;

    if (options.backendSelected && options.cOutputPath.empty() && !pipeCompile) {
        fprintf(stderr, "--backend requires --emit-c or a generated native project.\n");
        printUsage();
        return 1;
    }

    if (options.compileNative && options.cOutputPath.empty() && !pipeCompile) {
        fprintf(stderr, "--compile requires --emit-c or a generated native project.\n");
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

    const bool needsNativeSource = !options.cOutputPath.empty() || pipeCompile;
    mj2x::Backend* nativeSourceBackend = nullptr;

    if (needsNativeSource) {
        if (options.cBackendKind == CEmitterBackend::RegC) {
            auto regcBackend = std::make_unique<mj2x::RegCBackend>();
            if (!options.cOutputPath.empty()) regcBackend->setOutputPath(options.cOutputPath);
            nativeSourceBackend = regcBackend.get();
            // RegC 的 native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责。
            pipeline.registerBackend(std::move(regcBackend));
        }
        else {
            auto stackCBackend = std::make_unique<mj2x::StackCBackend>();
            if (!options.cOutputPath.empty()) stackCBackend->setOutputPath(options.cOutputPath);
            nativeSourceBackend = stackCBackend.get();
            if (jarNativeLoaderEnabled) {
                stackCBackend->setJarNativeLoaderResourcePath(
                    nativeResourcePath, nativeTempSuffix.empty() ? ".mj2x" : nativeTempSuffix);
            }
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
        pipeline.registerPreprocessor(std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    }
    if (needsNativeSource && options.cBackendKind == CEmitterBackend::StackC) {
        pipeline.registerPostprocessor(std::make_unique<mj2x::CLoaderPostprocessor>());
    }
    if (jarNativeLoaderEnabled) {
        // RegC 由 postprocessor 注入 clinit load 调用；StackC 因 RegisterNatives
        // 绑定需自己重写 clinit，此处只注入资源（injectClinitCalls=false）。
        const bool regcInjectsClinit = needsNativeSource &&
                                        options.cBackendKind == CEmitterBackend::RegC;
        pipeline.registerPostprocessor(std::make_unique<mj2x::NativeLoaderPostprocessor>(
            nativeResourcePath,
            nativeTempSuffix.empty() ? ".mj2x" : nativeTempSuffix,
            regcInjectsClinit));
    }

    int rc = pipeline.run();
    if (rc != 0) return rc;

    if (!options.cmakeProjectDir.empty()) {
        int projectRc = generateCMakeProject(options.cmakeProjectDir, options.cOutputPath);
        if (projectRc != 0) return projectRc;
    }

    if (!options.xmakeProjectDir.empty()) {
        mj2x::cli::XmakeProjectConfig projectConfig;
        projectConfig.projectDir          = options.xmakeProjectDir;
        projectConfig.generatedSourcePath = options.cOutputPath;
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
            if (options.compileMode == mj2x::cli::CompileMode::Pipe) {
                if (nativeSourceBackend == nullptr) {
                    fprintf(stderr, "[mj2x] ERROR: native source backend is unavailable.\n");
                    return 1;
                }
                const auto artifact = nativeSourceBackend->generatedSourceArtifact();
                if (artifact.language != mj2x::NativeSourceLanguage::C99) {
                    fprintf(stderr,
                            "[mj2x] ERROR: --compile-mode pipe only supports C99 native "
                            "source artifacts.\n");
                    return 1;
                }
                compileRc = mj2x::cli::compileCSourceFromPipe(
                    artifact.content, options.outputPath, options.cCompilerKind, crtRoot);
            }
            else {
                compileRc = mj2x::cli::compileCSource(
                    options.cOutputPath, options.outputPath, options.cCompilerKind, crtRoot);
            }
        }
        if (compileRc != 0) return compileRc;
        if (jarNativeLoaderEnabled)
            return embedNativeLibraryInJar(
                options.outputPath, nativeResourceEntry, nativeLibraryPath);
    }

    return 0;
}

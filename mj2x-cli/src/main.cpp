#include "mj2x/mj2x.hpp"
#include "mj2x/backend/cbackend/c_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/multianewarray_preprocessor.hpp"
#include "mj2x/preprocessor/native_annotation_preprocessor.hpp"
#include "mj2x/postprocessor/native_loader_postprocessor.hpp"
#include "native_compiler.hpp"
#include "jzip/memory_segment.hpp"
#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static void printUsage() {
    printf("Usage: mj2x-cli -i <input.class|input.jar> -o <output.class|output.jar> [options]\n");
    printf("Options:\n");
    printf("  --emit-c <file>            Output C source to file\n");
    printf("  --backend <cbackend|regc>  Select C emitter when --emit-c is used (default: cbackend)\n");
    printf("  --native-annotation <desc> Only compile methods annotated with JVM descriptor desc\n");
    printf("  --compile                  Compile emitted C into a native library beside --output\n");
    printf("  --c-compiler <name>        Override C compiler detection (msvc, gcc, clang)\n");
    printf("  --generate-cmake <dir>     Generate a standalone CMake project for emitted C\n");
    printf("  --dump-mir                 Dump MIR intermediate representation\n");
    printf("  --help                     Print help\n");
}

enum class CEmitterBackend {
    CBackend,
    RegC,
};

static bool parseBackendName(const std::string& name, CEmitterBackend& backend) {
    if (name == "cbackend" || name == "c") {
        backend = CEmitterBackend::CBackend;
        return true;
    }
    if (name == "regc") {
        backend = CEmitterBackend::RegC;
        return true;
    }
    return false;
}

static bool hasExtension(std::string path, const std::string& ext) {
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    for (char& ch : suffix)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return suffix == ext;
}

static std::vector<ca::u8> readFileBytes(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto size = ifs.tellg();
    std::vector<ca::u8> bytes(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(
    const std::string& name,
    const std::vector<ca::u8>& data) {
    auto local = std::make_unique<jzip::LocalFileHeader>();
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

static int embedNativeLibraryInJar(
    const std::string& jarPath,
    const std::string& entryName,
    const std::filesystem::path& nativeLibraryPath) {
    auto jarBytes = readFileBytes(jarPath);
    auto nativeBytes = readFileBytes(nativeLibraryPath);
    if (jarBytes.empty() || nativeBytes.empty()) {
        fprintf(stderr, "[mj2x] ERROR: cannot embed native library into output JAR.\n");
        return 1;
    }

    auto original = jzip::ZipIO::readJvm(jzip::MemorySegment(jarBytes));
    jzip::ZipArchive rewritten;
    bool replaced = false;
    for (const auto* local : original.getLocalFiles()) {
        const auto& name = local->getFileName();
        auto data = name == entryName ? nativeBytes : jzip::ZipIO::decompressLocalFile(*local);
        if (name == entryName) replaced = true;
        rewritten.addPart(makeStoredEntry(name, data));
    }
    if (!replaced)
        rewritten.addPart(makeStoredEntry(entryName, nativeBytes));

    auto outBytes = jzip::ZipIO::writeDirect(rewritten);
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

static bool isNativeAnnotationDescriptor(const std::string& descriptor) {
    return descriptor.size() >= 3 && descriptor.front() == 'L' &&
           descriptor.back() == ';' && descriptor.find('.') == std::string::npos;
}

static int generateCMakeProject(const std::string& projectDirValue, const std::string& cOutputPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path projectDir(projectDirValue);
    fs::create_directories(projectDir, ec);
    if (ec) {
        fprintf(stderr, "[mj2x] ERROR: cannot create CMake project directory: %s\n",
                projectDir.string().c_str());
        return 1;
    }

    fs::path sourcePath(cOutputPath);
    if (!fs::exists(sourcePath, ec)) {
        fprintf(stderr, "[mj2x] ERROR: emitted C source does not exist: %s\n",
                sourcePath.string().c_str());
        return 1;
    }

    fs::path expectedSource = projectDir / "native.c";
    if (fs::absolute(sourcePath, ec) != fs::absolute(expectedSource, ec)) {
        fs::copy_file(sourcePath, expectedSource, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr, "[mj2x] ERROR: cannot copy emitted C into CMake project: %s\n",
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

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    std::string cOutputPath;
    std::string cmakeProjectDir;
    std::string nativeAnnotation;
    CEmitterBackend cBackendKind = CEmitterBackend::CBackend;
    mj2x::cli::CCompilerKind cCompilerKind = mj2x::cli::CCompilerKind::Auto;
    bool backendSelected = false;
    bool compileNative = false;
    bool compilerSelected = false;
    bool dumpMIR = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--emit-c" && i + 1 < argc) cOutputPath = argv[++i];
        else if (arg == "--native-annotation" && i + 1 < argc) nativeAnnotation = argv[++i];
        else if (arg == "--compile") compileNative = true;
        else if (arg == "--generate-cmake" && i + 1 < argc) cmakeProjectDir = argv[++i];
        else if (arg == "--c-compiler" && i + 1 < argc) {
            compilerSelected = true;
            if (!mj2x::cli::parseCompilerName(argv[++i], cCompilerKind)) {
                fprintf(stderr, "Unknown --c-compiler value. Expected msvc, gcc, or clang.\n");
                printUsage();
                return 1;
            }
        }
        else if (arg == "--backend" && i + 1 < argc) {
            backendSelected = true;
            if (!parseBackendName(argv[++i], cBackendKind)) {
                fprintf(stderr, "Unknown --backend value. Expected cbackend or regc.\n");
                printUsage();
                return 1;
            }
        }
        else if (arg == "--dump-mir") dumpMIR = true;
        else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            printUsage();
            return 1;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        printUsage();
        return 1;
    }

    if (!nativeAnnotation.empty() && !isNativeAnnotationDescriptor(nativeAnnotation)) {
        fprintf(stderr,
                "--native-annotation must be a JVM annotation descriptor like Lcom/example/Native;.\n");
        printUsage();
        return 1;
    }

    if (backendSelected && cOutputPath.empty()) {
        fprintf(stderr, "--backend requires --emit-c because it selects the C emitter.\n");
        printUsage();
        return 1;
    }

    if (compileNative && cOutputPath.empty()) {
        fprintf(stderr, "--compile requires --emit-c because it compiles emitted C source.\n");
        printUsage();
        return 1;
    }

    if (!cmakeProjectDir.empty() && cOutputPath.empty()) {
        cOutputPath = (std::filesystem::path(cmakeProjectDir) / "native.c").string();
    }

    if (!cmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cmakeProjectDir, ec);
        if (ec) {
            fprintf(stderr, "[mj2x] ERROR: cannot create CMake project directory: %s\n",
                    cmakeProjectDir.c_str());
            return 1;
        }
    }

    if (compilerSelected && !compileNative) {
        fprintf(stderr, "--c-compiler requires --compile.\n");
        printUsage();
        return 1;
    }

    mj2x::PipelineConfig config;
    config.inputPath = inputPath;
    config.outputPath = outputPath;
    config.dumpMIR = dumpMIR;

    const bool jarNativeLoaderEnabled =
        compileNative &&
        cBackendKind == CEmitterBackend::CBackend &&
        hasExtension(inputPath, ".jar") &&
        hasExtension(outputPath, ".jar");
    std::filesystem::path nativeLibraryPath =
        mj2x::cli::nativeLibraryPathForOutput(outputPath);
    const std::string nativeResourceEntry =
        "META-INF/mj2x/native/" + nativeLibraryPath.filename().string();
    const std::string nativeResourcePath = nativeResourceEntry;
    const std::string nativeTempSuffix = nativeLibraryPath.extension().string();

    mj2x::Pipeline pipeline(std::move(config));

    if (!cOutputPath.empty()) {
        if (cBackendKind == CEmitterBackend::RegC) {
            auto regcBackend = std::make_unique<mj2x::RegCBackend>();
            regcBackend->setOutputPath(cOutputPath);
            pipeline.registerBackend(std::move(regcBackend));
        } else {
            auto cBackend = std::make_unique<mj2x::CBackend>();
            cBackend->setOutputPath(cOutputPath);
            if (jarNativeLoaderEnabled) {
                cBackend->setJarNativeLoaderResourcePath(
                    nativeResourcePath,
                    nativeTempSuffix.empty() ? ".mj2x" : nativeTempSuffix);
            }
            pipeline.registerBackend(std::move(cBackend));
        }
    }

    // Bytecode-shaping passes run before MarkAll so every selected method sees
    // the same normalized L1 input regardless of the chosen native backend.
    pipeline.registerPreprocessor(std::make_unique<mj2x::LdcPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::MultiANewArrayPreprocessor>());
    if (!nativeAnnotation.empty()) {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::NativeAnnotationPreprocessor>(nativeAnnotation));
    } else {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    }
    if (jarNativeLoaderEnabled) {
        pipeline.registerPostprocessor(
            std::make_unique<mj2x::NativeLoaderPostprocessor>(nativeResourcePath));
    }

    int rc = pipeline.run();
    if (rc != 0)
        return rc;

    if (!cmakeProjectDir.empty()) {
        int projectRc = generateCMakeProject(cmakeProjectDir, cOutputPath);
        if (projectRc != 0)
            return projectRc;
    }

    if (compileNative) {
        int compileRc = mj2x::cli::compileCSource(cOutputPath, outputPath, cCompilerKind);
        if (compileRc != 0)
            return compileRc;
        if (jarNativeLoaderEnabled)
            return embedNativeLibraryInJar(outputPath, nativeResourceEntry, nativeLibraryPath);
    }

    return 0;
}


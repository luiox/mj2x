#include "mj2x/mj2x.hpp"
#include "mj2x/logging.hpp"
#include "mj2x/backend/llvmir/llvm_ir_backend.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "mj2x/core/seed_derivation.hpp"
#include "mj2x/core/user_intrinsics.hpp"
#include "mj2x/core/user_intrinsics.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
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
#include "param_registry.hpp"
#include "jzip/memory_segment.hpp"
#include "typed/cast.hpp"
#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"

#include "libca/log/log_macros.hpp"
#include "libca/process/subprocess.hpp"

#ifdef _WIN32
// subprocess.hpp 为保护 Command 同名成员标识符 #undef 了全局 stdout/stderr
// 宏；恢复 MSVC CRT 语义（本文件 setvbuf 等 <cstdio> 接口需要）。
#    include <cstdio>
#    define stdout (__acrt_iob_func(1))
#    define stderr (__acrt_iob_func(2))
#endif

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// 参数容器/后端枚举已迁入参数注册表（param_registry.hpp）。
using mj2x::cli::CEmitterBackend;
using mj2x::cli::CliOptions;
using mj2x::cli::NativeBuildDriver;

static void printUsage()
{
    mj2x::cli::printHelp();
}

class TemporaryProjectCleanup
{
public:
    ~TemporaryProjectCleanup()
    {
        if (path_.empty()) return;
        if (removeAll(path_)) return;
        CA_LOGT_WARN("cli", "cannot clean temporary xmake project: {}\n", path_.string().c_str());
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

/// @brief --runtime-protection 字符串 → codegen::RuntimeProtectionAction。
static mj2x::codegen::RuntimeProtectionAction parseRuntimeProtectionAction(
    const std::string& name)
{
    if (name == "log") return mj2x::codegen::RuntimeProtectionAction::Log;
    if (name == "delay") return mj2x::codegen::RuntimeProtectionAction::Delay;
    if (name == "fake") return mj2x::codegen::RuntimeProtectionAction::Fake;
    if (name == "terminate") return mj2x::codegen::RuntimeProtectionAction::Terminate;
    return mj2x::codegen::RuntimeProtectionAction::None;
}

static bool hasExtension(std::string path, const std::string& ext)
{
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    for (char& ch : suffix) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return suffix == ext;
}

/// @brief 加载用户自定义 intrinsic 配置 JSON（issue #818）。
///
/// @details 格式：
/// {"intrinsics":[{"owner":"com/x/F","name":"m","desc":"(II)I","symbol":"my_m",
///                 "passContext":false,"mayThrow":false}],
///  "sources":["path/to/my_m.c"]}
/// owner/name/desc/symbol 必填；passContext/mayThrow 可选（默认 false）。
static bool loadUserIntrinsicConfig(const std::string& path, mj2x::UserIntrinsicConfig& out,
                                    std::string& error)
{
    using json = nlohmann::json;
    json root;
    try {
        std::ifstream file(path);
        if (!file) {
            error = "cannot open intrinsic config: " + path;
            return false;
        }
        file >> root;
    }
    catch (const json::exception& ex) {
        error = std::string("invalid JSON intrinsic config: ") + ex.what();
        return false;
    }
    if (!root.is_object()) {
        error = "intrinsic config root must be a JSON object";
        return false;
    }
    if (root.contains("intrinsics")) {
        const auto& arr = root["intrinsics"];
        if (!arr.is_array()) {
            error = "intrinsics must be a JSON array";
            return false;
        }
        for (const auto& item : arr) {
            if (!item.is_object()) {
                error = "intrinsic entry must be a JSON object";
                return false;
            }
            mj2x::UserIntrinsic u;
            auto               getStr = [&](const char* key, std::string& field) -> bool {
                if (!item.contains(key) || !item[key].is_string()) return false;
                field = item[key].get<std::string>();
                return true;
            };
            if (!getStr("owner", u.owner) || !getStr("name", u.name) ||
                !getStr("desc", u.desc) || !getStr("symbol", u.symbol)) {
                error = "intrinsic entry requires owner/name/desc/symbol strings";
                return false;
            }
            if (item.contains("passContext") && item["passContext"].is_boolean())
                u.passContext = item["passContext"].get<bool>();
            if (item.contains("mayThrow") && item["mayThrow"].is_boolean())
                u.mayThrow = item["mayThrow"].get<bool>();
            out.intrinsics.push_back(std::move(u));
        }
    }
    if (root.contains("sources")) {
        const auto& arr = root["sources"];
        if (!arr.is_array()) {
            error = "sources must be a JSON array";
            return false;
        }
        for (const auto& s : arr) {
            if (!s.is_string()) {
                error = "source entry must be a string";
                return false;
            }
            out.sources.push_back(s.get<std::string>());
        }
    }
    return true;
}

/// @brief 把用户 intrinsic 实现源文件复制进生成工程（xmake 模板用 glob 编译
/// `src/mj2x-crt/**.c`，故复制到 `src/mj2x-crt/user_intrinsics/` 即自动并入）。
static bool copyUserIntrinsicSources(const std::vector<std::string>& sources,
                                     const std::filesystem::path& projectDir, std::string& error)
{
    if (sources.empty()) return true;
    const std::filesystem::path destDir = projectDir / "src" / "mj2x-crt" / "user_intrinsics";
    std::error_code             ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        error = "cannot create user intrinsic dir: " + destDir.string();
        return false;
    }
    for (const auto& src : sources) {
        const std::filesystem::path from(src);
        const std::filesystem::path to = destDir / from.filename();
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy user intrinsic source: " + from.string();
            return false;
        }
    }
    return true;
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

/// @brief --native-lib-pack-hook：嵌入前外部处理钩子（sepacker DLL 加壳等，
/// 主线第 6 项的代码级预留接口）。
///
/// `{in}` 占位符替换为编译产物路径；hook 应就地改写该文件（加壳/签名等），
/// 随后的嵌入（可选 --native-lib-key 加密）使用处理后的产物。经系统 shell
/// 执行（Windows: ComSpec /d /c；POSIX: sh -c），非零退出码视为失败。
static int runNativeLibPackHook(const std::string&    hookTemplate,
                                const std::filesystem::path& nativeLibraryPath)
{
    std::string commandLine = hookTemplate;
    const std::string marker  = "{in}";
    const std::string libPath = nativeLibraryPath.string();
    std::size_t       pos     = 0;
    while ((pos = commandLine.find(marker, pos)) != std::string::npos) {
        commandLine.replace(pos, marker.size(), libPath);
        pos += libPath.size();
    }

#ifdef _WIN32
    const char* shellEnv = std::getenv("ComSpec");
    const std::string shellProgram = shellEnv != nullptr ? shellEnv : "cmd.exe";
#else
    const std::string shellProgram = "/bin/sh";
#endif
    ca::process::Command command(shellProgram);
#ifdef _WIN32
    command.arg("/d").arg("/c").arg(commandLine);
#else
    command.arg("-c").arg(commandLine);
#endif
    auto completed = command.output();
    CA_LOGT_INFO("cli", "native pack hook command line: {}\n", commandLine.c_str());
    if (completed.is_err()) {
        CA_LOGT_ERROR("cli", "cannot start native pack hook: {}\n",
                      completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    if (!output.status.success()) {
        CA_LOGT_ERROR("cli", "native pack hook failed with exit code {}\n", output.status.code);
        if (!output.stdout_data.empty()) {
            fprintf(stderr, "%s", output.stdout_data.c_str());
        }
        if (!output.stderr_data.empty()) {
            fprintf(stderr, "%s", output.stderr_data.c_str());
        }
        return 1;
    }
    return 0;
}

static int embedNativeLibraryInJar(const std::string& jarPath, const std::string& entryName,
                                   const std::filesystem::path& nativeLibraryPath,
                                   std::uint32_t xorKey, bool encryptEntry)
{
    auto jarBytes    = readFileBytes(jarPath);
    auto nativeBytes = readFileBytes(nativeLibraryPath);
    if (jarBytes.empty() || nativeBytes.empty()) {
        CA_LOGT_ERROR("cli", "cannot embed native library into output JAR.\n");
        return 1;
    }
    if (encryptEntry) {
        mj2x::xorStreamBytes(nativeBytes.data(), nativeBytes.size(), xorKey);
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
        CA_LOGT_ERROR("cli", "cannot rewrite output JAR: {}\n", jarPath.c_str());
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(outBytes.data()),
              static_cast<std::streamsize>(outBytes.size()));
    printf("Embedded native library in JAR: %s\n", entryName.c_str());
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
        CA_LOGT_ERROR("cli", "cannot create CMake project directory: {}\n", projectDir.string().c_str());
        return 1;
    }

    fs::path sourcePath(nativeOutputPath);
    if (!fs::exists(sourcePath, ec)) {
        CA_LOGT_ERROR("cli", "emitted native source does not exist: {}\n", sourcePath.string().c_str());
        return 1;
    }

    const bool isCppSource = hasExtension(sourcePath.string(), ".cpp");
    fs::path    expectedSource = projectDir / sourcePath.filename();
    if (fs::absolute(sourcePath, ec) != fs::absolute(expectedSource, ec)) {
        fs::copy_file(sourcePath, expectedSource, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "cannot copy emitted C into CMake project: {}\n", ec.message().c_str());
            return 1;
        }
    }

    const bool needsCrt = !crtRoot.empty();
    if (needsCrt && !copyCrtForCMakeProject(projectDir, crtRoot)) {
        CA_LOGT_ERROR("cli", "cannot copy mj2x-crt into CMake project.\n");
        return 1;
    }

    {
        std::ofstream out(projectDir / "CMakeLists.txt", std::ios::binary);
        if (!out) {
            CA_LOGT_ERROR("cli", "cannot write CMakeLists.txt\n");
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
            CA_LOGT_ERROR("cli", "cannot write .gitignore\n");
            return 1;
        }
        out << "build/\n";
        out << "cmake-build-*/\n";
        out << "*.obj\n*.o\n*.dll\n*.so\n*.dylib\n";
    }

    {
        std::ofstream out(projectDir / "README.md", std::ios::binary);
        if (!out) {
            CA_LOGT_ERROR("cli", "cannot write README.md\n");
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

    printf("CMake project written: %s\n", projectDir.string().c_str());
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

    // 日志门面（libca log + SimpleLogBackend）：默认 Info；--quiet/--verbose
    // 在第一遍选项扫描后调整级别（init 前的参数校验错误用 fprintf 兜底）。
    mj2x::initLogging();

    CliOptions               options;
    std::string              configPath;
    TemporaryProjectCleanup temporaryProjectCleanup;

    // 第一遍扫描：--config 路径与日志级别（CLI 显式优先于配置）。
    bool cliQuiet   = false;
    bool cliVerbose = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                CA_LOGT_ERROR("cli", "--config requires a JSON file path.\n");
                printUsage();
                return 1;
            }
            if (!configPath.empty()) {
                CA_LOGT_ERROR("cli", "--config may only be specified once.\n");
                printUsage();
                return 1;
            }
            configPath = argv[++i];
        }
        else if (arg == "--quiet")
            cliQuiet = true;
        else if (arg == "--verbose")
            cliVerbose = true;
    }
    if (cliQuiet) mj2x::setLogLevel(ca::log::Level::Warn);
    else if (cliVerbose) mj2x::setLogLevel(ca::log::Level::Debug);

    // 配置文件：注册表驱动加载（默认值 < 配置 < CLI 由解析顺序保证）。
    if (!configPath.empty()) {
        std::string error;
        if (!mj2x::cli::loadConfigFile(configPath, options, error)) {
            CA_LOGT_ERROR("cli", "{}\n", error.c_str());
            return 1;
        }
        if (!cliQuiet && !cliVerbose) {
            if (options.quiet) mj2x::setLogLevel(ca::log::Level::Warn);
            else if (options.verbose) mj2x::setLogLevel(ca::log::Level::Debug);
        }
    }

    // 第二遍：注册表驱动参数解析（--config 第一遍已消费；--quiet/--verbose
    // 走注册表 log_quiet/log_verbose，最终日志级别按 CLI 优先）。
    bool         dumpSchema   = false;
    bool         dumpTemplate = false;
    std::string  dumpFile;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            ++i;   // 第一遍已记录路径；这里跳过其值。
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--dump-config-schema" || arg == "--dump-config-template") {
            if (dumpSchema || dumpTemplate) {
                CA_LOGT_ERROR("cli",
                              "--dump-config-schema/--dump-config-template may only be "
                              "specified once.\n");
                return 1;
            }
            dumpSchema   = (arg == "--dump-config-schema");
            dumpTemplate = !dumpSchema;
            if (i + 1 < argc && argv[i + 1][0] != '-') dumpFile = argv[++i];
            continue;
        }
        const mj2x::cli::ParamDef* param = mj2x::cli::findParamByCli(arg);
        if (!param) {
            CA_LOGT_ERROR("cli", "unknown option: {}\n", arg.c_str());
            printUsage();
            return 1;
        }
        const char* value = nullptr;
        if (param->type != mj2x::cli::ParamType::Bool) {
            if (i + 1 >= argc) {
                CA_LOGT_ERROR("cli", "{} requires a value.\n", arg.c_str());
                printUsage();
                return 1;
            }
            value = argv[++i];
        }
        std::string error;
        if (!param->parseCli(options, value, error)) {
            CA_LOGT_ERROR("cli", "{}\n", error.c_str());
            printUsage();
            return 1;
        }
    }

    // --dump-config-schema / --dump-config-template：不要求 -i/-o，输出后退出。
    if (dumpSchema || dumpTemplate) {
        const std::string text = dumpSchema
                                     ? mj2x::cli::buildConfigSchemaJson().dump(2)
                                     : mj2x::cli::buildConfigTemplateText();
        if (dumpFile.empty()) {
            printf("%s\n", text.c_str());
        }
        else {
            std::ofstream out(dumpFile, std::ios::binary);
            if (!out) {
                CA_LOGT_ERROR("cli", "cannot write {}: {}\n",
                              dumpSchema ? "config schema" : "config template",
                              dumpFile.c_str());
                return 1;
            }
            out << text << "\n";
        }
        return 0;
    }

    if (options.inputPath.empty() || options.outputPath.empty()) {
        printUsage();
        return 1;
    }

    const int nativeOutputCount = static_cast<int>(!options.cOutputPath.empty()) +
                                  static_cast<int>(!options.cppOutputPath.empty()) +
                                  static_cast<int>(!options.llvmOutputPath.empty());
    if (nativeOutputCount > 1) {
        CA_LOGT_ERROR("cli", "--emit-c, --emit-cpp, and --emit-llvm are mutually exclusive.\n");
        return 1;
    }
    if (!options.cppOutputPath.empty() && !hasExtension(options.cppOutputPath, ".cpp")) {
        CA_LOGT_ERROR("cli", "--emit-cpp requires a .cpp output path.\n");
        return 1;
    }
    if (!options.cppOutputPath.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "--emit-cpp currently requires --backend regc.\n");
        return 1;
    }
    if (!options.llvmOutputPath.empty() && !hasExtension(options.llvmOutputPath, ".ll")) {
        CA_LOGT_ERROR("cli", "--emit-llvm requires a .ll output path.\n");
        return 1;
    }
    if (!options.llvmOutputPath.empty() && options.cBackendKind != CEmitterBackend::LlvmIr) {
        CA_LOGT_ERROR("cli", "--emit-llvm requires --backend llvm.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.llvmOutputPath.empty() &&
        options.compileMode != mj2x::cli::CompileMode::Pipe) {
        CA_LOGT_ERROR("cli", "--backend llvm requires --emit-llvm.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr &&
        options.compileMode == mj2x::cli::CompileMode::Pipe) {
        CA_LOGT_ERROR("cli", "--backend llvm does not support --compile-mode pipe.\n");
        return 1;
    }
    if (options.llvmToolSelected && options.cBackendKind != CEmitterBackend::LlvmIr) {
        CA_LOGT_ERROR("cli", "--llvm-tool requires --backend llvm.\n");
        return 1;
    }
    if (options.regcTraceCalls && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "--regc-trace-calls requires --backend regc.\n");
        return 1;
    }
    if ((options.regcSafeEnv || options.regcSafeEnvDebug) &&
        options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "--regc-safe-env requires --backend regc.\n");
        return 1;
    }
    if (options.runtimeProtection != "none" && options.runtimeProtection != "log" &&
        options.runtimeProtection != "delay" && options.runtimeProtection != "fake" &&
        options.runtimeProtection != "terminate") {
        CA_LOGT_ERROR("cli",
                      "Unknown --runtime-protection value. Expected none, log, delay, fake, or "
                      "terminate.\n");
        return 1;
    }
    if (options.runtimeProtection != "none" && options.cBackendKind != CEmitterBackend::RegC &&
        options.cBackendKind != CEmitterBackend::Vmc4) {
        CA_LOGT_ERROR("cli", "--runtime-protection requires --backend regc or vmc4.\n");
        return 1;
    }
    if (options.stackcExceptionCheck != "full" && options.stackcExceptionCheck != "lite") {
        CA_LOGT_ERROR("cli", "--stackc-exception-check must be full or lite.\n");
        return 1;
    }
    if (!options.nativeDirOutput.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "--emit-native-dir requires --backend regc.\n");
        return 1;
    }
    if (options.llvmToolSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "--llvm-tool requires --compile.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.compilerSelected) {
        CA_LOGT_ERROR("cli", "--backend llvm cannot be combined with --c-compiler.\n");
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr &&
        (!options.cmakeProjectDir.empty() || !options.xmakeProjectDir.empty())) {
        CA_LOGT_ERROR("cli", "--backend llvm does not support --generate-cmake or --generate-xmake.\n");
        return 1;
    }

    if (!options.nativeAnnotation.empty() &&
        !isNativeAnnotationDescriptor(options.nativeAnnotation)) {
        CA_LOGT_ERROR("cli", "--native-annotation must be a JVM annotation descriptor like Lcom/example/Native;.\n");
        printUsage();
        return 1;
    }

    if (!options.nativeAnnotation.empty() && !options.nativeMethodPrefixes.empty()) {
        CA_LOGT_ERROR("cli", "--native-annotation and --native-method-prefix are mutually exclusive.\n");
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && !options.xmakeProjectDir.empty()) {
        CA_LOGT_ERROR("cli", "--generate-cmake and --generate-xmake are mutually exclusive.\n");
        printUsage();
        return 1;
    }

    if (options.compileNative && !options.xmakeProjectDir.empty()) {
        if (options.compilerSelected && options.buildDriver == NativeBuildDriver::Direct) {
            CA_LOGT_ERROR("cli",
                          "A direct --c-compiler cannot be combined with --generate-xmake and "
                          "--compile.\n");
            return 1;
        }
        options.buildDriver = NativeBuildDriver::Xmake;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        !options.cmakeProjectDir.empty()) {
        CA_LOGT_ERROR("cli", "--c-compiler xmake cannot be combined with --generate-cmake.\n");
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.compileMode == mj2x::cli::CompileMode::Pipe) {
        CA_LOGT_ERROR("cli", "xmake compilation does not support --compile-mode pipe.\n");
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.xmakeProjectDir.empty()) {
        std::filesystem::path temporaryProject;
        if (!createTemporaryXmakeProject(options.outputPath, temporaryProject)) {
            CA_LOGT_ERROR("cli", "cannot create temporary xmake project.\n");
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
        CA_LOGT_ERROR("cli",
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
        CA_LOGT_ERROR("cli",
                      "--backend requires --emit-c, --emit-cpp, --emit-llvm, --emit-native-dir, "
                      "or a generated native project.\n");
        printUsage();
        return 1;
    }

    if (options.compileNative && nativeOutputPath.empty() && !nativeDirMode && !pipeCompile) {
        CA_LOGT_ERROR("cli",
                      "--compile requires --emit-c, --emit-cpp, --emit-llvm, --emit-native-dir, "
                      "or a generated native project.\n");
        printUsage();
        return 1;
    }

    if (options.compileModeSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "--compile-mode requires --compile.\n");
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.cmakeProjectDir, ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "cannot create CMake project directory: {}\n", options.cmakeProjectDir.c_str());
            return 1;
        }
    }

    if (!options.xmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(options.xmakeProjectDir) / "src" / "generated", ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "cannot create xmake project directory: {}\n", options.xmakeProjectDir.c_str());
            return 1;
        }
    }

    if (options.compilerSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "--c-compiler requires --compile.\n");
        printUsage();
        return 1;
    }

    mj2x::PipelineConfig config;
    config.inputPath  = options.inputPath;
    config.outputPath = options.outputPath;
    config.dumpMIR    = options.dumpMIR;

    // 用户自定义 intrinsic（issue #818）：--intrinsic-config JSON + --intrinsic-src 追加。
    mj2x::UserIntrinsicConfig userIntrinsicConfig;
    if (!options.intrinsicConfigPath.empty()) {
        std::string error;
        if (!loadUserIntrinsicConfig(options.intrinsicConfigPath, userIntrinsicConfig, error)) {
            CA_LOGT_ERROR("cli", "{}\n", error.c_str());
            return 1;
        }
    }
    for (const auto& s : options.intrinsicSources) userIntrinsicConfig.sources.push_back(s);
    const bool hasUserIntrinsics =
        !userIntrinsicConfig.intrinsics.empty() || !userIntrinsicConfig.sources.empty();

    const bool jarOutput =
        hasExtension(options.inputPath, ".jar") && hasExtension(options.outputPath, ".jar");
    const bool jarNativeLoaderEnabled =
        (options.compileNative || !options.xmakeProjectDir.empty()) && jarOutput;
    std::filesystem::path nativeLibraryPath =
        mj2x::cli::nativeLibraryPathForOutput(options.outputPath);
    std::string nativeResourceEntry =
        "META-INF/mj2x/native/" + nativeLibraryPath.filename().string();
    if (options.nativeLibKeySelected) {
        // 加密存储形态：入口加 .enc 后缀，嵌入前 XOR 加密（NativeLoader 按
        // 后缀解密；sepacker 加壳 DLL 走同一形态）。
        nativeResourceEntry += mj2x::kNativeLibEncryptedSuffix;
    }
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
            if (hasUserIntrinsics) regcBackend->setUserIntrinsics(userIntrinsicConfig);
            if (!options.cOutputPath.empty()) regcBackend->setOutputPath(options.cOutputPath);
            if (options.regcTraceCalls) regcBackend->setTraceCallLog(true);
            if (options.regcDirectCalls) regcBackend->setDirectCalls(true);
            regcBackend->setCommentLevel(options.regcCommentLevel);
            if (options.regcSafeEnv) regcBackend->setSafeEnv(true);
            if (options.regcSafeEnvDebug) regcBackend->setSafeEnvDebug(true);
            if (options.regcNoInline) regcBackend->setInlineEnabled(false);
            // 基础混淆默认关闭；--regc-obfuscate 显式开启基础三项，
            // #829 增强开关各自联动对应基础项（mbaDeep→MBA、opaqueZero→opaque、
            // cffDead/stateEncrypt→CFF）。
            if (options.regcObfuscate || options.regcCffDead != 0 ||
                options.regcCffStateEncrypt || options.regcMbaDeep ||
                options.regcOpaqueZero) {
                mj2x::RegTransformOptions transform;
                transform.enableMba                   = options.regcObfuscate || options.regcMbaDeep;
                transform.enableOpaquePredicates      = options.regcObfuscate || options.regcOpaqueZero;
                transform.enableControlFlowFlattening = options.regcObfuscate ||
                                                        options.regcCffDead != 0 ||
                                                        options.regcCffStateEncrypt;
                transform.cffDeadBlockDensity = static_cast<ca::u32>(options.regcCffDead);
                transform.cffStateEncryption  = options.regcCffStateEncrypt;
                transform.mbaDeep             = options.regcMbaDeep;
                transform.opaqueZero          = options.regcOpaqueZero;
                transform.transformSeed =
                    mj2x::deriveClientSeed(options.regcTransformSeed, options.clientId);
                regcBackend->setTransformOptions(transform);
            }
            if (!options.nativeDirOutput.empty())
                regcBackend->setSplitOutputDir(options.nativeDirOutput);
            if (options.runtimeProtection != "none") {
                regcBackend->setRuntimeProtection(
                    parseRuntimeProtectionAction(options.runtimeProtection),
                    static_cast<uint32_t>(options.runtimeProtectionStep),
                    static_cast<uint32_t>(options.runtimeProtectionDelayMs));
            }
            nativeSourceBackend = regcBackend.get();
            // RegC 的 native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责。
            pipeline.registerBackend(std::move(regcBackend));
        }
        else if (options.cBackendKind == CEmitterBackend::Vmc4) {
            auto vmc4Backend = std::make_unique<mj2x::Vmc4Backend>();
            if (hasUserIntrinsics) vmc4Backend->setUserIntrinsics(userIntrinsicConfig);
            if (!options.cOutputPath.empty()) vmc4Backend->setOutputPath(options.cOutputPath);
            // dispatcher 形态字符串 -> 枚举（默认 switch_table）。
            mj2x::Vmc4Options vmc4Opts;
            vmc4Opts.seed = mj2x::deriveClientSeed(options.vmc4Seed, options.clientId);
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
            // 实例粒度：per-class（默认，同类方法共享 VM 实例）/ per-method /
            // per-program（全程序共享一个 VM 实例，opcode 空间全局统一）。
            if (options.vmc4Granularity == "method" || options.vmc4Granularity == "per-method")
                vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerMethod;
            else if (options.vmc4Granularity == "program" ||
                     options.vmc4Granularity == "per-program")
                vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerProgram;
            else
                vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerClass;
            vmc4Opts.aliasCount = options.vmc4Alias;
            vmc4Opts.bogusHandlers = options.vmc4Bogus;
            vmc4Opts.driftVariants = options.vmc4Drift;
            vmc4Opts.driftPeriod = options.vmc4DriftPeriod;
            if (options.runtimeProtection != "none") {
                vmc4Opts.runtimeProtection.action  =
                    parseRuntimeProtectionAction(options.runtimeProtection);
                vmc4Opts.runtimeProtection.step    =
                    static_cast<uint32_t>(options.runtimeProtectionStep);
                vmc4Opts.runtimeProtection.delayMs =
                    static_cast<uint32_t>(options.runtimeProtectionDelayMs);
            }
            vmc4Backend->setOptions(vmc4Opts);
            // VMC4 复用同样的 native-loader clinit 注入（NativeLoaderPostprocessor）。
            nativeSourceBackend = vmc4Backend.get();
            pipeline.registerBackend(std::move(vmc4Backend));
        }
        else {
            auto stackCBackend = std::make_unique<mj2x::StackCBackend>();
            if (hasUserIntrinsics) stackCBackend->setUserIntrinsics(userIntrinsicConfig);
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
         options.cBackendKind == CEmitterBackend::RegC ||
         options.cBackendKind == CEmitterBackend::Vmc4)) {
        // StackC / RegC / VMC4 均使用 RegisterNatives 动态绑定，需要
        // mj2x/cloader/Loader 桥接类（其 native 方法 registerNativesForClass 由
        // 本侧 _mj2x_register_for_class 实现；VMC4 finalize 同样发射该桥接 C
        // 函数，缺 Loader.class 时 jar 模式运行报 NoClassDefFoundError）。
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
            injectLoadIntoClinit,
            options.nativeLibKey));
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
            CA_LOGT_ERROR("cli", "native source backend is unavailable.\n");
            return 1;
        }
        nativeArtifact = nativeSourceBackend->generatedSourceArtifact();
    }
    if (!options.cppOutputPath.empty()) {
        if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
            CA_LOGT_ERROR("cli", "--emit-cpp requires a C99 backend source artifact.\n");
            return 1;
        }
        cppSource = mj2x::cli::emitCpp17Dialect(nativeArtifact.content);
        std::ofstream output(options.cppOutputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            CA_LOGT_ERROR("cli", "cannot write emitted C++ source: {}\n", options.cppOutputPath.c_str());
            return 1;
        }
        output.write(cppSource.data(), static_cast<std::streamsize>(cppSource.size()));
        if (!output) {
            CA_LOGT_ERROR("cli", "cannot write emitted C++ source: {}\n", options.cppOutputPath.c_str());
            return 1;
        }
        nativeArtifact = {mj2x::NativeSourceLanguage::Cpp17, cppSource, "native.cpp"};
    }
    if (!options.llvmOutputPath.empty() &&
        nativeArtifact.language != mj2x::NativeSourceLanguage::LlvmIr) {
        CA_LOGT_ERROR("cli", "--emit-llvm requires an LLVM IR backend source artifact.\n");
        return 1;
    }

    if (!options.cmakeProjectDir.empty()) {
        std::filesystem::path cmakeCrtRoot;
        if (options.cBackendKind == CEmitterBackend::RegC) {
            cmakeCrtRoot = mj2x::cli::findCrtRoot(argv[0]);
            if (cmakeCrtRoot.empty()) {
                CA_LOGT_ERROR("cli",
                              "RegC CMake project requires an mj2x-crt source pack; set "
                              "MJ2X_CRT_DIR.\n");
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
        // 用户 intrinsic 实现文件（issue #818）：复制进工程 src/mj2x-crt/ 下，
        // xmake 模板的 glob（src/mj2x-crt/**.c）自动并入编译。
        if (!userIntrinsicConfig.sources.empty()) {
            std::string error;
            if (!copyUserIntrinsicSources(userIntrinsicConfig.sources,
                                          std::filesystem::path(options.xmakeProjectDir), error)) {
                CA_LOGT_ERROR("cli", "{}\n", error.c_str());
                return 1;
            }
        }
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
                    CA_LOGT_ERROR("cli",
                                  "RegC --compile requires an mj2x-crt source pack; set "
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
                    CA_LOGT_ERROR("cli", "native source backend is unavailable.\n");
                    return 1;
                }
                if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
                    CA_LOGT_ERROR("cli",
                                  "--compile-mode pipe only supports C99 native source "
                                  "artifacts.\n");
                    return 1;
                }
                compileRc = mj2x::cli::compileCSourceFromPipe(
                    nativeArtifact.content, options.outputPath, options.cCompilerKind, crtRoot,
                    userIntrinsicConfig.sources);
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
                                      crtRoot,
                                      userIntrinsicConfig.sources)
                                : mj2x::cli::compileCSource(
                                      compileSource,
                                      options.outputPath,
                                      options.cCompilerKind,
                                      crtRoot,
                                      userIntrinsicConfig.sources);
            }
        }
        if (compileRc != 0) return compileRc;
        if (jarNativeLoaderEnabled) {
            // 嵌入前外部处理钩子（sepacker 加壳等）：就地改写编译产物，
            // 随后嵌入（可选 --native-lib-key 加密）使用处理后的文件。
            if (!options.nativeLibPackHook.empty()) {
                const int hookRc = runNativeLibPackHook(options.nativeLibPackHook,
                                                        nativeLibraryPath);
                if (hookRc != 0) return hookRc;
            }
            return embedNativeLibraryInJar(options.outputPath, nativeResourceEntry,
                                           nativeLibraryPath, options.nativeLibKey,
                                           options.nativeLibKeySelected);
        }
    }

    return 0;
}

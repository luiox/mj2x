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

#include <fstream>
#include <optional>
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
#include "embedded_rt.hpp"
#include "native_compiler.hpp"
#include "native_project_generator.hpp"
#include "param_registry.hpp"
#include "jzip/memory_segment.hpp"
#include "libca/core/tag_cast.hpp"
#include "jzip/zip_io.hpp"
#include "libca/zip/checksum.hpp"

#include "libca/log/log_macros.hpp"
#include "libca/process/subprocess.hpp"
#include "libca/i18n/i18n.hpp"

// i18n.embed-lang rule 生成的嵌入翻译表(mj2x-cli 单源文案表)。
#include "i18n_mj2x_cli_translations.generated.hpp"

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
///                 "passContext":false,"mayThrow":false,"passReceiver":false}],
///  "sources":["path/to/my_m.c"]}
/// owner/name/desc/symbol 必填；passContext/mayThrow/passReceiver 可选（默认
/// false）。passReceiver=true 匹配 instance 调用点并把 receiver 作为首参传入
/// （见 doc/user_intrinsic.md）；static 调用点匹配 passReceiver 条目为配置错误，
/// 由后端匹配/编码面 fail-closed 报错。
static bool loadUserIntrinsicConfig(const std::string& path, mj2x::UserIntrinsicConfig& out,
                                    std::string& error)
{
    using json = nlohmann::json;
    json root;
    try {
        std::ifstream file(path);
        if (!file) {
            error = ca::i18n::trf("mj2x.cli.err.intrinsic_open_failed", {path}).to_std_string();
            return false;
        }
        file >> root;
    }
    catch (const json::exception& ex) {
        error = ca::i18n::trf("mj2x.cli.err.intrinsic_invalid_json", {ex.what()}).to_std_string();
        return false;
    }
    if (!root.is_object()) {
        error = ca::i18n::tr("mj2x.cli.err.intrinsic_root_object");
        return false;
    }
    if (root.contains("intrinsics")) {
        const auto& arr = root["intrinsics"];
        if (!arr.is_array()) {
            error = ca::i18n::tr("mj2x.cli.err.intrinsic_needs_array");
            return false;
        }
        for (const auto& item : arr) {
            if (!item.is_object()) {
                error = ca::i18n::tr("mj2x.cli.err.intrinsic_entry_object");
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
                error = ca::i18n::tr("mj2x.cli.err.intrinsic_entry_fields");
                return false;
            }
            if (item.contains("passContext") && item["passContext"].is_boolean())
                u.passContext = item["passContext"].get<bool>();
            if (item.contains("mayThrow") && item["mayThrow"].is_boolean())
                u.mayThrow = item["mayThrow"].get<bool>();
            if (item.contains("passReceiver") && item["passReceiver"].is_boolean())
                u.passReceiver = item["passReceiver"].get<bool>();
            out.intrinsics.push_back(std::move(u));
        }
    }
    if (root.contains("sources")) {
        const auto& arr = root["sources"];
        if (!arr.is_array()) {
            error = ca::i18n::tr("mj2x.cli.err.intrinsic_sources_array");
            return false;
        }
        for (const auto& s : arr) {
            if (!s.is_string()) {
                error = ca::i18n::tr("mj2x.cli.err.intrinsic_source_string");
                return false;
            }
            out.sources.push_back(s.get<std::string>());
        }
    }
    return true;
}

/// @brief 把用户 intrinsic 实现源文件复制进生成工程（xmake 模板用 glob 编译
/// `src/mj2x-rt/**.c`，故复制到 `src/mj2x-rt/user_intrinsics/` 即自动并入）。
static bool copyUserIntrinsicSources(const std::vector<std::string>& sources,
                                     const std::filesystem::path& projectDir, std::string& error)
{
    if (sources.empty()) return true;
    const std::filesystem::path destDir = projectDir / "src" / "mj2x-rt" / "user_intrinsics";
    std::error_code             ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        error = ca::i18n::trf("mj2x.cli.err.intrinsic_dir_failed", {destDir.string()}).to_std_string();
        return false;
    }
    for (const auto& src : sources) {
        const std::filesystem::path from(src);
        const std::filesystem::path to = destDir / from.filename();
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = ca::i18n::trf("mj2x.cli.err.intrinsic_copy_failed", {from.string()}).to_std_string();
            return false;
        }
    }
    return true;
}

static std::vector<ca::u8> readFileBytes(const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    const auto tellg = ifs.tellg();
    // tellg 失败返回 -1：转 size_t 会构造巨大 vector（bad_alloc/崩溃）。
    if (tellg < 0) return {};
    const auto          size = static_cast<std::streamsize>(tellg);
    std::vector<ca::u8> bytes(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), size);
    // 短读（磁盘满/并发截断）静默返回零填充会嵌入截断的 DLL，必须清空并让
    // 调用方的空结果检查报错。
    if (ifs.gcount() != size) return {};
    return bytes;
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(const std::string&         name,
                                                              const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    ca::zip::Crc32 crc;
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
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::trf("mj2x.cli.err.jar_rewrite_failed", {std::string(jarPath)}).c_str());
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(outBytes.data()),
              static_cast<std::streamsize>(outBytes.size()));
    printf("%s\n", ca::i18n::trf("mj2x.cli.info.embedded_lib", {entryName}).c_str());
    return 0;
}

static bool isNativeAnnotationDescriptor(const std::string& descriptor)
{
    return descriptor.size() >= 3 && descriptor.front() == 'L' && descriptor.back() == ';' &&
           descriptor.find('.') == std::string::npos;
}

/// @brief LLVM IR 版 CMake 工程：native.ll + CMakeLists.txt（clang 编译 .ll）。
/// @param withRuntimeProtection 为 true 时内嵌 mj2x-rt rt_protect 源并一起编译
///        （--runtime-protection 检查点的实现侧）。
static int generateLlvmCMakeProject(const std::string& projectDirValue,
                                    const std::string& nativeOutputPath,
                                    bool withRuntimeProtection)
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
        CA_LOGT_ERROR("cli", "emitted LLVM IR source does not exist: {}\n", sourcePath.string().c_str());
        return 1;
    }

    fs::path expectedSource = projectDir / sourcePath.filename();
    if (fs::absolute(sourcePath, ec) != fs::absolute(expectedSource, ec)) {
        fs::copy_file(sourcePath, expectedSource, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "cannot copy emitted LLVM IR into CMake project: {}\n", ec.message().c_str());
            return 1;
        }
    }

    // JDK intrinsic 直调（mj2x_math_* 等，符号实现在 regc）、mj2x_llvm_* 运行时
    // （llvmir）与 --runtime-protection 检查点（common）都来自 mj2x-rt：按 LLVM
    // 分发面（common+regc+llvmir）随工程分发，CMake glob 编译。
    if (!mj2x::cli::writeRtTree(projectDir, CEmitterBackend::LlvmIr)) {
        CA_LOGT_ERROR("cli", "embedded mj2x-rt resources are unavailable.\n");
        return 1;
    }

    {
        std::ofstream out(projectDir / "CMakeLists.txt", std::ios::binary);
        if (!out) {
            CA_LOGT_ERROR("cli", "cannot write CMakeLists.txt\n");
            return 1;
        }
        out << "cmake_minimum_required(VERSION 3.16)\n";
        out << "project(mj2x_native NONE)\n\n";
        out << "find_program(CLANG_EXECUTABLE clang REQUIRED)\n";
        // COMPONENTS JVM：headless JDK（linux CI）无 AWT 头/库，默认全组件必炸；
        // 生成工程只用 JNI_INCLUDE_DIRS，JVM 组件足够。
        out << "find_package(JNI REQUIRED COMPONENTS JVM)\n\n";
        out << "set(LLVM_IR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/native.ll)\n";
        // mj2x-rt：JDK intrinsic runtime（regc/*.c）+ rt_protect 检查点（common/）
        // + mj2x_llvm_* 运行时（llvmir/）——LLVM 分发面三挂载全参与编译。
        out << "file(GLOB CRT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt/regc/*.c)\n";
        out << "list(APPEND CRT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt/common/"
               "mj2x_rt_protect.c)\n";
        out << "file(GLOB LLVMIR_RT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt/llvmir/*.c)\n";
        out << "list(APPEND CRT_SOURCES ${LLVMIR_RT_SOURCES})\n";
        out << "set(NATIVE_LIB ${CMAKE_CURRENT_BINARY_DIR}/mj2x_native"
               "${CMAKE_SHARED_LIBRARY_SUFFIX})\n";
        out << "set(CRT_ARGS \"\")\n";
        out << "foreach(INC ${JNI_INCLUDE_DIRS})\n";
        out << "  string(APPEND CRT_ARGS \" -I${INC}\")\n";
        out << "endforeach()\n";
        out << "string(APPEND CRT_ARGS \" -I${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt\")\n";
        out << "string(APPEND CRT_ARGS \" -D"
            << std::string{mj2x::cli::rtBackendDefine(CEmitterBackend::LlvmIr)} << "\")\n";
        out << "add_custom_command(\n";
        out << "  OUTPUT ${NATIVE_LIB}\n";
        out << "  COMMAND ${CLANG_EXECUTABLE} -shared ${CRT_ARGS} ${LLVM_IR_FILE} "
               "${CRT_SOURCES} -o ${NATIVE_LIB}\n";
        out << "  DEPENDS ${LLVM_IR_FILE} ${CRT_SOURCES}\n";
        out << "  COMMENT \"Compiling LLVM IR with clang\")\n";
        out << "add_custom_target(mj2x_native ALL DEPENDS ${NATIVE_LIB})\n";
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
        out << "# mj2x native CMake project (LLVM IR)\n\n";
        out << "Build:\n\n";
        out << "```sh\n";
        out << "cmake -S . -B build\n";
        out << "cmake --build build --config Release\n";
        out << "```\n\n";
        out << "Requires clang on PATH.\n";
    }

    printf("%s\n", ca::i18n::trf("mj2x.cli.info.cmake_written", {projectDir.string()}).c_str());
    return 0;
}

static int generateCMakeProject(const std::string& projectDirValue,
                                const std::string& nativeOutputPath,
                                const std::filesystem::path& crtRoot,
                                CEmitterBackend backend)
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

    // mj2x-rt 按后端分发面进入工程 src/mj2x-rt/(与 xmake 工程同布局):
    // 外部源包可用时走拷贝,否则落回内嵌 bundle。
    if (mj2x::cli::isCrtRoot(crtRoot)) {
        if (!mj2x::cli::copyRtTree(crtRoot, projectDir / "src" / "mj2x-rt", backend)) return 1;
    }
    else if (!mj2x::cli::writeRtTree(projectDir, backend)) {
        CA_LOGT_ERROR("cli", "embedded mj2x-rt resources are unavailable.\n");
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
        // COMPONENTS JVM：同上——headless JDK 无 AWT，默认全组件必炸。
        out << "find_package(JNI REQUIRED COMPONENTS JVM)\n\n";
        out << "if(MSVC)\n";
        out << "  add_compile_options(/utf-8)\n";
        out << "endif()\n\n";
        out << "file(GLOB_RECURSE MJ2X_CRT_SOURCES CONFIGURE_DEPENDS "
               "\"${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt/*.c\")\n";
        out << "add_library(mj2x_native SHARED " << sourcePath.filename().string()
            << " ${MJ2X_CRT_SOURCES})\n";
        out << "target_include_directories(mj2x_native PRIVATE ${JNI_INCLUDE_DIRS})\n";
        out << "target_include_directories(mj2x_native PRIVATE "
               "${CMAKE_CURRENT_SOURCE_DIR}/src/mj2x-rt)\n";
        out << "target_compile_definitions(mj2x_native PRIVATE "
            << std::string{mj2x::cli::rtBackendDefine(backend)} << ")\n";
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

    printf("%s\n", ca::i18n::trf("mj2x.cli.info.cmake_written", {projectDir.string()}).c_str());
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
#ifdef _WIN32
    // CLI 全部文案/help/日志输出为 UTF-8（翻译表与源码字面量口径）。传统
    // Windows 控制台默认 OEM 代码页（中文 cp936）下 UTF-8 中文会显示为乱码，
    // 这里切到 UTF-8（65001）；重定向/管道输出不受影响。
    SetConsoleOutputCP(65001);
#endif

    // 无缓冲 stdout/stderr：崩溃/异常退出时日志不丢（定位 pipeline 崩溃点）。
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // 日志门面（libca log + SimpleLogBackend）：默认 Info；--quiet/--verbose
    // 在第一遍选项扫描后调整级别（init 前的参数校验错误用 fprintf 兜底）。
    mj2x::initLogging();

    // i18n:嵌入翻译表注册 + 语言解析。--lang 预扫描优先于 MJ2X_LANG 环境变量,
    // 默认 zh_CN;须在任何错误输出/参数解析之前完成。
    ca::i18n::init({kI18nTableSet_mj2x_cli}, {"MJ2X_LANG"});
    ca::i18n::apply_lang_from_argv(argc, argv);

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
                CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.config_requires_path"));
                printUsage();
                return 1;
            }
            if (!configPath.empty()) {
                CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.config_once"));
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

    // 配置文件：注册表驱动加载，直接写入 options（默认值 < 配置；CLI 显式
    // 参数随后经 parseCli 覆盖同名字段，构成三级优先级）。
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

    // 第二遍：ca::opt 单次解析归一（别名/互斥/未知 token 报错），值语义
    // 校验（Int base0、UInt64/Hex/Enum）仍由注册表 parseCli 单源承担。
    // --lang 已由 i18n 预扫描消费，opt 侧注册 String 仅为通过未知选项校验；
    // --quiet/--verbose 同时给出时 quiet 赢（上方 if/else 顺序），与迁移前一致。
    ca::opt::Parser parser(mj2x::cli::buildOptCommand());
    auto            parsed = parser.parse(argc, argv);
    if (!parsed.is_ok()) {
        const ca::opt::ParseError& err = parsed.unwrap_err();
        if (err.category == ca::opt::ParseErrorCategory::HelpRequested) {
            printUsage();
            return 0;
        }
        std::string message;
        switch (err.category) {
        case ca::opt::ParseErrorCategory::UnknownOption:
        case ca::opt::ParseErrorCategory::UnexpectedArgument:
            message = ca::i18n::trf("mj2x.cli.err.unknown_option",
                                    {err.option.empty() ? err.message : err.option})
                          .to_std_string();
            break;
        case ca::opt::ParseErrorCategory::MissingValue:
        case ca::opt::ParseErrorCategory::EmptyValue:
            message = ca::i18n::trf("mj2x.cli.err.flag_requires_value", {"--" + err.option})
                          .to_std_string();
            break;
        case ca::opt::ParseErrorCategory::MutexConflict:
        case ca::opt::ParseErrorCategory::MutexRequired:
            // 按组标识分派文案（libca ParseError.group 回填 MutexGroup::label;
            // 无 label 组为成员名拼接,落入 default 的现成描述）。
            if (err.group == "dump_once")
                message = std::string(ca::i18n::tr("mj2x.cli.err.dump_once"));
            else
                message = err.message;
            break;
        default:
            message = err.message;
            break;
        }
        CA_LOGT_ERROR("cli", "{}\n", message.c_str());
        printUsage();
        return 1;
    }
    const ca::opt::ParseResult& input = parsed.unwrap();

    // 注册表校验环：仅处理命令行显式来源（source_of == CommandLine）——配置
    // 文件值已由 loadConfigFile 直写 options，重复应用会破坏 emit 输出组
    // "首个 CLI 输出清空其他"的替换语义（回归测试
    // JsonConfigSupportsCppOutputAndCommandLineCanReplaceIt 锁定）。Bool Flag
    // 出现即置位；StringList 逐项追加。
    for (std::size_t i = 0; i < mj2x::cli::kParamCount; ++i) {
        const mj2x::cli::ParamDef& param = mj2x::cli::kParams[i];
        const std::string          name  = mj2x::cli::optCanonicalName(param.cliFlag);
        if (input.source_of(name) != ca::opt::ValueSource::CommandLine) continue;
        std::string error;
        bool        ok = true;
        if (param.type == mj2x::cli::ParamType::Bool) {
            ok = param.parseCli(options, nullptr, error);
        }
        else if (param.type == mj2x::cli::ParamType::StringList) {
            for (const std::string& item : input.get_list(name)) {
                ok = param.parseCli(options, item.c_str(), error);
                if (!ok) break;
            }
        }
        else {
            const std::string value = input.get(name);
            ok                      = param.parseCli(options, value.c_str(), error);
        }
        if (!ok) {
            CA_LOGT_ERROR("cli", "{}\n", error.c_str());
            printUsage();
            return 1;
        }
    }

    bool         dumpSchema   = false;
    bool         dumpTemplate = false;
    std::string  dumpFile;
    dumpSchema   = input.source_of("dump-config-schema") != ca::opt::ValueSource::None;
    dumpTemplate = input.source_of("dump-config-template") != ca::opt::ValueSource::None;
    dumpFile     = dumpSchema   ? input.get("dump-config-schema")
                   : dumpTemplate ? input.get("dump-config-template")
                                  : std::string{};

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
                CA_LOGT_ERROR("cli", "{}\n",
                      ca::i18n::trf("mj2x.cli.err.dump_write_failed",
                                 {dumpSchema ? ca::i18n::tr("mj2x.cli.term.config_schema")
                                             : ca::i18n::tr("mj2x.cli.term.config_template"),
                                  dumpFile})
                                      .c_str());
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
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_mutually_exclusive"));
        return 1;
    }
    if (!options.cppOutputPath.empty() && !hasExtension(options.cppOutputPath, ".cpp")) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_cpp_needs_cpp_ext"));
        return 1;
    }
    if (!options.cppOutputPath.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_cpp_needs_regc"));
        return 1;
    }
    if (!options.llvmOutputPath.empty() && !hasExtension(options.llvmOutputPath, ".ll")) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_llvm_needs_ll_ext"));
        return 1;
    }
    if (!options.llvmOutputPath.empty() && options.cBackendKind != CEmitterBackend::LlvmIr) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_llvm_needs_llvm"));
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.llvmOutputPath.empty() &&
        options.compileMode != mj2x::cli::CompileMode::Pipe) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.backend_llvm_needs_emit_llvm"));
        return 1;
    }
    if (options.llvmToolSelected && options.cBackendKind != CEmitterBackend::LlvmIr) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.llvm_tool_needs_llvm"));
        return 1;
    }
    if (options.regcTraceCalls && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.regc_trace_calls_needs_regc"));
        return 1;
    }
    if ((options.regcSafeEnv || options.regcSafeEnvDebug) &&
        options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.regc_safe_env_needs_regc"));
        return 1;
    }
    if (options.vmc4LazyDecrypt && !options.vmc4EncryptProgram) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.lazy_decrypt_needs_encrypt"));
        return 1;
    }
    if (options.runtimeProtection != "none" && options.runtimeProtection != "log" &&
        options.runtimeProtection != "delay" && options.runtimeProtection != "fake" &&
        options.runtimeProtection != "terminate") {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.runtime_protection_unknown"));
        return 1;
    }
    if (options.runtimeProtection != "none" && options.cBackendKind != CEmitterBackend::RegC &&
        options.cBackendKind != CEmitterBackend::Vmc4 &&
        options.cBackendKind != CEmitterBackend::LlvmIr) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.runtime_protection_needs_native"));
        return 1;
    }
    if (options.stackcExceptionCheck != "full" && options.stackcExceptionCheck != "lite") {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.stackc_exception_check_invalid"));
        return 1;
    }
    if (!options.nativeDirOutput.empty() && options.cBackendKind != CEmitterBackend::RegC) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_native_dir_needs_regc"));
        return 1;
    }
    if (options.llvmToolSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.llvm_tool_needs_compile"));
        return 1;
    }
    if (options.cBackendKind == CEmitterBackend::LlvmIr && options.compilerSelected) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.llvm_backend_no_c_compiler"));
        return 1;
    }

    if (!options.nativeAnnotation.empty() &&
        !isNativeAnnotationDescriptor(options.nativeAnnotation)) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.native_annotation_invalid"));
        printUsage();
        return 1;
    }

    if (!options.nativeAnnotation.empty() && !options.nativeMethodPrefixes.empty()) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.selection_mutually_exclusive"));
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && !options.xmakeProjectDir.empty()) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.generate_mutually_exclusive"));
        printUsage();
        return 1;
    }

    if (options.compileNative && !options.xmakeProjectDir.empty()) {
        if (options.compilerSelected && options.buildDriver == NativeBuildDriver::Direct) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.direct_compiler_no_xmake"));
            return 1;
        }
        options.buildDriver = NativeBuildDriver::Xmake;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        !options.cmakeProjectDir.empty()) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.xmake_driver_no_cmake"));
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.compileMode == mj2x::cli::CompileMode::Pipe) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.xmake_no_pipe"));
        return 1;
    }
    if (options.compileNative && options.buildDriver == NativeBuildDriver::Xmake &&
        options.xmakeProjectDir.empty()) {
        std::filesystem::path temporaryProject;
        if (!createTemporaryXmakeProject(options.outputPath, temporaryProject)) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.temp_xmake_create_failed"));
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
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.pipe_no_emit"));
        return 1;
    }

    if (!options.cmakeProjectDir.empty() && options.cOutputPath.empty() &&
        options.cppOutputPath.empty() && options.llvmOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.cmakeProjectDir) / "native.c").string();
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            options.llvmOutputPath =
                (std::filesystem::path(options.cmakeProjectDir) / "native.ll").string();
            options.cOutputPath.clear();
        }
    }

    if (!options.xmakeProjectDir.empty() && options.cOutputPath.empty() &&
        options.cppOutputPath.empty() && options.llvmOutputPath.empty()) {
        options.cOutputPath =
            (std::filesystem::path(options.xmakeProjectDir) / "src" / "generated" / "native.c")
                .string();
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            options.llvmOutputPath =
                (std::filesystem::path(options.xmakeProjectDir) / "native.ll").string();
            options.cOutputPath.clear();
        }
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
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.backend_needs_output"));
        printUsage();
        return 1;
    }

    if (options.compileNative && nativeOutputPath.empty() && !nativeDirMode && !pipeCompile) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.compile_needs_output"));
        printUsage();
        return 1;
    }

    if (options.compileModeSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.compile_mode_needs_compile"));
        printUsage();
        return 1;
    }

    if (!options.cmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.cmakeProjectDir, ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::trf("mj2x.cli.err.cmake_dir_create_failed", {options.cmakeProjectDir}).c_str());
            return 1;
        }
    }

    if (!options.xmakeProjectDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(options.xmakeProjectDir) / "src" / "generated", ec);
        if (ec) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::trf("mj2x.cli.err.xmake_dir_create_failed", {options.xmakeProjectDir}).c_str());
            return 1;
        }
    }

    if (options.compilerSelected && !options.compileNative) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.c_compiler_needs_compile"));
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

    // R4 保护变换选项（RegIR 级，RegC/LLVM 共用；基础混淆默认关闭，
    // --regc-obfuscate 显式开启基础三项，#829 增强开关各自联动对应基础项）。
    auto buildTransformOptions = [&]() -> std::optional<mj2x::RegTransformOptions> {
        if (!options.regcObfuscate && options.regcCffDead == 0 && !options.regcCffStateEncrypt &&
            !options.regcMbaDeep && !options.regcOpaqueZero) {
            return std::nullopt;
        }
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
        return transform;
    };

    mj2x::Pipeline pipeline(std::move(config));

    const bool needsNativeSource = !nativeOutputPath.empty() || pipeCompile || nativeDirMode;
    mj2x::Backend* nativeSourceBackend = nullptr;

    if (needsNativeSource) {
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            auto llvmBackend = std::make_unique<mj2x::LlvmIrBackend>();
            if (!options.llvmOutputPath.empty()) llvmBackend->setOutputPath(options.llvmOutputPath);
            if (auto transform = buildTransformOptions()) {
                llvmBackend->setTransformOptions(*transform);
            }
            if (options.runtimeProtection != "none") {
                llvmBackend->setRuntimeProtection(
                    parseRuntimeProtectionAction(options.runtimeProtection),
                    static_cast<uint32_t>(options.runtimeProtectionStep),
                    static_cast<uint32_t>(options.runtimeProtectionDelayMs));
            }
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
            // cffDead/stateEncrypt→CFF）。RegIR 级变换与 LLVM 后端共用。
            if (auto transform = buildTransformOptions()) {
                regcBackend->setTransformOptions(*transform);
            }
            if (!options.nativeDirOutput.empty())
                regcBackend->setSplitOutputDir(options.nativeDirOutput);
            // --strings-encrypt：字面量 at-rest 加密（stackc/regc 专属开关，
            // LLVM 后端不支持——PARAM 的 backends 面已限 stackc|regc）。
            if (options.stringsEncrypt) regcBackend->setStringsEncrypt(true);
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
            vmc4Opts.programBlobEncrypt = options.vmc4EncryptProgram;
            vmc4Opts.programLazyDecrypt = options.vmc4LazyDecrypt;
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
            // --strings-encrypt：字符串池 at-rest 加密（stackc/regc 专属开关）。
            if (options.stringsEncrypt) stackCBackend->setStringsEncrypt(true);
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
        auto* vmc4 = ca::core::tag_cast::cast<mj2x::Vmc4Backend>(nativeSourceBackend);
        std::ofstream ts(options.vmc4TraceSurface, std::ios::binary | std::ios::trunc);
        if (ts) ts << vmc4->traceSurface();
    }

    std::string                  cppSource;
    mj2x::NativeSourceArtifact nativeArtifact{mj2x::NativeSourceLanguage::C99, {}, {}};
    if (needsNativeSource) {
        if (nativeSourceBackend == nullptr) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.backend_unavailable"));
            return 1;
        }
        nativeArtifact = nativeSourceBackend->generatedSourceArtifact();
    }
    if (!options.cppOutputPath.empty()) {
        if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_cpp_needs_c99"));
            return 1;
        }
        cppSource = mj2x::cli::emitCpp17Dialect(nativeArtifact.content);
        std::ofstream output(options.cppOutputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::trf("mj2x.cli.err.emit_cpp_write_failed", {options.cppOutputPath}).c_str());
            return 1;
        }
        output.write(cppSource.data(), static_cast<std::streamsize>(cppSource.size()));
        if (!output) {
            CA_LOGT_ERROR("cli", "{}\n", ca::i18n::trf("mj2x.cli.err.emit_cpp_write_failed", {options.cppOutputPath}).c_str());
            return 1;
        }
        nativeArtifact = {mj2x::NativeSourceLanguage::Cpp17, cppSource, "native.cpp"};
    }
    if (!options.llvmOutputPath.empty() &&
        nativeArtifact.language != mj2x::NativeSourceLanguage::LlvmIr) {
        CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.emit_llvm_needs_artifact"));
        return 1;
    }

    const bool withRuntimeProtection = options.runtimeProtection != "none";

    if (!options.cmakeProjectDir.empty()) {
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            int projectRc = generateLlvmCMakeProject(options.cmakeProjectDir, nativeOutputPath,
                                                     withRuntimeProtection);
            if (projectRc != 0) return projectRc;
        }
        else {
            // StackC/RegC/VMC4 的 CMake 工程同样按后端分发 mj2x-rt(#894 后
            // 四后端运行时 ABI 全在 mj2x-rt);外部源包缺失时嵌入兜底。
            int projectRc = generateCMakeProject(options.cmakeProjectDir, nativeOutputPath,
                                                 mj2x::cli::findCrtRoot(argv[0]),
                                                 options.cBackendKind);
            if (projectRc != 0) return projectRc;
        }
    }

    if (!options.xmakeProjectDir.empty()) {
        if (options.cBackendKind == CEmitterBackend::LlvmIr) {
            int projectRc = mj2x::cli::generateLlvmXmakeProject(options.xmakeProjectDir,
                                                                nativeOutputPath,
                                                                withRuntimeProtection);
            if (projectRc != 0) return projectRc;
        }
        else {
            mj2x::cli::XmakeProjectConfig projectConfig;
            projectConfig.projectDir          = options.xmakeProjectDir;
            projectConfig.generatedSourcePath = nativeOutputPath;
            projectConfig.crtRoot             = mj2x::cli::findCrtRoot(argv[0]);
            projectConfig.projectName         = "mj2x_native_project";
            projectConfig.backend             = options.cBackendKind;
            int projectRc                     = mj2x::cli::generateXmakeProject(projectConfig);
            if (projectRc != 0) return projectRc;
            // 用户 intrinsic 实现文件（issue #818）：复制进工程 src/mj2x-rt/ 下，
            // xmake 模板的 glob（src/mj2x-rt/**.c）自动并入编译。
            if (!userIntrinsicConfig.sources.empty()) {
                std::string error;
                if (!copyUserIntrinsicSources(userIntrinsicConfig.sources,
                                              std::filesystem::path(options.xmakeProjectDir), error)) {
                    CA_LOGT_ERROR("cli", "{}\n", error.c_str());
                    return 1;
                }
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
            {
                // #894 运行时/模板分离后,四个后端的运行时 ABI 全部在 mj2x-rt
                // (regc/vmc4/stackc 源 + LLVM 的 JDK intrinsic 直调符号),
                // --compile 一律要求源包并链接消费,不再有自包含生成 TU。
                crtRoot = mj2x::cli::findCrtRoot(argv[0]);
                if (crtRoot.empty()) {
                    const char* backendName = "RegC";
                    if (options.cBackendKind == CEmitterBackend::StackC) backendName = "StackC";
                    else if (options.cBackendKind == CEmitterBackend::Vmc4) backendName = "VMC4";
                    else if (options.cBackendKind == CEmitterBackend::LlvmIr) backendName = "LLVM";
                    CA_LOGT_ERROR("cli", "{}\n",
                                      ca::i18n::trf("mj2x.cli.err.compile_crt_required",
                                                {std::string{backendName}})
                                          .c_str());
                    return 1;
                }
            }
            if (options.cBackendKind == CEmitterBackend::LlvmIr) {
                if (options.compileMode == mj2x::cli::CompileMode::Pipe) {
                    compileRc = mj2x::cli::compileLlvmIrFromPipe(
                        nativeArtifact.content, options.outputPath, options.llvmTool,
                        withRuntimeProtection, crtRoot);
                }
                else {
                    compileRc = mj2x::cli::compileLlvmIrSource(
                        nativeOutputPath, options.outputPath, options.llvmTool,
                        withRuntimeProtection, crtRoot);
                }
            }
            else if (options.compileMode == mj2x::cli::CompileMode::Pipe) {
                if (nativeSourceBackend == nullptr) {
                    CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.backend_unavailable"));
                    return 1;
                }
                if (nativeArtifact.language != mj2x::NativeSourceLanguage::C99) {
                    CA_LOGT_ERROR("cli", "{}\n", ca::i18n::tr("mj2x.cli.err.pipe_c99_only"));
                    return 1;
                }
                compileRc = mj2x::cli::compileCSourceFromPipe(
                    nativeArtifact.content, options.outputPath, options.cBackendKind,
                    options.cCompilerKind, crtRoot, userIntrinsicConfig.sources);
            }
            else {
                // 多文件模式（--emit-native-dir）：编译目录内全部 .c。
                const std::string compileSource =
                    nativeDirMode ? options.nativeDirOutput : nativeOutputPath;
                compileRc = nativeArtifact.language == mj2x::NativeSourceLanguage::Cpp17
                                ? mj2x::cli::compileCppSource(
                                      compileSource,
                                      options.outputPath,
                                      options.cBackendKind,
                                      options.cCompilerKind,
                                      crtRoot,
                                      userIntrinsicConfig.sources)
                                : mj2x::cli::compileCSource(
                                      compileSource,
                                      options.outputPath,
                                      options.cBackendKind,
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

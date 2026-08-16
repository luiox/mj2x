#include "native_compiler.hpp"

#include "embedded_resources.hpp"

#include "libca/log/log_macros.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#ifndef _WIN32
#    include <strings.h>
#endif
// libca/process/subprocess.hpp 会 #undef 全局 stdout/stderr 宏（Command 成员
// 同名防污染），这里在 include 之前固化宏值，供文件尾部导出的 MSVC 环境
// 加载函数（loadMsvcEnvironment）使用。
namespace {
std::FILE* const kStdoutStream = stdout;
std::FILE* const kStderrStream = stderr;
}

#include "jdk_finder/jdk_finder.hpp"
#include "libca/process/subprocess.hpp"

namespace mj2x::cli {

// 导出给 native_project_builder（xmake 子进程复用 MSVC 环境）：加载
// INCLUDE/LIB/PATH，必要时调 vcvarsall.bat x64。自包含实现——匿名 namespace
// 内的同名函数是 internal linkage 无法跨 TU 链接，这里独立实现（宏值取自
// kStdoutStream/kStderrStream，其在 subprocess.hpp 之前已固化）。
using ProcessEnvironment = std::vector<std::pair<std::string, std::string>>;

bool environmentKeyEquals(const std::string& left, const char* right)
{
#ifdef _WIN32
    return _stricmp(left.c_str(), right) == 0;
#else
    return strcasecmp(left.c_str(), right) == 0;
#endif
}

bool loadMsvcEnvironment(ProcessEnvironment& environment,
                         std::filesystem::path& compilerPath)
{
    const char* includeValue = std::getenv("INCLUDE");
    if (includeValue != nullptr && includeValue[0] != '\0') {
        compilerPath = "cl";
        return true;
    }

    const auto vcvars = jdk_finder::findVcvarsAll();
    if (vcvars.empty()) {
        CA_LOGT_ERROR("compile",
                      "MSVC environment is not configured and vcvarsall.bat was not found.\n");
        return false;
    }
    std::string shellPath = std::getenv("ComSpec") != nullptr ? std::getenv("ComSpec")
                                                              : "cmd.exe";
    ca::process::Command shell(shellPath);
    shell.current_dir(vcvars.parent_path().string());
    shell.env("MJ2X_VCVARS_COMMAND",
              "chcp 65001 >nul && call vcvarsall.bat x64 >nul && set");
    shell.arg("/d").arg("/c").arg("%MJ2X_VCVARS_COMMAND%");
    auto captured = shell.output();
    if (captured.is_err()) {
        CA_LOGT_ERROR("compile", "cannot configure MSVC environment: {}\n", captured.unwrap_err().to_string().c_str());
        return false;
    }
    auto output = std::move(captured).unwrap();
    if (!output.status.success()) {
        if (!output.stdout_data.empty())
            fwrite(output.stdout_data.data(), 1, output.stdout_data.size(), kStdoutStream);
        if (!output.stderr_data.empty())
            fwrite(output.stderr_data.data(), 1, output.stderr_data.size(), kStderrStream);
        CA_LOGT_ERROR("compile", "vcvarsall.bat failed with exit code {}.\n", output.status.code);
        return false;
    }

    std::istringstream lines(output.stdout_data);
    std::string        line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0) continue;
        environment.emplace_back(line.substr(0, separator), line.substr(separator + 1));
    }
    std::filesystem::path found;
    std::string           pathValue;
    for (const auto& [key, value] : environment) {
        if (environmentKeyEquals(key, "PATH")) {
            pathValue = value;
            break;
        }
    }
    std::istringstream paths(pathValue);
    std::string        entry;
    std::error_code    ec;
    while (std::getline(paths, entry, ';')) {
        const auto first = entry.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto last = entry.find_last_not_of(" \t\r\n");
        entry = entry.substr(first, last - first + 1);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
            entry = entry.substr(1, entry.size() - 2);
        if (entry.empty()) continue;
        const auto candidate = std::filesystem::path(entry) / "cl.exe";
        if (std::filesystem::is_regular_file(candidate, ec)) {
            found = candidate;
            break;
        }
        ec.clear();
    }
    if (found.empty()) {
        CA_LOGT_ERROR("compile", "vcvarsall.bat completed without making cl.exe available.\n");
        return false;
    }
    compilerPath = found;
    return true;
}

namespace {

/// 跨平台读环境变量（编译流程用：判断 INCLUDE 是否配置等）。JDK/工具链探测
/// 已统一到 libjdk_finder，这里只保留编译命令构造需要的少量 env 读取。
std::string envValue(const char* name)
{
#ifdef _WIN32
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

/// CCompilerKind → 可执行文件名（cl/gcc/clang/zig），用于构造编译命令。
const char* compilerExecutable(CCompilerKind compiler)
{
    switch (compiler) {
    case CCompilerKind::Msvc:  return "cl";
    case CCompilerKind::Gcc:   return "gcc";
    case CCompilerKind::Clang: return "clang";
    case CCompilerKind::Zig:   return "zig";
    case CCompilerKind::Auto:  break;
    }
    return "";
}

/// CCompilerKind → C++ 可执行文件名。MSVC 通过源文件扩展名自动选择 C++ 前端。
const char* cppCompilerExecutable(CCompilerKind compiler)
{
    switch (compiler) {
    case CCompilerKind::Msvc:  return "cl";
    case CCompilerKind::Gcc:   return "g++";
    case CCompilerKind::Clang: return "clang++";
    case CCompilerKind::Zig:   return "zig";
    case CCompilerKind::Auto:  break;
    }
    return "";
}

/// mj2x::cli::CCompilerKind 与 jdk_finder::CCompilerKind 值一一对应，做透传映射。
jdk_finder::CCompilerKind toFinderKind(CCompilerKind kind)
{
    switch (kind) {
    case CCompilerKind::Msvc:  return jdk_finder::CCompilerKind::Msvc;
    case CCompilerKind::Gcc:   return jdk_finder::CCompilerKind::Gcc;
    case CCompilerKind::Clang: return jdk_finder::CCompilerKind::Clang;
    case CCompilerKind::Zig:   return jdk_finder::CCompilerKind::Zig;
    case CCompilerKind::Auto:  break;
    }
    return jdk_finder::CCompilerKind::Auto;
}

CCompilerKind fromFinderKind(jdk_finder::CCompilerKind kind)
{
    switch (kind) {
    case jdk_finder::CCompilerKind::Msvc:  return CCompilerKind::Msvc;
    case jdk_finder::CCompilerKind::Gcc:   return CCompilerKind::Gcc;
    case jdk_finder::CCompilerKind::Clang: return CCompilerKind::Clang;
    case jdk_finder::CCompilerKind::Zig:   return CCompilerKind::Zig;
    case jdk_finder::CCompilerKind::Auto:  break;
    }
    return CCompilerKind::Auto;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool addCrtSources(const std::filesystem::path&        crtRoot,
                   std::vector<std::filesystem::path>& includes,
                   std::vector<std::filesystem::path>& sources)
{
    const auto      sourceRoot = crtRoot / "src";
    const auto      manifest   = crtRoot / "resources.manifest";
    std::error_code ec;
    if (!std::filesystem::exists(sourceRoot / "regc" / "mj2x_regc_crt.h", ec) ||
        !std::filesystem::exists(manifest, ec)) {
        CA_LOGT_ERROR("compile", "invalid mj2x-crt source pack: {}\n", crtRoot.string().c_str());
        return false;
    }

    std::ifstream in(manifest);
    if (!in) {
        CA_LOGT_ERROR("compile", "cannot read mj2x-crt manifest: {}\n", manifest.string().c_str());
        return false;
    }

    includes.push_back(sourceRoot);
    bool        foundSource = false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("src/", 0) != 0) continue;

        const std::filesystem::path relative(line);
        bool                        escapesRoot = relative.is_absolute();
        for (const auto& component : relative) {
            if (component == "..") {
                escapesRoot = true;
                break;
            }
        }
        if (escapesRoot) {
            CA_LOGT_ERROR("compile", "invalid mj2x-crt manifest source path: {}\n", line.c_str());
            return false;
        }

        const auto source    = crtRoot / relative;
        const auto extension = source.extension().string();
        if (extension == ".cpp") {
            CA_LOGT_ERROR("compile",
                          "--compile does not yet support C++ mj2x-crt sources; use "
                          "--generate-xmake.\n");
            return false;
        }
        if (extension != ".c") continue;
        if (!std::filesystem::exists(source, ec)) {
            CA_LOGT_ERROR("compile", "mj2x-crt manifest source is missing: {}\n", source.string().c_str());
            return false;
        }
        sources.push_back(source);
        foundSource = true;
    }

    if (!foundSource) {
        CA_LOGT_ERROR("compile", "mj2x-crt manifest has no C sources.\n");
        return false;
    }
    return true;
}

bool canUseMsvc()
{
    auto tc = jdk_finder::findNativeCompiler(jdk_finder::CCompilerKind::Msvc);
    return tc.available;
}

bool detectCompiler(CCompilerKind requested, CCompilerKind& detected)
{
    auto tc = jdk_finder::findNativeCompiler(toFinderKind(requested));
    if (!tc.available) return false;
    detected = fromFinderKind(tc.compiler);
    return true;
}

std::filesystem::path nativeLibraryPath(const std::string& outputPath)
{
    std::filesystem::path output(outputPath);
    std::filesystem::path dir = output.parent_path();
    if (dir.empty()) dir = ".";

#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif

    return dir / (output.stem().string() + ext);
}

bool jniIncludePaths(std::vector<std::filesystem::path>& includes)
{
    jdk_finder::FindOptions opts;
    opts.requireJniHeader = true;
    auto jdk = jdk_finder::findJdk(opts);
    if (!jdk.available) {
        CA_LOGT_ERROR("compile", "{}; required to locate JNI headers.\n", jdk.reason.c_str());
        return false;
    }
    includes.push_back(jdk.jdkInclude);
    includes.push_back(jdk.jdkIncludePlat);
    return true;
}

std::filesystem::path findVcvarsAll()
{
    return jdk_finder::findVcvarsAll();
}

void writeProcessOutput(const ca::process::Output& output)
{
    if (!output.stdout_data.empty())
        fwrite(output.stdout_data.data(), 1, output.stdout_data.size(), kStdoutStream);
    if (!output.stderr_data.empty())
        fwrite(output.stderr_data.data(), 1, output.stderr_data.size(), kStderrStream);
}

int runCompilerCommand(ca::process::Command& command, const char* compilerName)
{
    auto completed = command.output();
    if (completed.is_err()) {
        CA_LOGT_ERROR("compile", "cannot start {} compiler: {}\n", compilerName,
                completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        CA_LOGT_ERROR("compile", "{} compiler failed with exit code {}.\n", compilerName,
                output.status.code);
        return 1;
    }
    return 0;
}

class ScopedTemporaryDirectory
{
public:
    ~ScopedTemporaryDirectory()
    {
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    bool create(const std::filesystem::path& parent, const char* prefix)
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto token = std::string(prefix) + std::to_string(ticks) + "_" +
                           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        std::error_code ec;
        path_ = parent / token;
        return std::filesystem::create_directory(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool compileCrtSourcesToObjects(const std::vector<std::filesystem::path>& sources,
                                const std::vector<std::filesystem::path>& includes,
                                CCompilerKind compiler,
                                const std::filesystem::path& objectDirectory,
                                std::vector<std::filesystem::path>& objects)
{
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto object = objectDirectory / ("crt_" + std::to_string(index) + ".o");
        ca::process::Command command(compilerExecutable(compiler));
        if (compiler == CCompilerKind::Zig) command.arg("cc");
        // MSVC 用 cl 参数风格，且 CRT 源文件为 UTF-8 注释（无 BOM）：
        // 必须带 /utf-8，否则默认按 ANSI 代码页解析中文注释（C4819 甚至
        // 注释字节被误读为续行符吞掉 #if 导致 C1018）。
        if (compiler == CCompilerKind::Msvc) {
            command.arg("/c").arg("/utf-8");
            for (const auto& include : includes) command.arg("/I" + include.string());
            command.arg("/Fo" + object.string());
            command.arg(sources[index].string());
        }
        else {
            command.arg("-c");
#ifndef _WIN32
            command.arg("-fPIC");
#endif
            for (const auto& include : includes) command.arg("-I" + include.string());
            command.arg(sources[index].string()).arg("-o").arg(object.string());
        }
        if (runCompilerCommand(command, compilerExecutable(compiler)) != 0) return false;
        objects.push_back(object);
    }
    return true;
}

#ifdef _WIN32
std::string uniqueCompilerSourceToken()
{
    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "mj2x_compile_" + std::to_string(ticks) + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

using ProcessEnvironment = std::vector<std::pair<std::string, std::string>>;

bool environmentKeyEquals(const std::string& left, const char* right)
{
#ifdef _WIN32
    return _stricmp(left.c_str(), right) == 0;
#else
    return strcasecmp(left.c_str(), right) == 0;
#endif
}

std::string environmentValue(const ProcessEnvironment& environment, const char* key)
{
    const auto found = std::find_if(environment.begin(), environment.end(), [&](const auto& entry) {
        return environmentKeyEquals(entry.first, key);
    });
    return found == environment.end() ? std::string{} : found->second;
}

std::filesystem::path findExecutableOnPath(const std::string& pathValue,
                                           const char*        executable)
{
    std::istringstream paths(pathValue);
    std::string        entry;
    std::error_code    ec;
    while (std::getline(paths, entry, ';')) {
        entry = trim(entry);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
            entry = entry.substr(1, entry.size() - 2);
        if (entry.empty()) continue;
        const auto candidate = std::filesystem::path(entry) / executable;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
        ec.clear();
    }
    return {};
}

class ScopedTemporarySource
{
public:
    explicit ScopedTemporarySource(std::filesystem::path path)
        : path_(std::move(path))
    {}
    ~ScopedTemporarySource()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

int compileMsvcSourceFromTemporaryFile(
    std::string_view                           cSource,
    const std::filesystem::path&               libraryPath,
    const std::vector<std::filesystem::path>& includes,
    const std::vector<std::filesystem::path>& sources)
{
    ProcessEnvironment    environment;
    std::filesystem::path compilerPath;
    if (!loadMsvcEnvironment(environment, compilerPath)) return 1;

    std::filesystem::path sourceDir = libraryPath.parent_path();
    if (sourceDir.empty()) sourceDir = ".";
    ScopedTemporarySource sourceFile(
        sourceDir /
        ("." + libraryPath.stem().string() + ".mj2x." + uniqueCompilerSourceToken() + ".c"));
    std::ofstream source(sourceFile.path(), std::ios::binary | std::ios::trunc);
    if (!source) {
        CA_LOGT_ERROR("compile", "cannot create temporary MSVC source: {}\n", sourceFile.path().string().c_str());
        return 1;
    }
    source.write(cSource.data(), static_cast<std::streamsize>(cSource.size()));
    source.close();
    if (!source) {
        CA_LOGT_ERROR("compile", "cannot write temporary MSVC source: {}\n", sourceFile.path().string().c_str());
        return 1;
    }

    ca::process::Command command(compilerPath.string());
    command.arg("/nologo").arg("/LD").arg("/TC").arg("/utf-8");
    for (const auto& include : includes) command.arg("/I" + include.string());
    std::filesystem::path objectDir = libraryPath.parent_path();
    if (objectDir.empty()) objectDir = ".";
    objectDir /= "";
    command.arg("/Fo" + objectDir.string());
    command.arg(sourceFile.path().string());
    for (const auto& source : sources) command.arg(source.string());
    command.arg("/Fe" + libraryPath.string());
    for (const auto& [key, value] : environment) command.env(key, value);
    command.stdout(ca::process::Stdio::piped()).stderr(ca::process::Stdio::piped());

    auto completed = command.output();
    if (completed.is_err()) {
        CA_LOGT_ERROR("compile", "cannot start MSVC compiler: {}\n", completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        CA_LOGT_ERROR("compile", "MSVC compiler failed with exit code {}.\n", output.status.code);
        return 1;
    }
    return 0;
}
#endif

}   // namespace

bool parseCompilerName(const std::string& name, CCompilerKind& compiler)
{
    if (name == "msvc") {
        compiler = CCompilerKind::Msvc;
        return true;
    }
    if (name == "gcc") {
        compiler = CCompilerKind::Gcc;
        return true;
    }
    if (name == "clang") {
        compiler = CCompilerKind::Clang;
        return true;
    }
    if (name == "zig") {
        compiler = CCompilerKind::Zig;
        return true;
    }
    return false;
}

bool parseCompileModeName(const std::string& name, CompileMode& mode)
{
    if (name == "file") {
        mode = CompileMode::File;
        return true;
    }
    if (name == "pipe") {
        mode = CompileMode::Pipe;
        return true;
    }
    return false;
}

bool parseLlvmToolName(const std::string& name, LlvmTool& tool)
{
    if (name == "clang") {
        tool = LlvmTool::Clang;
        return true;
    }
    if (name == "llc") {
        tool = LlvmTool::Llc;
        return true;
    }
    return false;
}

std::filesystem::path nativeLibraryPathForOutput(const std::string& outputPath)
{
    return nativeLibraryPath(outputPath);
}

/// @brief 把内嵌 mj2x-crt rt_protect 源写入临时目录；返回 .c 路径（空 = 失败）。
/// @details rt_protect 的 .h/.c 必须同目录（.c 相对 include）；临时目录生命周期
/// 由调用方（ScopedTemporaryDirectory）保证覆盖整个编译过程。
std::filesystem::path writeRtProtectSource(ScopedTemporaryDirectory& directory)
{
    const auto* header = findEmbeddedResource("src/rt_protect/mj2x_rt_protect.h");
    const auto* source = findEmbeddedResource("src/rt_protect/mj2x_rt_protect.c");
    if (header == nullptr || source == nullptr) return {};
    const auto headerPath = directory.path() / "mj2x_rt_protect.h";
    const auto sourcePath = directory.path() / "mj2x_rt_protect.c";
    {
        std::ofstream out(headerPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(header->data),
                  static_cast<std::streamsize>(header->size));
    }
    {
        std::ofstream out(sourcePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(source->data),
                  static_cast<std::streamsize>(source->size));
    }
    if (!std::filesystem::is_regular_file(sourcePath)) return {};
    return sourcePath;
}

/// @brief 准备 LLVM 编译侧的 mj2x-crt 附加源（JDK intrinsic 直调符号如
/// mj2x_math_*，实现见 src/regc/mj2x_regc_intrinsic.c）。
/// @return false 表示 crtRoot 非空但源包/JNI include 无效（调用方应报错）。
bool prepareLlvmCrtSources(const std::filesystem::path&        crtRoot,
                           std::vector<std::filesystem::path>& includes,
                           std::vector<std::filesystem::path>& sources)
{
    if (crtRoot.empty()) return true;
    std::vector<std::filesystem::path> manifestSources;
    if (!addCrtSources(crtRoot, includes, manifestSources)) return false;
    // rt_protect 由 --runtime-protection 的专用提取路径提供（.h/.c 同目录成对），
    // manifest 里也列了它——跳过以免链接期重复符号（LNK2005）。
    for (const auto& source : manifestSources) {
        if (source.filename() == "mj2x_rt_protect.c") continue;
        sources.push_back(source);
    }
    std::vector<std::filesystem::path> jni;
    if (!jniIncludePaths(jni)) return false;
    includes.insert(includes.end(), jni.begin(), jni.end());
    return true;
}

int compileLlvmIrSource(const std::string& llvmOutputPath, const std::string& outputPath,
                        LlvmTool tool, bool withRuntimeProtection,
                        const std::filesystem::path& crtRoot)
{
    std::error_code ec;
    const std::filesystem::path sourcePath(llvmOutputPath);
    if (!std::filesystem::is_regular_file(sourcePath, ec)) {
        CA_LOGT_ERROR("compile", "emitted LLVM IR source does not exist: {}\n", llvmOutputPath.c_str());
        return 1;
    }
    if (!jdk_finder::commandExists("clang")) {
        CA_LOGT_ERROR("compile", "LLVM IR compilation requires clang on PATH.\n");
        return 1;
    }
    if (tool == LlvmTool::Llc && !jdk_finder::commandExists("llc")) {
        CA_LOGT_ERROR("compile", "--llvm-tool llc requires llc on PATH.\n");
        return 1;
    }

    const std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::filesystem::remove(libraryPath, ec);
    ec.clear();

    std::vector<std::filesystem::path> crtIncludes;
    std::vector<std::filesystem::path> crtSources;
    if (!prepareLlvmCrtSources(crtRoot, crtIncludes, crtSources)) return 1;

#ifdef __APPLE__
    const char* sharedFlag = "-dynamiclib";
#else
    const char* sharedFlag = "-shared";
#endif

    // --runtime-protection：检查点实现（mj2x_rt_protect.c）随 .ll 一起编译。
    ScopedTemporaryDirectory rtProtectDirectory;
    std::filesystem::path    rtProtectSource;
    if (withRuntimeProtection) {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        if (!rtProtectDirectory.create(parent, ".mj2x-rt-protect-")) {
            CA_LOGT_ERROR("compile", "cannot create temporary rt_protect directory.\n");
            return 1;
        }
        rtProtectSource = writeRtProtectSource(rtProtectDirectory);
        if (rtProtectSource.empty()) {
            CA_LOGT_ERROR("compile",
                          "embedded mj2x-crt rt_protect sources are unavailable.\n");
            return 1;
        }
    }

    int result = 1;
    if (tool == LlvmTool::Clang) {
        printf("Compiling LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command command("clang");
        command.arg(sharedFlag).arg(sourcePath.string());
        for (const auto& include : crtIncludes) command.arg("-I").arg(include.string());
        if (!rtProtectSource.empty()) command.arg(rtProtectSource.string());
        for (const auto& source : crtSources) command.arg(source.string());
        command.arg("-o").arg(libraryPath.string());
        result = runCompilerCommand(command, "clang");
    }
    else {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        ScopedTemporaryDirectory objectDirectory;
        if (!objectDirectory.create(parent, ".mj2x-llvm-")) {
            CA_LOGT_ERROR("compile", "cannot create temporary LLVM object directory.\n");
            return 1;
        }
#ifdef _WIN32
        const std::filesystem::path objectPath = objectDirectory.path() / "native.obj";
#else
        const std::filesystem::path objectPath = objectDirectory.path() / "native.o";
#endif
        printf("Compiling LLVM IR object with llc: %s\n", objectPath.string().c_str());
        ca::process::Command llcCommand("llc");
        llcCommand.arg("-filetype=obj").arg(sourcePath.string()).arg("-o").arg(
            objectPath.string());
        if (runCompilerCommand(llcCommand, "llc") != 0) return 1;

        std::vector<std::filesystem::path> linkInputs{objectPath};
        if (!rtProtectSource.empty()) {
            const std::filesystem::path rtObject =
                rtProtectDirectory.path() / "mj2x_rt_protect.obj";
            ca::process::Command rtCompile("clang");
            rtCompile.arg("-c").arg(rtProtectSource.string()).arg("-o").arg(rtObject.string());
            if (runCompilerCommand(rtCompile, "clang") != 0) return 1;
            linkInputs.push_back(rtObject);
        }
        for (size_t index = 0; index < crtSources.size(); ++index) {
            const std::filesystem::path crtObject =
                objectDirectory.path() / ("crt_" + std::to_string(index) + ".obj");
            ca::process::Command crtCompile("clang");
            crtCompile.arg("-c");
            for (const auto& include : crtIncludes) crtCompile.arg("-I").arg(include.string());
            crtCompile.arg(crtSources[index].string()).arg("-o").arg(crtObject.string());
            if (runCompilerCommand(crtCompile, "clang") != 0) return 1;
            linkInputs.push_back(crtObject);
        }

        printf("Linking LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command clangCommand("clang");
        clangCommand.arg(sharedFlag);
        for (const auto& input : linkInputs) clangCommand.arg(input.string());
        clangCommand.arg("-o").arg(libraryPath.string());
        result = runCompilerCommand(clangCommand, "clang");
    }

    if (result != 0) {
        std::filesystem::remove(libraryPath, ec);
        return 1;
    }
    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        std::filesystem::remove(libraryPath, ec);
        CA_LOGT_ERROR("compile", "LLVM compiler succeeded without a native library: {}\n", libraryPath.string().c_str());
        return 1;
    }
    printf("Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

int compileNativeSource(const std::string& sourcePath, const std::string& outputPath,
                        CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                        bool useCpp17, const std::vector<std::string>& extraSources)
{
    std::error_code ec;
    if (!std::filesystem::exists(sourcePath, ec)) {
        CA_LOGT_ERROR("compile", "emitted native source does not exist: {}\n", sourcePath.c_str());
        return 1;
    }

    CCompilerKind compiler = CCompilerKind::Auto;
    if (!detectCompiler(requestedCompiler, compiler)) {
        if (requestedCompiler == CCompilerKind::Auto) {
            CA_LOGT_ERROR("compile", "no supported C compiler found on PATH.\n");
        }
        else {
            CA_LOGT_ERROR("compile", "C compiler '{}' not found on PATH.\n", compilerExecutable(requestedCompiler));
        }
        return 1;
    }

    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes)) return 1;

    // 多文件模式（--emit-native-dir）：sourcePath 是目录——编译目录内全部 .c
    // （mj2x_regc_runtime.c / m_*.c / mj2x_regc_entry.c，api.h 由它们 include；
    // crt include 路径由 addCrtSources 统一提供）。
    std::vector<std::filesystem::path> sources;
    if (std::filesystem::is_directory(sourcePath, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(sourcePath, ec)) {
            if (entry.path().extension() == ".c") sources.push_back(entry.path());
        }
        if (sources.empty()) {
            CA_LOGT_ERROR("compile", "no .c sources in native directory: {}\n", sourcePath.c_str());
            return 1;
        }
    }
    else {
        sources.push_back(std::filesystem::path(sourcePath));
    }
    for (const auto& s : extraSources) sources.push_back(std::filesystem::path(s));
    std::vector<std::filesystem::path> crtSources;
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, crtSources)) return 1;

    std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::filesystem::remove(libraryPath, ec);
    ec.clear();
    printf("Compiling native library: %s\n", libraryPath.string().c_str());
    int result = 1;
    std::vector<std::filesystem::path> cppCrtObjects;
    ScopedTemporaryDirectory             cppObjectDirectory;
    if (useCpp17 && compiler != CCompilerKind::Msvc && !crtSources.empty()) {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        if (!cppObjectDirectory.create(parent, ".mj2x-cpp-")) {
            CA_LOGT_ERROR("compile", "cannot create temporary C++ object directory.\n");
            return 1;
        }
        if (!compileCrtSourcesToObjects(
                crtSources, includes, compiler, cppObjectDirectory.path(), cppCrtObjects)) {
            return 1;
        }
    }
    else {
        sources.insert(sources.end(), crtSources.begin(), crtSources.end());
    }
    if (compiler == CCompilerKind::Msvc) {
#ifdef _WIN32
        ProcessEnvironment    environment;
        std::filesystem::path compilerPath;
        if (!loadMsvcEnvironment(environment, compilerPath)) return 1;
        ca::process::Command command(compilerPath.string());
        command.arg("/nologo").arg("/LD").arg("/utf-8");
        if (useCpp17) command.arg("/std:c++17");
        for (const auto& include : includes) command.arg("/I" + include.string());
        std::filesystem::path objectDir = libraryPath.parent_path();
        if (objectDir.empty()) objectDir = ".";
        objectDir /= "";
        command.arg("/Fo" + objectDir.string());
        for (const auto& source : sources) command.arg(source.string());
        command.arg("/Fe" + libraryPath.string());
        for (const auto& [key, value] : environment) command.env(key, value);
        result = runCompilerCommand(command, "MSVC");
#else
        CA_LOGT_ERROR("compile", "MSVC is only supported on Windows.\n");
#endif
    }
    else {
#ifdef __APPLE__
        const char* sharedFlag = "-dynamiclib";
#else
        const char* sharedFlag = "-shared";
#endif
        const char* executable = useCpp17 ? cppCompilerExecutable(compiler)
                                           : compilerExecutable(compiler);
        ca::process::Command command(executable);
        if (compiler == CCompilerKind::Zig) command.arg(useCpp17 ? "c++" : "cc");
        command.arg(sharedFlag);
        if (useCpp17) command.arg("-std=c++17");
#ifndef _WIN32
        command.arg("-fPIC").arg("-pthread");
#endif
        for (const auto& include : includes) command.arg("-I" + include.string());
        command.arg("-o").arg(libraryPath.string());
        for (const auto& source : sources) command.arg(source.string());
#ifndef _WIN32
        command.arg("-lm");
#endif
        for (const auto& object : cppCrtObjects) command.arg(object.string());
        result = runCompilerCommand(command, executable);
    }

    if (result != 0) {
        std::filesystem::remove(libraryPath, ec);
        return 1;
    }
    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        std::filesystem::remove(libraryPath, ec);
        CA_LOGT_ERROR("compile", "compiler succeeded without a native library: {}\n", libraryPath.string().c_str());
        return 1;
    }
    printf("Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                   const std::vector<std::string>& extraSources)
{
    return compileNativeSource(cOutputPath, outputPath, requestedCompiler, crtRoot, false,
                               extraSources);
}

int compileCppSource(const std::string& cppOutputPath, const std::string& outputPath,
                     CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                     const std::vector<std::string>& extraSources)
{
    return compileNativeSource(cppOutputPath, outputPath, requestedCompiler, crtRoot, true,
                               extraSources);
}

int compileCSourceFromPipe(std::string_view cSource, const std::string& outputPath,
                           CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                           const std::vector<std::string>& extraSources)
{
    if (cSource.empty()) {
        CA_LOGT_ERROR("compile", "generated C source is empty.\n");
        return 1;
    }

    CCompilerKind compiler = CCompilerKind::Auto;
    if (!detectCompiler(requestedCompiler, compiler)) {
        if (requestedCompiler == CCompilerKind::Auto) {
            CA_LOGT_ERROR("compile", "no supported C compiler found on PATH.\n");
        }
        else {
            CA_LOGT_ERROR("compile", "C compiler '{}' not found on PATH.\n", compilerExecutable(requestedCompiler));
        }
        return 1;
    }
    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes)) return 1;

    std::vector<std::filesystem::path> sources;
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, sources)) return 1;
    for (const auto& s : extraSources) sources.push_back(std::filesystem::path(s));

    const std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::error_code             ec;
    std::filesystem::remove(libraryPath, ec);
    ec.clear();
#ifdef _WIN32
    if (compiler == CCompilerKind::Msvc) {
        printf("Compiling native library with MSVC temporary source fallback: %s\n",
               libraryPath.string().c_str());
        const int rc =
            compileMsvcSourceFromTemporaryFile(cSource, libraryPath, includes, sources);
        if (rc != 0) return rc;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(libraryPath, ec) ||
            std::filesystem::file_size(libraryPath, ec) == 0) {
            CA_LOGT_ERROR("compile", "MSVC succeeded without a native library: {}\n", libraryPath.string().c_str());
            return 1;
        }
        printf("Native library written: %s\n", libraryPath.string().c_str());
        return 0;
    }
#endif
#ifdef __APPLE__
    const char* sharedFlag = "-dynamiclib";
#else
    const char* sharedFlag = "-shared";
#endif

    ca::process::Command command(compilerExecutable(compiler));
    if (compiler == CCompilerKind::Zig) command.arg("cc");
    command.arg(sharedFlag);
#ifndef _WIN32
    command.arg("-fPIC").arg("-pthread");
#endif
    for (const auto& include : includes) command.arg("-I" + include.string());
    command.arg("-o").arg(libraryPath.string());
    for (const auto& source : sources) command.arg(source.string());
#ifndef _WIN32
    command.arg("-lm");
#endif
    command.arg("-x").arg("c").arg("-");
    command.stdin(ca::process::Stdio::piped())
        .stdout(ca::process::Stdio::piped())
        .stderr(ca::process::Stdio::piped());

    printf("Compiling native library from stdin: %s\n", libraryPath.string().c_str());
    auto spawned = command.spawn();
    if (spawned.is_err()) {
        CA_LOGT_ERROR("compile", "cannot start C compiler: {}\n", spawned.unwrap_err().to_string().c_str());
        return 1;
    }

    auto child = std::move(spawned).unwrap();
    auto input = child.take_stdin();
    if (!input.has_value()) {
        child.kill();
        child.wait();
        CA_LOGT_ERROR("compile", "C compiler stdin pipe is unavailable.\n");
        return 1;
    }
    auto written = input->write_all(cSource.data(), cSource.size());
    input->close();
    if (written.is_err()) {
        auto completed = child.wait_with_output();
        if (completed.is_ok()) {
            auto output = std::move(completed).unwrap();
            writeProcessOutput(output);
        }
        CA_LOGT_ERROR("compile", "cannot write generated C source to compiler stdin: {}\n", written.to_string().c_str());
        return 1;
    }

    auto completed = child.wait_with_output();
    if (completed.is_err()) {
        CA_LOGT_ERROR("compile", "cannot wait for C compiler: {}\n", completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        CA_LOGT_ERROR("compile", "C compiler failed with exit code {}.\n", output.status.code);
        return 1;
    }

    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        CA_LOGT_ERROR("compile", "C compiler succeeded without a native library: {}\n", libraryPath.string().c_str());
        return 1;
    }
    printf("Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

int compileLlvmIrFromPipe(std::string_view llvmSource, const std::string& outputPath,
                          LlvmTool tool, bool withRuntimeProtection,
                          const std::filesystem::path& crtRoot)
{
    if (!jdk_finder::commandExists("clang")) {
        CA_LOGT_ERROR("compile", "LLVM IR compilation requires clang on PATH.\n");
        return 1;
    }
    if (tool == LlvmTool::Llc && !jdk_finder::commandExists("llc")) {
        CA_LOGT_ERROR("compile", "--llvm-tool llc requires llc on PATH.\n");
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::filesystem::remove(libraryPath, ec);
    ec.clear();

    std::vector<std::filesystem::path> crtIncludes;
    std::vector<std::filesystem::path> crtSources;
    if (!prepareLlvmCrtSources(crtRoot, crtIncludes, crtSources)) return 1;

#ifdef __APPLE__
    const char* sharedFlag = "-dynamiclib";
#else
    const char* sharedFlag = "-shared";
#endif

    // --runtime-protection：检查点实现（mj2x_rt_protect.c）随 .ll 一起编译。
    ScopedTemporaryDirectory rtProtectDirectory;
    std::filesystem::path    rtProtectSource;
    if (withRuntimeProtection) {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        if (!rtProtectDirectory.create(parent, ".mj2x-rt-protect-")) {
            CA_LOGT_ERROR("compile", "cannot create temporary rt_protect directory.\n");
            return 1;
        }
        rtProtectSource = writeRtProtectSource(rtProtectDirectory);
        if (rtProtectSource.empty()) {
            CA_LOGT_ERROR("compile",
                          "embedded mj2x-crt rt_protect sources are unavailable.\n");
            return 1;
        }
    }

    if (tool == LlvmTool::Clang) {
        // clang -shared -x ir - [-I ...] [-x c rt_protect.c] [-x c crt...] -o <lib>。
        printf("Compiling LLVM IR native library from stdin with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command command("clang");
        command.arg(sharedFlag).arg("-x").arg("ir").arg("-");
        for (const auto& include : crtIncludes) command.arg("-I").arg(include.string());
        if (!rtProtectSource.empty()) {
            command.arg("-x").arg("c").arg(rtProtectSource.string());
        }
        for (const auto& source : crtSources) {
            command.arg("-x").arg("c").arg(source.string());
        }
        command.arg("-o").arg(libraryPath.string());
        command.stdin(ca::process::Stdio::piped())
            .stdout(ca::process::Stdio::piped())
            .stderr(ca::process::Stdio::piped());
        auto spawned = command.spawn();
        if (spawned.is_err()) {
            CA_LOGT_ERROR("compile", "cannot start clang: {}\n",
                          spawned.unwrap_err().to_string().c_str());
            return 1;
        }
        auto child = std::move(spawned).unwrap();
        auto input = child.take_stdin();
        if (!input.has_value()) {
            child.kill();
            child.wait();
            CA_LOGT_ERROR("compile", "clang stdin pipe is unavailable.\n");
            return 1;
        }
        auto written = input->write_all(llvmSource.data(), llvmSource.size());
        input->close();
        if (written.is_err()) {
            auto completed = child.wait_with_output();
            if (completed.is_ok()) {
                auto output = std::move(completed).unwrap();
                writeProcessOutput(output);
            }
            CA_LOGT_ERROR("compile", "cannot write LLVM IR to clang stdin: {}\n",
                          written.to_string().c_str());
            return 1;
        }
        auto completed = child.wait_with_output();
        if (completed.is_err()) {
            CA_LOGT_ERROR("compile", "cannot wait for clang: {}\n",
                          completed.unwrap_err().to_string().c_str());
            return 1;
        }
        auto output = std::move(completed).unwrap();
        writeProcessOutput(output);
        if (!output.status.success()) {
            std::filesystem::remove(libraryPath, ec);
            CA_LOGT_ERROR("compile", "clang failed with exit code {}.\n", output.status.code);
            return 1;
        }
    }
    else {
        // llc -filetype=obj - -o <obj>（stdin），再由 clang 链接。
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        ScopedTemporaryDirectory objectDirectory;
        if (!objectDirectory.create(parent, ".mj2x-llvm-")) {
            CA_LOGT_ERROR("compile", "cannot create temporary LLVM object directory.\n");
            return 1;
        }
#ifdef _WIN32
        const std::filesystem::path objectPath = objectDirectory.path() / "native.obj";
#else
        const std::filesystem::path objectPath = objectDirectory.path() / "native.o";
#endif
        printf("Compiling LLVM IR object from stdin with llc: %s\n",
               objectPath.string().c_str());
        ca::process::Command llcCommand("llc");
        llcCommand.arg("-filetype=obj").arg("-").arg("-o").arg(objectPath.string());
        llcCommand.stdin(ca::process::Stdio::piped())
            .stdout(ca::process::Stdio::piped())
            .stderr(ca::process::Stdio::piped());
        auto llcSpawned = llcCommand.spawn();
        if (llcSpawned.is_err()) {
            CA_LOGT_ERROR("compile", "cannot start llc: {}\n",
                          llcSpawned.unwrap_err().to_string().c_str());
            return 1;
        }
        auto llcChild = std::move(llcSpawned).unwrap();
        auto llcInput = llcChild.take_stdin();
        if (!llcInput.has_value()) {
            llcChild.kill();
            llcChild.wait();
            CA_LOGT_ERROR("compile", "llc stdin pipe is unavailable.\n");
            return 1;
        }
        auto llcWritten = llcInput->write_all(llvmSource.data(), llvmSource.size());
        llcInput->close();
        if (llcWritten.is_err()) {
            llcChild.kill();
            llcChild.wait();
            CA_LOGT_ERROR("compile", "cannot write LLVM IR to llc stdin: {}\n",
                          llcWritten.to_string().c_str());
            return 1;
        }
        auto llcCompleted = llcChild.wait_with_output();
        if (llcCompleted.is_err()) {
            CA_LOGT_ERROR("compile", "cannot wait for llc: {}\n",
                          llcCompleted.unwrap_err().to_string().c_str());
            return 1;
        }
        auto llcOutput = std::move(llcCompleted).unwrap();
        writeProcessOutput(llcOutput);
        if (!llcOutput.status.success()) {
            CA_LOGT_ERROR("compile", "llc failed with exit code {}.\n", llcOutput.status.code);
            return 1;
        }

        std::vector<std::filesystem::path> linkInputs{objectPath};
        if (!rtProtectSource.empty()) {
            const std::filesystem::path rtObject =
                rtProtectDirectory.path() / "mj2x_rt_protect.obj";
            ca::process::Command rtCompile("clang");
            rtCompile.arg("-c").arg(rtProtectSource.string()).arg("-o").arg(rtObject.string());
            if (runCompilerCommand(rtCompile, "clang") != 0) return 1;
            linkInputs.push_back(rtObject);
        }
        for (size_t index = 0; index < crtSources.size(); ++index) {
            const std::filesystem::path crtObject =
                objectDirectory.path() / ("crt_" + std::to_string(index) + ".obj");
            ca::process::Command crtCompile("clang");
            crtCompile.arg("-c");
            for (const auto& include : crtIncludes) crtCompile.arg("-I").arg(include.string());
            crtCompile.arg(crtSources[index].string()).arg("-o").arg(crtObject.string());
            if (runCompilerCommand(crtCompile, "clang") != 0) return 1;
            linkInputs.push_back(crtObject);
        }

        printf("Linking LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command clangCommand("clang");
        clangCommand.arg(sharedFlag);
        for (const auto& input : linkInputs) clangCommand.arg(input.string());
        clangCommand.arg("-o").arg(libraryPath.string());
        if (runCompilerCommand(clangCommand, "clang") != 0) {
            std::filesystem::remove(libraryPath, ec);
            return 1;
        }
    }

    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        std::filesystem::remove(libraryPath, ec);
        CA_LOGT_ERROR("compile", "LLVM compiler succeeded without a native library: {}\n",
                      libraryPath.string().c_str());
        return 1;
    }
    printf("Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

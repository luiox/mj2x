// 宏值固化必须先于任何能到达 libca/process/subprocess.hpp 的 include：
// 本文件首个 include（native_compiler.hpp → native_process_timeout.hpp）
// 即传递引入 subprocess.hpp，其会 #undef Windows UCRT 的 stdout/stderr 宏
// （Command 同名成员防污染）。故 <cstdio> 与固化块置于所有项目头之前，
// 供文件尾部导出的 MSVC 环境加载函数（loadMsvcEnvironment）使用。
#include <cstdio>

namespace {
std::FILE* const kStdoutStream = stdout;
std::FILE* const kStderrStream = stderr;
}

#include "native_compiler.hpp"

#include "embedded_rt.hpp"

#include "libca/log/log_macros.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "jdk_finder/jdk_finder.hpp"
#include "libca/process/subprocess.hpp"
// 环境形状卫生函数簇（environmentKeyEquals/currentProcessEnvironment/
// isPathFamilyKey/dedupePathValue/mergeProcessEnvironment 等）：#1023 抽到
// 内部头供合成 env 断言 gtest 复用，本文件只保留 loadMsvcEnvironment 等本体。
// 宏值固化须在其前（见上），故 include 顺序在此而非文件头。
#include "native_env_hygiene.hpp"
// P1-4 外部进程超时（对标 jvmtp compiler.timeout）：libca 0.0.15 subprocess
// 超时基件的 mj2x 侧策略/文案单源。须在 kStdoutStream 固化之后 include
// （subprocess.hpp 会 #undef Windows UCRT 的 stdin/stdout/stderr 宏）。
#include "native_process_timeout.hpp"

namespace mj2x::cli {

// 导出给 native_project_builder（xmake 子进程复用 MSVC 环境）：加载
// INCLUDE/LIB/PATH，必要时调 vcvarsall.bat x64。自包含实现（宏值取自
// kStdoutStream/kStderrStream，其在 subprocess.hpp 之前已固化）。

bool loadMsvcEnvironment(ProcessEnvironment& environment,
                         std::filesystem::path& compilerPath,
                         ProcessTimeout timeout)
{
    const char* includeValue = std::getenv("INCLUDE");
    if (includeValue != nullptr && includeValue[0] != '\0') {
        compilerPath = "cl";
        return true;
    }

    const auto vcvars = jdk_finder::find_vcvars_all();
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
    auto captured = shell.output(outputOptionsWithTimeout(timeout));
    if (captured.is_err()) {
        if (isDeadlineExceeded(captured.unwrap_err())) {
            logProcessTimeout("MSVC environment setup (vcvarsall.bat)", timeout);
            return false;
        }
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

/// 把 stdin 写入放独立线程并立即返回：wait_with_output 才会并发排空
/// stdout/stderr。若在排空开始前同步 write_all，子进程消费 stdin 期间写满
/// 输出管道即双向死锁（大源码 + 多警告输出时必现窗口）。写失败（管道提前
/// 关闭等）不在此处上报——子进程的真实错误会经输出面暴露。
void spawnStdinWriter(ca::process::ChildStdin&& stdinWriter, std::string data)
{
    std::thread writer(
        [handle = std::move(stdinWriter), payload = std::move(data)]() mutable {
            handle.write_all(payload.data(), payload.size());
        });
    writer.detach();
}

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
    case CCompilerKind::Msvc:  return jdk_finder::CCompilerKind::MSVC;
    case CCompilerKind::Gcc:   return jdk_finder::CCompilerKind::GCC;
    case CCompilerKind::Clang: return jdk_finder::CCompilerKind::CLANG;
    case CCompilerKind::Zig:   return jdk_finder::CCompilerKind::ZIG;
    case CCompilerKind::Auto:  break;
    }
    return jdk_finder::CCompilerKind::AUTO;
}

CCompilerKind fromFinderKind(jdk_finder::CCompilerKind kind)
{
    switch (kind) {
    case jdk_finder::CCompilerKind::MSVC:  return CCompilerKind::Msvc;
    case jdk_finder::CCompilerKind::GCC:   return CCompilerKind::Gcc;
    case jdk_finder::CCompilerKind::CLANG: return CCompilerKind::Clang;
    case jdk_finder::CCompilerKind::ZIG:   return CCompilerKind::Zig;
    case jdk_finder::CCompilerKind::AUTO:  break;
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
                   std::vector<std::filesystem::path>& sources,
                   CEmitterBackend                     backend)
{
    // 磁盘源包(mj2x-rt 布局):递归收集 src/**/*.c 中属于该后端分发面的
    // 挂载子集(rtFoldersForBackend),不再依赖清单格式。
    const auto sourceRoot = crtRoot / "src";
    std::error_code ec;
    if (!std::filesystem::exists(sourceRoot / "regc" / "mj2x_regc_crt.h", ec)) {
        CA_LOGT_ERROR("compile", "invalid mj2x-rt source pack: {}\n", crtRoot.string().c_str());
        return false;
    }

    const auto mounts = mj2x::cli::rtFoldersForBackend(backend);
    includes.push_back(sourceRoot);
    bool foundSource = false;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(sourceRoot, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto relative = entry.path().lexically_relative(sourceRoot);
        std::string mount    = relative.string();
        const auto separator = mount.find_first_of("/\\");
        if (separator != std::string::npos) mount = mount.substr(0, separator);
        if (std::find(mounts.begin(), mounts.end(), std::string_view{mount}) == mounts.end()) {
            continue;
        }
        const auto extension = entry.path().extension().string();
        if (extension == ".cpp") {
            CA_LOGT_ERROR("compile",
                          "--compile does not yet support C++ mj2x-rt sources; use "
                          "--generate-xmake.\n");
            return false;
        }
        if (extension != ".c") continue;
        sources.push_back(entry.path());
        foundSource = true;
    }
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot iterate mj2x-rt source pack: {}\n",
                      ec.message().c_str());
        return false;
    }

    if (!foundSource) {
        CA_LOGT_ERROR("compile", "mj2x-rt source pack has no C sources.\n");
        return false;
    }
    return true;
}

bool canUseMsvc()
{
    auto tc = jdk_finder::find_native_compiler(jdk_finder::CCompilerKind::MSVC);
    return tc.available;
}

bool detectCompiler(CCompilerKind requested, CCompilerKind& detected)
{
    auto tc = jdk_finder::find_native_compiler(toFinderKind(requested));
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

bool jniIncludePaths(std::vector<std::filesystem::path>& includes,
                     const char* platformOverride = nullptr)
{
    jdk_finder::FindOptions opts;
    opts.require_jni_header = true;
    auto jdk = jdk_finder::find_jdk(opts);
    if (!jdk.available) {
        CA_LOGT_ERROR("compile", "{}; required to locate JNI headers.\n", jdk.reason.c_str());
        return false;
    }
    includes.push_back(jdk.jdk_include);
    if (platformOverride == nullptr) {
        includes.push_back(jdk.jdk_include_plat);
    }
    else {
        // 交叉目标（issue #11）：jni_md.h 按目标 ABI 的平台子目录取
        // （win32 的 jlong 是 long long，linux 是 long——交叉编译必须用目标的）。
        // jdkInclude 本身就是 <jdk>/include 目录，平台子目录是其直接子目录。
        includes.push_back(jdk.jdk_include / platformOverride);
    }
    return true;
}

std::filesystem::path findVcvarsAll()
{
    return jdk_finder::find_vcvars_all();
}

void writeProcessOutput(const ca::process::Output& output)
{
    if (!output.stdout_data.empty())
        fwrite(output.stdout_data.data(), 1, output.stdout_data.size(), kStdoutStream);
    if (!output.stderr_data.empty())
        fwrite(output.stderr_data.data(), 1, output.stderr_data.size(), kStderrStream);
}

int runCompilerCommand(ca::process::Command& command, const char* compilerName,
                       ProcessTimeout timeout)
{
    auto completed = command.output(outputOptionsWithTimeout(timeout));
    if (completed.is_err()) {
        if (isDeadlineExceeded(completed.unwrap_err())) {
            logProcessTimeout(compilerName, timeout);
            return 1;
        }
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

/// 运行时源后端宏注入(embedded_rt.hpp rtBackendDefine 契约):MSVC 用 /D,
/// 其余用 -D;define 体为空时为空操作。作用于整个编译命令——生成源、
/// mj2x-rt 运行时源与用户 intrinsic 同一定义面。
void appendRtBackendDefine(ca::process::Command& command, bool msvcStyle,
                           CEmitterBackend backend)
{
    const std::string_view define = mj2x::cli::rtBackendDefine(backend);
    if (define.empty()) return;
    command.arg(std::string{msvcStyle ? "/D" : "-D"} + std::string{define});
}

bool compileCrtSourcesToObjects(const std::vector<std::filesystem::path>& sources,
                                const std::vector<std::filesystem::path>& includes,
                                CCompilerKind compiler, CEmitterBackend backend,
                                const std::filesystem::path& objectDirectory,
                                std::vector<std::filesystem::path>& objects,
                                ProcessTimeout timeout)
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
            appendRtBackendDefine(command, true, backend);
            for (const auto& include : includes) command.arg("/I" + include.string());
            command.arg("/Fo" + object.string());
            command.arg(sources[index].string());
        }
        else {
            command.arg("-c");
            appendRtBackendDefine(command, false, backend);
#ifndef _WIN32
            command.arg("-fPIC");
#endif
            for (const auto& include : includes) command.arg("-I" + include.string());
            command.arg(sources[index].string()).arg("-o").arg(object.string());
        }
        if (runCompilerCommand(command, compilerExecutable(compiler), timeout) != 0)
            return false;
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

// environmentKeyEquals 用 native_env_hygiene.hpp 的 inline 版本（#1023 起）：
// 此前的匿名 namespace 副本与它在 mergeProcessEnvironment 的调用点上
// 产生过 C2668 二义性，实现逐字节相同，统一走头文件一处。

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

int compileMsvcSourceFromTemporaryFile(std::string_view                           cSource,
                                       const std::filesystem::path&               libraryPath,
                                       const std::vector<std::filesystem::path>& includes,
                                       const std::vector<std::filesystem::path>& sources,
                                       CEmitterBackend backend,
                                       ProcessTimeout timeout)
{
    ProcessEnvironment    environment;
    std::filesystem::path compilerPath;
    if (!loadMsvcEnvironment(environment, compilerPath, timeout)) return 1;

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
    appendRtBackendDefine(command, true, backend);
    for (const auto& include : includes) command.arg("/I" + include.string());
    // 中间件（.obj/.lib/.exp）走 scoped 临时目录：产物目录只留 .dll（多目标
    // 的 <abi>/ 是产品布局，不收编译中间件）。
    std::filesystem::path objectParent = libraryPath.parent_path();
    if (objectParent.empty()) objectParent = ".";
    ScopedTemporaryDirectory objectDirectory;
    if (!objectDirectory.create(objectParent, ".mj2x-obj-")) {
        CA_LOGT_ERROR("compile", "cannot create temporary MSVC object directory.\n");
        return 1;
    }
    std::filesystem::path objectDir = objectDirectory.path();
    objectDir /= "";
    command.arg("/Fo" + objectDir.string());
    command.arg(sourceFile.path().string());
    for (const auto& source : sources) command.arg(source.string());
    command.arg("/Fe" + libraryPath.string());
    command.arg("/link").arg("/IMPLIB:" + (objectDirectory.path() / "mj2x_native.lib").string());
    mergeProcessEnvironment(command, environment);
    command.stdout(ca::process::Stdio::piped()).stderr(ca::process::Stdio::piped());

    auto completed = command.output(outputOptionsWithTimeout(timeout));
    if (completed.is_err()) {
        if (isDeadlineExceeded(completed.unwrap_err())) {
            logProcessTimeout("MSVC compiler", timeout);
            return 1;
        }
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

/// 多目标按 ABI 编译的共享实现（issue #11）：file 模式给 sourcePath（pipeSource
/// 为空）、pipe 模式给 pipeSource（sourcePath 忽略）。编译器解析（显式路径 >
/// PATH 探测）、JNI 平台子目录、shared/PIC/链接 flag、zig -target 全部由
/// target ABI 驱动，而非宿主宏。
int compileForTargetImpl(const std::string& sourcePath, const std::string* pipeSource,
                         const CompileTargetSpec& target, CEmitterBackend backend,
                         CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                         const std::vector<std::string>& extraSources, ProcessTimeout timeout)
{
    const TargetAbiInfo& info = targetAbiInfo(target.abi);

    CCompilerKind compiler = CCompilerKind::Auto;
    std::string   executable;
    if (!target.compilerPath.empty()) {
        if (!inferCompilerKindFromPath(std::filesystem::path(target.compilerPath), compiler)) {
            CA_LOGT_ERROR("compile",
                          "cannot infer compiler kind from '{}' (expected a cl/zig/gcc/clang "
                          "executable name).\n",
                          target.compilerPath.c_str());
            return 1;
        }
        executable = target.compilerPath;
    }
    else {
        if (!detectCompiler(requestedCompiler, compiler)) {
            if (requestedCompiler == CCompilerKind::Auto) {
                CA_LOGT_ERROR("compile", "no supported C compiler found on PATH.\n");
            }
            else {
                CA_LOGT_ERROR("compile", "C compiler '{}' not found on PATH.\n",
                              compilerExecutable(requestedCompiler));
            }
            return 1;
        }
        executable = compilerExecutable(compiler);
    }

    if (compiler == CCompilerKind::Msvc) {
#if defined(_WIN32)
        if (target.abi != TargetAbi::WindowsX64) {
            CA_LOGT_ERROR("compile", "MSVC can only target windows-x64 (got {}); use a "
                                     "zig/gcc cross toolchain via --target-cc.\n",
                          info.id);
            return 1;
        }
#else
        (void)info;
        CA_LOGT_ERROR("compile", "MSVC is only supported on Windows hosts; use a zig/gcc "
                                 "cross toolchain via --target-cc for cross targets.\n");
        return 1;
#endif
    }

    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes, info.jniPlatform)) return 1;

    std::vector<std::filesystem::path> crtSources;
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, crtSources, backend)) return 1;
    for (const auto& s : extraSources) crtSources.push_back(std::filesystem::path(s));

    const std::filesystem::path libraryPath = target.libraryPath;
    std::error_code             ec;
    std::filesystem::create_directories(libraryPath.parent_path(), ec);
    ec.clear();
    std::filesystem::remove(libraryPath, ec);
    ec.clear();

    int result = 1;
#ifdef _WIN32
    if (compiler == CCompilerKind::Msvc) {
        if (pipeSource != nullptr) {
            printf("Compiling native library with MSVC temporary source fallback: %s\n",
                   libraryPath.string().c_str());
            result = compileMsvcSourceFromTemporaryFile(*pipeSource, libraryPath, includes,
                                                        crtSources, backend, timeout);
        }
        else {
            ProcessEnvironment    environment;
            std::filesystem::path compilerPath;
            if (!loadMsvcEnvironment(environment, compilerPath, timeout)) return 1;
            // 中间件走 scoped 临时目录（同 pipe 形态：产物目录只留 .dll）。
            std::filesystem::path objectParent = libraryPath.parent_path();
            if (objectParent.empty()) objectParent = ".";
            ScopedTemporaryDirectory objectDirectory;
            if (!objectDirectory.create(objectParent, ".mj2x-obj-")) {
                CA_LOGT_ERROR("compile", "cannot create temporary MSVC object directory.\n");
                return 1;
            }
            ca::process::Command command(compilerPath.string());
            command.arg("/nologo").arg("/LD").arg("/TC").arg("/utf-8");
            appendRtBackendDefine(command, true, backend);
            for (const auto& include : includes) command.arg("/I" + include.string());
            std::filesystem::path objectDir = objectDirectory.path();
            objectDir /= "";
            command.arg("/Fo" + objectDir.string());
            command.arg(std::filesystem::path(sourcePath).string());
            command.arg("/Fe" + libraryPath.string());
            command.arg("/link").arg(
                "/IMPLIB:" + (objectDirectory.path() / "mj2x_native.lib").string());
            mergeProcessEnvironment(command, environment);
            printf("Compiling native library: %s\n", libraryPath.string().c_str());
            result = runCompilerCommand(command, "MSVC", timeout);
        }
    }
    else
#endif
    {
        // 目标 ABI 决定 flag 面：windows-gnu 目标不要 -fPIC/-pthread/-lm；
        // linux/android 目标与宿主 POSIX 形态一致。
        ca::process::Command command(executable);
        if (compiler == CCompilerKind::Zig) {
            command.arg("cc");
            command.arg("-target").arg(info.zigTriple);
        }
        command.arg("-shared");
        appendRtBackendDefine(command, false, backend);
        const bool posixTarget = (target.abi != TargetAbi::WindowsX64);
        if (posixTarget) command.arg("-fPIC").arg("-pthread");
        for (const auto& include : includes) command.arg("-I" + include.string());
        command.arg("-o").arg(libraryPath.string());
        if (pipeSource != nullptr) {
            for (const auto& source : crtSources) command.arg(source.string());
            command.arg("-x").arg("c").arg("-");
            command.stdin(ca::process::Stdio::piped())
                .stdout(ca::process::Stdio::piped())
                .stderr(ca::process::Stdio::piped());
            printf("Compiling native library from stdin: %s\n", libraryPath.string().c_str());
            auto spawned = command.spawn();
            if (spawned.is_err()) {
                CA_LOGT_ERROR("compile", "cannot start C compiler: {}\n",
                              spawned.unwrap_err().to_string().c_str());
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
            // stdin 写入放独立线程（与宿主 pipe 形态同契约：wait 并发排空
            // stdout/stderr，同步 write_all 在大源码下双向死锁）。
            spawnStdinWriter(std::move(*input), *pipeSource);
            ca::process::Output output;
            if (!waitChildWithTimeout(child, "C compiler", timeout, output)) return 1;
            writeProcessOutput(output);
            if (!output.status.success()) {
                CA_LOGT_ERROR("compile", "C compiler failed with exit code {}.\n",
                              output.status.code);
                result = 1;
            }
            else {
                result = 0;
            }
        }
        else {
            command.arg(std::filesystem::path(sourcePath).string());
            if (posixTarget) command.arg("-lm");
            printf("Compiling native library: %s\n", libraryPath.string().c_str());
            result = runCompilerCommand(command, executable.c_str(), timeout);
        }
    }

    if (result != 0) {
        std::filesystem::remove(libraryPath, ec);
        return 1;
    }
    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        std::filesystem::remove(libraryPath, ec);
        CA_LOGT_ERROR("compile", "compiler succeeded without a native library: {}\n",
                      libraryPath.string().c_str());
        return 1;
    }
    printf("Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

}   // namespace

bool parseCompilerName(const std::string& name, CCompilerKind& compiler)
{
    // "auto" 是 config_file.md 记载的默认值形态：显式写 auto = 回到自动探测。
    if (name == "auto") {
        compiler = CCompilerKind::Auto;
        return true;
    }
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

// ===========================================================================
// 多目标 ABI 注册表（issue #11，--targets）。宿主 ABI 由编译期平台宏推导；
// 交叉目标经 --target-cc 显式工具链（issue 方案 (a)），zig 恒带 -target 三元组。
// ===========================================================================

bool parseTargetAbi(const std::string& id, TargetAbi& out)
{
    if (id == "windows-x64") { out = TargetAbi::WindowsX64; return true; }
    if (id == "linux-x64") { out = TargetAbi::LinuxX64; return true; }
    if (id == "linux-aarch64") { out = TargetAbi::LinuxAarch64; return true; }
    if (id == "android-arm64") { out = TargetAbi::AndroidArm64; return true; }
    return false;
}

const TargetAbiInfo& targetAbiInfo(TargetAbi abi)
{
    static const TargetAbiInfo kRegistry[] = {
        {"windows-x64", ".dll", "win32", "x86_64-windows-gnu"},
        {"linux-x64", ".so", "linux", "x86_64-linux-gnu"},
        {"linux-aarch64", ".so", "linux", "aarch64-linux-gnu"},
        {"android-arm64", ".so", "linux", "aarch64-linux-android"},
    };
    return kRegistry[static_cast<size_t>(abi)];
}

std::string targetAbiIdList()
{
    std::string out;
    for (const TargetAbi v :
         {TargetAbi::WindowsX64, TargetAbi::LinuxX64, TargetAbi::LinuxAarch64,
          TargetAbi::AndroidArm64}) {
        if (!out.empty()) out += ", ";
        out += targetAbiInfo(v).id;
    }
    return out;
}

TargetAbi hostTargetAbi()
{
#if defined(_WIN32)
    return TargetAbi::WindowsX64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return TargetAbi::LinuxAarch64;   // darwin-arm64 不在 v1 注册表；按 arm64 归 linux 档
#else
    return TargetAbi::LinuxX64;
#endif
}

bool inferCompilerKindFromPath(const std::filesystem::path& executable, CCompilerKind& out)
{
    const std::string name = executable.filename().string();
    const std::string lower = [=] {
        std::string s = name;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    if (lower.rfind("cl", 0) == 0) { out = CCompilerKind::Msvc; return true; }
    if (lower.rfind("zig", 0) == 0) { out = CCompilerKind::Zig; return true; }
    if (lower.find("clang") != std::string::npos) { out = CCompilerKind::Clang; return true; }
    if (lower.find("gcc") != std::string::npos || lower.find("g++") != std::string::npos ||
        lower == "cc") {
        out = CCompilerKind::Gcc;
        return true;
    }
    return false;
}

int compileCSourceForTarget(const std::string& cOutputPath, const CompileTargetSpec& target,
                            CEmitterBackend backend, CCompilerKind requestedCompiler,
                            const std::filesystem::path& crtRoot,
                            const std::vector<std::string>& extraSources, ProcessTimeout timeout)
{
    return compileForTargetImpl(cOutputPath, nullptr, target, backend, requestedCompiler, crtRoot,
                                extraSources, timeout);
}

bool probeHostCompiler(CCompilerKind requested)
{
    CCompilerKind detected = CCompilerKind::Auto;
    return detectCompiler(requested, detected);
}

std::string_view compilerKindName(CCompilerKind kind)
{
    switch (kind) {
    case CCompilerKind::Msvc: return "msvc";
    case CCompilerKind::Gcc: return "gcc";
    case CCompilerKind::Clang: return "clang";
    case CCompilerKind::Zig: return "zig";
    case CCompilerKind::Auto: break;
    }
    return {};
}

int compileCSourceFromPipeForTarget(std::string_view cSource, const CompileTargetSpec& target,
                                    CEmitterBackend backend, CCompilerKind requestedCompiler,
                                    const std::filesystem::path& crtRoot,
                                    const std::vector<std::string>& extraSources,
                                    ProcessTimeout timeout)
{
    if (cSource.empty()) {
        CA_LOGT_ERROR("compile", "generated C source is empty.\n");
        return 1;
    }
    std::string buffer(cSource);
    return compileForTargetImpl({}, &buffer, target, backend, requestedCompiler, crtRoot,
                                extraSources, timeout);
}

/// @brief 把内嵌 mj2x-rt common(rt_protect)源写入临时目录；返回 .c 路径（空 = 失败）。
/// @details rt_protect 的 .h/.c 必须同目录（.c 相对 include）；临时目录生命周期
/// 由调用方（ScopedTemporaryDirectory）保证覆盖整个编译过程。
std::filesystem::path writeRtProtectSource(ScopedTemporaryDirectory& directory)
{
    const auto* bundle = mj2x::cli::rtBundle("common");
    if (bundle == nullptr) return {};
    const auto header = bundle->get("/mj2x_rt_protect.h");
    const auto source = bundle->get("/mj2x_rt_protect.c");
    if (header.is_err() || source.is_err()) return {};
    const auto headerPath = directory.path() / "mj2x_rt_protect.h";
    const auto sourcePath = directory.path() / "mj2x_rt_protect.c";
    {
        std::ofstream out(headerPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.unwrap().data()),
                  static_cast<std::streamsize>(header.unwrap().size()));
    }
    {
        std::ofstream out(sourcePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(source.unwrap().data()),
                  static_cast<std::streamsize>(source.unwrap().size()));
    }
    if (!std::filesystem::is_regular_file(sourcePath)) return {};
    return sourcePath;
}

/// @brief 准备 LLVM 编译侧的 mj2x-rt 附加源（JDK intrinsic 直调符号如
/// mj2x_math_*，实现见 src/regc/mj2x_regc_intrinsic.c）。
/// @return false 表示 crtRoot 非空但源包/JNI include 无效（调用方应报错）。
bool prepareLlvmCrtSources(const std::filesystem::path&        crtRoot,
                           std::vector<std::filesystem::path>& includes,
                           std::vector<std::filesystem::path>& sources)
{
    if (crtRoot.empty()) return true;
    std::vector<std::filesystem::path> manifestSources;
    if (!addCrtSources(crtRoot, includes, manifestSources, CEmitterBackend::LlvmIr)) return false;
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
                        const std::filesystem::path& crtRoot,
                        ProcessTimeout timeout)
{
    std::error_code ec;
    const std::filesystem::path sourcePath(llvmOutputPath);
    if (!std::filesystem::is_regular_file(sourcePath, ec)) {
        CA_LOGT_ERROR("compile", "emitted LLVM IR source does not exist: {}\n", llvmOutputPath.c_str());
        return 1;
    }
    if (!jdk_finder::command_exists("clang")) {
        CA_LOGT_ERROR("compile", "LLVM IR compilation requires clang on PATH.\n");
        return 1;
    }
    if (tool == LlvmTool::Llc && !jdk_finder::command_exists("llc")) {
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
                          "embedded mj2x-rt rt_protect sources are unavailable.\n");
            return 1;
        }
    }

    int result = 1;
    if (tool == LlvmTool::Clang) {
        printf("Compiling LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command command("clang");
        command.arg(sharedFlag).arg(sourcePath.string());
#ifndef _WIN32
        // Linux 下链接 .so 必须带 -fPIC：随 .ll 一起编的 C 运行时含 TLS
        // （_mj2x_regc_ctx 等），非 PIC 编译走 initial-exec 模型，产生
        // R_X86_64_TPOFF32 重定位，链接共享对象直接失败（recompile with -fPIC）。
        command.arg("-fPIC");
#endif
        appendRtBackendDefine(command, false, CEmitterBackend::LlvmIr);
        for (const auto& include : crtIncludes) command.arg("-I").arg(include.string());
        if (!rtProtectSource.empty()) command.arg(rtProtectSource.string());
        for (const auto& source : crtSources) command.arg(source.string());
        command.arg("-o").arg(libraryPath.string());
        result = runCompilerCommand(command, "clang", timeout);
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
#ifndef _WIN32
        llcCommand.arg("-relocation-model=pic");
#endif
        llcCommand.arg("-filetype=obj").arg(sourcePath.string()).arg("-o").arg(
            objectPath.string());
        if (runCompilerCommand(llcCommand, "llc", timeout) != 0) return 1;

        std::vector<std::filesystem::path> linkInputs{objectPath};
        if (!rtProtectSource.empty()) {
            const std::filesystem::path rtObject =
                rtProtectDirectory.path() / "mj2x_rt_protect.obj";
            ca::process::Command rtCompile("clang");
            rtCompile.arg("-c");
            appendRtBackendDefine(rtCompile, false, CEmitterBackend::LlvmIr);
#ifndef _WIN32
            rtCompile.arg("-fPIC"); // 产物进 .so，见 compileLlvmIr 侧的 TPOFF32 说明。
#endif
            rtCompile.arg(rtProtectSource.string()).arg("-o").arg(rtObject.string());
            if (runCompilerCommand(rtCompile, "clang", timeout) != 0) return 1;
            linkInputs.push_back(rtObject);
        }
        for (size_t index = 0; index < crtSources.size(); ++index) {
            const std::filesystem::path crtObject =
                objectDirectory.path() / ("crt_" + std::to_string(index) + ".obj");
            ca::process::Command crtCompile("clang");
            crtCompile.arg("-c");
            appendRtBackendDefine(crtCompile, false, CEmitterBackend::LlvmIr);
#ifndef _WIN32
            crtCompile.arg("-fPIC"); // 产物进 .so，见 compileLlvmIr 侧的 TPOFF32 说明。
#endif
            for (const auto& include : crtIncludes) crtCompile.arg("-I").arg(include.string());
            crtCompile.arg(crtSources[index].string()).arg("-o").arg(crtObject.string());
            if (runCompilerCommand(crtCompile, "clang", timeout) != 0) return 1;
            linkInputs.push_back(crtObject);
        }

        printf("Linking LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command clangCommand("clang");
        clangCommand.arg(sharedFlag);
        for (const auto& input : linkInputs) clangCommand.arg(input.string());
        clangCommand.arg("-o").arg(libraryPath.string());
        result = runCompilerCommand(clangCommand, "clang", timeout);
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
                        CEmitterBackend backend, CCompilerKind requestedCompiler,
                        const std::filesystem::path& crtRoot, bool useCpp17,
                        const std::vector<std::string>& extraSources,
                        ProcessTimeout timeout)
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
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, crtSources, backend)) return 1;

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
                crtSources, includes, compiler, backend, cppObjectDirectory.path(),
                cppCrtObjects, timeout)) {
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
        if (!loadMsvcEnvironment(environment, compilerPath, timeout)) return 1;
        ca::process::Command command(compilerPath.string());
        command.arg("/nologo").arg("/LD").arg("/utf-8");
        if (useCpp17) command.arg("/std:c++17");
        appendRtBackendDefine(command, true, backend);
        for (const auto& include : includes) command.arg("/I" + include.string());
        std::filesystem::path objectDir = libraryPath.parent_path();
        if (objectDir.empty()) objectDir = ".";
        objectDir /= "";
        command.arg("/Fo" + objectDir.string());
        for (const auto& source : sources) command.arg(source.string());
        command.arg("/Fe" + libraryPath.string());
        mergeProcessEnvironment(command, environment);
        result = runCompilerCommand(command, "MSVC", timeout);
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
        appendRtBackendDefine(command, false, backend);
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
        result = runCompilerCommand(command, executable, timeout);
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
                   CEmitterBackend backend, CCompilerKind requestedCompiler,
                   const std::filesystem::path& crtRoot,
                   const std::vector<std::string>& extraSources,
                   ProcessTimeout timeout)
{
    return compileNativeSource(cOutputPath, outputPath, backend, requestedCompiler, crtRoot,
                               false, extraSources, timeout);
}

int compileCppSource(const std::string& cppOutputPath, const std::string& outputPath,
                     CEmitterBackend backend, CCompilerKind requestedCompiler,
                     const std::filesystem::path& crtRoot,
                     const std::vector<std::string>& extraSources,
                     ProcessTimeout timeout)
{
    return compileNativeSource(cppOutputPath, outputPath, backend, requestedCompiler, crtRoot,
                               true, extraSources, timeout);
}

int compileCSourceFromPipe(std::string_view cSource, const std::string& outputPath,
                           CEmitterBackend backend, CCompilerKind requestedCompiler,
                           const std::filesystem::path& crtRoot,
                           const std::vector<std::string>& extraSources,
                           ProcessTimeout timeout)
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
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, sources, backend)) return 1;
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
            compileMsvcSourceFromTemporaryFile(cSource, libraryPath, includes, sources, backend,
                                               timeout);
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
    appendRtBackendDefine(command, false, backend);
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
    // stdin 写入放独立线程：wait_with_output 会并发排空 stdout/stderr，若在
    // 排空开始前同步 write_all，编译器在消费 stdin 期间写满输出管道即双向
    // 死锁（大源码 + 多警告输出时必现窗口）。
    spawnStdinWriter(std::move(*input), std::string(cSource));

    // 超时路径：helper 内部收割进程树并收尸后返回 false（已记录超时日志）；
    // 输出无条件先落盘——卡死前编译器吐出的部分告警对定位源码有价值。
    ca::process::Output output;
    if (!waitChildWithTimeout(child, "C compiler", timeout, output)) return 1;
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
                          const std::filesystem::path& crtRoot,
                          ProcessTimeout timeout)
{
    if (!jdk_finder::command_exists("clang")) {
        CA_LOGT_ERROR("compile", "LLVM IR compilation requires clang on PATH.\n");
        return 1;
    }
    if (tool == LlvmTool::Llc && !jdk_finder::command_exists("llc")) {
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
                          "embedded mj2x-rt rt_protect sources are unavailable.\n");
            return 1;
        }
    }

    if (tool == LlvmTool::Clang) {
        // clang -shared -x ir - [-I ...] [-x c rt_protect.c] [-x c crt...] -o <lib>。
        printf("Compiling LLVM IR native library from stdin with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command command("clang");
        command.arg(sharedFlag).arg("-x").arg("ir").arg("-");
#ifndef _WIN32
        // Linux 下链接 .so 必须带 -fPIC：C 运行时的 TLS（_mj2x_regc_ctx 等）
        // 非 PIC 编译走 initial-exec 模型，产生 R_X86_64_TPOFF32 重定位，
        // 链接共享对象直接失败（recompile with -fPIC）。
        command.arg("-fPIC");
#endif
        appendRtBackendDefine(command, false, CEmitterBackend::LlvmIr);
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
        spawnStdinWriter(std::move(*input), std::string(llvmSource));
        ca::process::Output output;
        if (!waitChildWithTimeout(child, "clang", timeout, output)) return 1;
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
#ifndef _WIN32
        llcCommand.arg("-relocation-model=pic");
#endif
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
        spawnStdinWriter(std::move(*llcInput), std::string(llvmSource));
        ca::process::Output llcOutput;
        if (!waitChildWithTimeout(llcChild, "llc", timeout, llcOutput)) return 1;
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
            rtCompile.arg("-c");
            appendRtBackendDefine(rtCompile, false, CEmitterBackend::LlvmIr);
#ifndef _WIN32
            rtCompile.arg("-fPIC"); // 产物进 .so，见 compileLlvmIr 侧的 TPOFF32 说明。
#endif
            rtCompile.arg(rtProtectSource.string()).arg("-o").arg(rtObject.string());
            if (runCompilerCommand(rtCompile, "clang", timeout) != 0) return 1;
            linkInputs.push_back(rtObject);
        }
        for (size_t index = 0; index < crtSources.size(); ++index) {
            const std::filesystem::path crtObject =
                objectDirectory.path() / ("crt_" + std::to_string(index) + ".obj");
            ca::process::Command crtCompile("clang");
            crtCompile.arg("-c");
            appendRtBackendDefine(crtCompile, false, CEmitterBackend::LlvmIr);
#ifndef _WIN32
            crtCompile.arg("-fPIC"); // 产物进 .so，见 compileLlvmIr 侧的 TPOFF32 说明。
#endif
            for (const auto& include : crtIncludes) crtCompile.arg("-I").arg(include.string());
            crtCompile.arg(crtSources[index].string()).arg("-o").arg(crtObject.string());
            if (runCompilerCommand(crtCompile, "clang", timeout) != 0) return 1;
            linkInputs.push_back(crtObject);
        }

        printf("Linking LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command clangCommand("clang");
        clangCommand.arg(sharedFlag);
        for (const auto& input : linkInputs) clangCommand.arg(input.string());
        clangCommand.arg("-o").arg(libraryPath.string());
        if (runCompilerCommand(clangCommand, "clang", timeout) != 0) {
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

// 宏值固化必须先于任何能到达 libca/process/subprocess.hpp 的 include：
// 本文件首个 include（native_project_generator.hpp → native_compiler.hpp →
// native_process_timeout.hpp）即传递引入 subprocess.hpp，其会 #undef
// Windows UCRT 的 stdout/stderr 宏（Command 同名成员防污染）。故 <cstdio>
// 与固化块置于所有项目头之前，供下方 writeProcessOutput 使用。
#include <cstdio>

namespace {
std::FILE* const kStdoutStream = stdout;
std::FILE* const kStderrStream = stderr;
}

#include "native_project_generator.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

#include "libca/process/subprocess.hpp"
#include "libca/log/log_macros.hpp"
// 环境形状卫生函数簇（mergeProcessEnvironment/environmentKeyEquals/
// isAbnormalChildExit/ProcessEnvironment 别名）声明与定义统一走该内部头
// （#1023 自本文件与 native_compiler.cpp 抽出）；宏值固化须在其前（见上）。
#include "native_env_hygiene.hpp"
// P1-4 外部进程超时：xmake 站点走 spawn + waitChildWithTimeout（超时先
// taskkill /T /F 收割进程树再收尸——xmake 拉起的 cl.exe/link.exe 继承管道
// 写端，只杀 xmake 会让收尸的 EOF 等待永不到达）。须在 kStdoutStream 固化
// 之后 include（subprocess.hpp 会 #undef Windows UCRT 的 stdin/stdout/stderr）。
#include "native_process_timeout.hpp"

namespace mj2x::cli {

// native_compiler.cpp 提供的外部链接实现（无头文件声明）：加载 MSVC 编译环境
// （INCLUDE/LIB/PATH，必要时调 vcvarsall.bat x64）。xmake 子进程复用同一环境，
// 否则在 PATH 含 mingw64 的 shell（Git Bash）下 xmake 会探测 mingw 工具链导致
// "cannot get program for cc"。timeout 同时作用于 vcvarsall.bat 探针（P1-4）。
bool loadMsvcEnvironment(ProcessEnvironment& environment, std::filesystem::path& compilerPath,
                         ProcessTimeout timeout);

namespace fs = std::filesystem;
namespace {

void writeProcessOutput(const ca::process::Output& output)
{
    if (!output.stdout_data.empty())
        fwrite(output.stdout_data.data(), 1, output.stdout_data.size(), kStdoutStream);
    if (!output.stderr_data.empty())
        fwrite(output.stderr_data.data(), 1, output.stderr_data.size(), kStderrStream);
}

bool runXmake(ca::process::Command& command, const char* phase, int* failedCode,
              ProcessTimeout timeout)
{
    auto spawned = command.spawn();
    if (spawned.is_err()) {
        CA_LOGT_ERROR("compile", "cannot start xmake for {}: {}\n", phase,
                spawned.unwrap_err().to_string().c_str());
        return false;
    }
    auto child = std::move(spawned).unwrap();
    ca::process::Output output;
    // 超时路径在 helper 内部：先 taskkill /T /F 收割整树（xmake 的 cl.exe/
    // link.exe 孙进程继承管道写端）再收尸，已排空的日志随 Output 返回不丢。
    // name 带阶段前缀，超时日志可定位到 configure 还是 build。
    const std::string phaseName = std::string("xmake ") + phase;
    if (!waitChildWithTimeout(child, phaseName.c_str(), timeout, output)) {
        writeProcessOutput(output);
        return false;
    }
    writeProcessOutput(output);
    if (!output.status.success()) {
        if (failedCode != nullptr) *failedCode = output.status.code;
        CA_LOGT_ERROR("compile", "xmake {} failed with exit code {}.\n", phase,
                output.status.code);
        return false;
    }
    return true;
}

/// 构造 configure/build 两阶段的 xmake 命令（不含 env 注入）。重试需要
/// 全新 Command 对象——旧对象已携带失败的 env 形状与参数序列。
ca::process::Command makeXmakeCommand(const XmakeBuildConfig& config, bool configurePhase)
{
    ca::process::Command command("xmake");
    command.current_dir(config.projectDir.string());
    if (configurePhase) {
        command.arg("f").arg("-P").arg(".").arg("-m").arg(config.mode);
#if defined(_WIN32)
        // Git Bash（MSYS）下 xmake 会把平台自动探测为 mingw 导致 "cannot get
        // program for cc"——显式锁定 windows + msvc 工具链。arch 也必须显式锁
        // x64：-p windows 的缺省 arch 取 os.arch()="x86_64"（unix 命名），msvc
        // 工具链按 "x64" 探测 VS 环境，于是 "checking for Microsoft Visual
        // Studio (x86_64) version ... no → toolchain not found"（回归 c9edba8fe）。
        // x64 与 loadMsvcEnvironment 固定的 vcvarsall.bat x64 同一口径。
        command.arg("-p").arg("windows").arg("--toolchain=msvc").arg("-a").arg("x64");
#endif
        command.arg("-y");
    }
    else {
        command.arg("build").arg("-P").arg(".").arg("-y").arg("mj2x_native");
    }
    return command;
}

#if defined(_WIN32)
/// NTSTATUS 失败重试用的最小 env 注入：仅 captured 中的关键覆盖集
/// （PATH/INCLUDE/LIB/LIBPATH/TEMP/TMP，case-insensitive，空值跳过），
/// 其余整块继承父进程（Command::env 覆盖集语义）。env 块内容形状与
/// mergeProcessEnvironment 全量注入完全不同，作为子 xmake 对特定 env
/// 形状缺陷（A1 报告 §5.2）的确定性逃生口。
void injectKeyEnvironmentOverrides(ca::process::Command&     command,
                                   const ProcessEnvironment& captured)
{
    static const char* const keys[] = {"PATH", "INCLUDE", "LIB", "LIBPATH", "TEMP", "TMP"};
    for (const char* key : keys) {
        for (const auto& [name, value] : captured) {
            if (!environmentKeyEquals(name, key) || value.empty()) continue;
            command.env(name, value);
            break;
        }
    }
}
#endif

const char* nativeLibraryExtension()
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

bool isExpectedLibrary(const fs::path& path, const std::string& mode)
{
    if (path.extension() != nativeLibraryExtension()) return false;
    const std::string name = path.filename().string();
#if defined(_WIN32)
    if (name != "mj2x_native.dll") return false;
#else
    if (name != std::string("libmj2x_native") + nativeLibraryExtension()) return false;
#endif
    for (const auto& component : path) {
        if (component == mode) return true;
    }
    return false;
}

bool copyLibraryAtomically(const fs::path& source, const fs::path& destination)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temporary = destination;
    temporary += ".mj2x-copy-" + std::to_string(token);
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    if (!fs::is_regular_file(temporary, ec) || fs::file_size(temporary, ec) == 0) {
        fs::remove(temporary, ec);
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, ec);
        return false;
    }
#else
    fs::rename(temporary, destination, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
#endif
    return true;
}

}

int buildXmakeProject(const XmakeBuildConfig& config)
{
    std::error_code ec;
    if (config.projectDir.empty() || !fs::is_regular_file(config.projectDir / "xmake.lua", ec)) {
        CA_LOGT_ERROR("compile", "xmake project is missing xmake.lua.\n");
        return 1;
    }
    if (config.outputLibraryPath.empty()) {
        CA_LOGT_ERROR("compile", "xmake output library path is empty.\n");
        return 1;
    }
    fs::remove(config.outputLibraryPath, ec);
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot remove previous native library: {}\n", config.outputLibraryPath.string().c_str());
        return 1;
    }

#if defined(_WIN32)
    ProcessEnvironment    environment;
    std::filesystem::path compilerPath;
    if (!loadMsvcEnvironment(environment, compilerPath, config.processTimeout)) {
        CA_LOGT_ERROR("compile", "cannot configure MSVC environment for xmake build.\n");
        return 1;
    }
#endif
    // 两阶段共用运行路径：全量合并 env 首跑；退码为 NTSTATUS 负值（子进程
    // 异常崩溃，非常规错误退出）时以最小 env 覆盖集自动重试一次（A1 报告
    // P0-B：重试 env 块形状完全改变，确定性绕开内容巧合触发；重试仍失败
    // 照常报错）。
    const auto runPhase = [&](const char* phase, bool configurePhase) {
        auto command = makeXmakeCommand(config, configurePhase);
#if defined(_WIN32)
        mergeProcessEnvironment(command, environment);
#endif
        int failedCode = 0;
        if (runXmake(command, phase, &failedCode, config.processTimeout)) return true;
#if defined(_WIN32)
        if (isAbnormalChildExit(failedCode)) {
            CA_LOGT_WARN("compile",
                         "xmake {} exited abnormally (exit code {}, NTSTATUS); "
                         "retrying once with minimal environment overrides.\n",
                         phase, failedCode);
            auto retry = makeXmakeCommand(config, configurePhase);
            injectKeyEnvironmentOverrides(retry, environment);
            return runXmake(retry, phase, nullptr, config.processTimeout);
        }
#endif
        return false;
    };
    if (!runPhase("configure", true)) return 1;

    if (!runPhase("build", false)) return 1;

    std::vector<fs::path> libraries;
    const fs::path        buildRoot = config.projectDir / "build";
    if (fs::exists(buildRoot, ec)) {
        for (fs::recursive_directory_iterator it(buildRoot, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file(ec) && isExpectedLibrary(it->path(), config.mode))
                libraries.push_back(it->path());
        }
    }
    if (ec || libraries.size() != 1) {
        CA_LOGT_ERROR("compile", "xmake build produced {} matching native libraries; expected 1.\n", libraries.size());
        return 1;
    }
    if (!copyLibraryAtomically(libraries.front(), config.outputLibraryPath)) {
        CA_LOGT_ERROR("compile", "cannot copy xmake native library to: {}\n", config.outputLibraryPath.string().c_str());
        return 1;
    }
    CA_LOGT_INFO("compile", "Native library written: {}\n", config.outputLibraryPath.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

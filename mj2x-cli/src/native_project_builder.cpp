#include "native_project_generator.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

namespace {
std::FILE* const kStdoutStream = stdout;
std::FILE* const kStderrStream = stderr;
}

#include "libca/process/subprocess.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x::cli {

// native_compiler.cpp 提供的外部链接实现（无头文件声明）：加载 MSVC 编译环境
// （INCLUDE/LIB/PATH，必要时调 vcvarsall.bat x64）。xmake 子进程复用同一环境，
// 否则在 PATH 含 mingw64 的 shell（Git Bash）下 xmake 会探测 mingw 工具链导致
// "cannot get program for cc"。
using ProcessEnvironment = std::vector<std::pair<std::string, std::string>>;
bool loadMsvcEnvironment(ProcessEnvironment& environment, std::filesystem::path& compilerPath);

// native_compiler.cpp 提供（与 loadMsvcEnvironment 同源导出）：把捕获的 MSVC
// 环境以「父进程环境为底、捕获变量覆盖其上（同名 case-insensitive 以捕获值
// 胜出）」注入子进程命令，在调用点显式保证子 xmake 拿到完整环境块，不依赖
// libca 对 Command::env 覆盖集的内部实现细节。
void mergeProcessEnvironment(ca::process::Command& command, const ProcessEnvironment& captured);

namespace fs = std::filesystem;
namespace {

void writeProcessOutput(const ca::process::Output& output)
{
    if (!output.stdout_data.empty())
        fwrite(output.stdout_data.data(), 1, output.stdout_data.size(), kStdoutStream);
    if (!output.stderr_data.empty())
        fwrite(output.stderr_data.data(), 1, output.stderr_data.size(), kStderrStream);
}

bool runXmake(ca::process::Command& command, const char* phase)
{
    auto completed = command.output();
    if (completed.is_err()) {
        CA_LOGT_ERROR("compile", "cannot start xmake for {}: {}\n", phase,
                completed.unwrap_err().to_string().c_str());
        return false;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        CA_LOGT_ERROR("compile", "xmake {} failed with exit code {}.\n", phase,
                output.status.code);
        return false;
    }
    return true;
}

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

    ca::process::Command configure("xmake");
    configure.current_dir(config.projectDir.string())
        .arg("f")
        .arg("-P")
        .arg(".")
        .arg("-m")
        .arg(config.mode);
#if defined(_WIN32)
    // Git Bash（MSYS）下 xmake 会把平台自动探测为 mingw 导致 "cannot get
    // program for cc"——显式锁定 windows + msvc 工具链。arch 也必须显式锁
    // x64：-p windows 的缺省 arch 取 os.arch()="x86_64"（unix 命名），msvc
    // 工具链按 "x64" 探测 VS 环境，于是 "checking for Microsoft Visual
    // Studio (x86_64) version ... no → toolchain not found"（回归 c9edba8fe）。
    // x64 与 loadMsvcEnvironment 固定的 vcvarsall.bat x64 同一口径。
    configure.arg("-p").arg("windows").arg("--toolchain=msvc").arg("-a").arg("x64");
#endif
    configure.arg("-y");
    ca::process::Command build("xmake");
    build.current_dir(config.projectDir.string())
        .arg("build")
        .arg("-P")
        .arg(".")
        .arg("-y")
        .arg("mj2x_native");
#if defined(_WIN32)
    ProcessEnvironment    environment;
    std::filesystem::path compilerPath;
    if (!loadMsvcEnvironment(environment, compilerPath)) {
        CA_LOGT_ERROR("compile", "cannot configure MSVC environment for xmake build.\n");
        return 1;
    }
    mergeProcessEnvironment(configure, environment);
    mergeProcessEnvironment(build, environment);
#endif
    if (!runXmake(configure, "configure")) return 1;

    if (!runXmake(build, "build")) return 1;

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

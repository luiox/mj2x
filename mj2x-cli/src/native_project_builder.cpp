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

namespace mj2x::cli {
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot start xmake for %s: %s\n",
                phase,
                completed.unwrap_err().to_string().c_str());
        return false;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: xmake %s failed with exit code %d.\n",
                phase,
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
        fprintf(kStderrStream, "[mj2x] ERROR: xmake project is missing xmake.lua.\n");
        return 1;
    }
    if (config.outputLibraryPath.empty()) {
        fprintf(kStderrStream, "[mj2x] ERROR: xmake output library path is empty.\n");
        return 1;
    }
    fs::remove(config.outputLibraryPath, ec);
    if (ec) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot remove previous native library: %s\n",
                config.outputLibraryPath.string().c_str());
        return 1;
    }

    ca::process::Command configure("xmake");
    configure.current_dir(config.projectDir.string())
        .arg("f")
        .arg("-P")
        .arg(".")
        .arg("-m")
        .arg(config.mode)
        .arg("-y");
    if (!runXmake(configure, "configure")) return 1;

    ca::process::Command build("xmake");
    build.current_dir(config.projectDir.string())
        .arg("build")
        .arg("-P")
        .arg(".")
        .arg("-y")
        .arg("mj2x_native");
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: xmake build produced %zu matching native libraries; expected 1.\n",
                libraries.size());
        return 1;
    }
    if (!copyLibraryAtomically(libraries.front(), config.outputLibraryPath)) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot copy xmake native library to: %s\n",
                config.outputLibraryPath.string().c_str());
        return 1;
    }
    printf("[mj2x] Native library written: %s\n", config.outputLibraryPath.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

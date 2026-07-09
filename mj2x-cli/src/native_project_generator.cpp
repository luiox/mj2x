#include "native_project_generator.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mj2x::cli {
namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool writeText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fprintf(stderr, "[mj2x] ERROR: cannot write %s\n", path.string().c_str());
        return false;
    }
    out << text;
    return true;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool copyDirectory(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    if (!fs::exists(from, ec) || !fs::is_directory(from, ec)) {
        fprintf(
            stderr, "[mj2x] ERROR: CRT source directory not found: %s\n", from.string().c_str());
        return false;
    }

    fs::create_directories(to, ec);
    if (ec) {
        fprintf(stderr, "[mj2x] ERROR: cannot create CRT destination: %s\n", ec.message().c_str());
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
        if (ec) {
            fprintf(
                stderr, "[mj2x] ERROR: cannot enumerate CRT directory: %s\n", ec.message().c_str());
            return false;
        }
        const auto rel = fs::relative(entry.path(), from, ec);
        if (ec) return false;
        const auto dest = to / rel;
        if (entry.is_directory()) {
            fs::create_directories(dest, ec);
        }
        else if (entry.is_regular_file()) {
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            fprintf(stderr, "[mj2x] ERROR: cannot copy CRT file: %s\n", ec.message().c_str());
            return false;
        }
    }
    return true;
}

fs::path envPath(const char* name)
{
#ifdef _WIN32
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
    fs::path result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? fs::path{} : fs::path(value);
#endif
}

bool isCrtRoot(const fs::path& root)
{
    std::error_code ec;
    return fs::exists(root / "src" / "regc" / "mj2x_regc_crt.h", ec) &&
           fs::exists(root / "templates" / "xmake.lua.in", ec);
}

void addCandidate(std::vector<fs::path>& candidates, const fs::path& path)
{
    if (!path.empty()) candidates.push_back(path);
}

}   // namespace

std::filesystem::path findCrtRoot(const char* argv0)
{
    std::vector<fs::path> candidates;
    addCandidate(candidates, envPath("MJ2X_CRT_DIR"));

    std::error_code ec;
    auto            cwd = fs::current_path(ec);
    if (!ec) {
        addCandidate(candidates, cwd / "mj2x" / "mj2x-crt");
        addCandidate(candidates, cwd / "mj2x-crt");
    }

    if (argv0 != nullptr && argv0[0] != '\0') {
        fs::path exe = fs::absolute(argv0, ec);
        if (!ec) {
            auto dir = exe.parent_path();
            addCandidate(candidates, dir / "mj2x-crt");
            addCandidate(candidates, dir / ".." / "mj2x-crt");
            addCandidate(candidates, dir / ".." / "share" / "mj2x-crt");
            addCandidate(candidates, dir / ".." / ".." / "mj2x" / "mj2x-crt");
        }
    }

    for (const auto& candidate : candidates) {
        auto normalized = fs::weakly_canonical(candidate, ec);
        if (ec) {
            ec.clear();
            normalized = candidate;
        }
        if (isCrtRoot(normalized)) return normalized;
    }
    return {};
}

int generateXmakeProject(const XmakeProjectConfig& config)
{
    std::error_code ec;
    if (config.projectDir.empty()) {
        fprintf(stderr, "[mj2x] ERROR: xmake project directory is empty.\n");
        return 1;
    }
    if (!fs::exists(config.generatedSourcePath, ec)) {
        fprintf(stderr,
                "[mj2x] ERROR: emitted C source does not exist: %s\n",
                config.generatedSourcePath.string().c_str());
        return 1;
    }
    if (!isCrtRoot(config.crtRoot)) {
        fprintf(stderr,
                "[mj2x] ERROR: mj2x-crt root not found. Set MJ2X_CRT_DIR or run from repo root.\n");
        return 1;
    }

    const fs::path generatedDir = config.projectDir / "src" / "generated";
    const fs::path crtDest      = config.projectDir / "src" / "mj2x-crt";
    fs::create_directories(generatedDir, ec);
    if (ec) {
        fprintf(stderr, "[mj2x] ERROR: cannot create xmake project: %s\n", ec.message().c_str());
        return 1;
    }

    const fs::path nativeDest = generatedDir / "native.c";
    auto           sourceAbs  = fs::absolute(config.generatedSourcePath, ec);
    auto           nativeAbs  = fs::absolute(nativeDest, ec);
    if (sourceAbs != nativeAbs) {
        fs::copy_file(
            config.generatedSourcePath, nativeDest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(
                stderr, "[mj2x] ERROR: cannot copy emitted C source: %s\n", ec.message().c_str());
            return 1;
        }
    }

    if (!copyDirectory(config.crtRoot / "src", crtDest)) return 1;

    auto xmakeTemplate  = readText(config.crtRoot / "templates" / "xmake.lua.in");
    auto readmeTemplate = readText(config.crtRoot / "templates" / "README.md.in");
    if (xmakeTemplate.empty() || readmeTemplate.empty()) {
        fprintf(stderr, "[mj2x] ERROR: mj2x-crt templates are missing.\n");
        return 1;
    }
    replaceAll(xmakeTemplate, "@PROJECT_NAME@", config.projectName);
    replaceAll(readmeTemplate, "@PROJECT_NAME@", config.projectName);

    if (!writeText(config.projectDir / "xmake.lua", xmakeTemplate)) return 1;
    if (!writeText(config.projectDir / "README.md", readmeTemplate)) return 1;
    if (!writeText(config.projectDir / ".gitignore",
                   "build/\n.xmake/\n*.obj\n*.o\n*.dll\n*.so\n*.dylib\n")) {
        return 1;
    }

    printf("[mj2x] xmake project written: %s\n", config.projectDir.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

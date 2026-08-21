#include "native_project_generator.hpp"

#include "embedded_rt.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "libca/log/log_macros.hpp"

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
        CA_LOGT_ERROR("compile", "cannot write {}\n", path.string().c_str());
        return false;
    }
    out << text;
    return true;
}

bool writeProjectTemplate(const fs::path& path, std::string_view source,
                          std::string_view projectName)
{
    constexpr std::string_view projectNameToken = "@PROJECT_NAME@";
    if (source.find(projectNameToken) == std::string_view::npos) {
        CA_LOGT_ERROR("compile", "project template is missing {}: {}\n",
                std::string(projectNameToken),
                path.string().c_str());
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        CA_LOGT_ERROR("compile", "cannot write {}\n", path.string().c_str());
        return false;
    }

    std::size_t cursor = 0;
    while (cursor < source.size()) {
        const std::size_t token = source.find(projectNameToken, cursor);
        if (token == std::string_view::npos) {
            out.write(source.data() + cursor, static_cast<std::streamsize>(source.size() - cursor));
            break;
        }
        out.write(source.data() + cursor, static_cast<std::streamsize>(token - cursor));
        out.write(projectName.data(), static_cast<std::streamsize>(projectName.size()));
        cursor = token + projectNameToken.size();
    }
    return static_cast<bool>(out);
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool copyRtTree(const fs::path& root, const fs::path& to)
{
    // 外部磁盘源包(mj2x-rt 布局):复制 src/<挂载>/ 树到目标目录。
    std::error_code ec;
    if (!fs::exists(root / "src" / "regc", ec) || !fs::exists(root / "src" / "common", ec)) {
        CA_LOGT_ERROR("compile", "RT source pack incomplete: {}\n", root.string().c_str());
        return false;
    }

    fs::create_directories(to, ec);
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot create RT destination: {}\n", ec.message().c_str());
        return false;
    }

    for (const std::string_view mount : mj2x::cli::kRtSourceMounts) {
        const fs::path mountSource = root / "src" / mount;
        const fs::path mountDest   = to / mount;
        fs::create_directories(mountDest, ec);
        if (ec) {
            CA_LOGT_ERROR("compile", "cannot create RT mount dir: {}\n", mountDest.string().c_str());
            return false;
        }
        for (const auto& entry : fs::recursive_directory_iterator(mountSource, ec)) {
            if (ec) break;
            const fs::path dest = mountDest / entry.path().lexically_relative(mountSource);
            if (entry.is_directory()) {
                fs::create_directories(dest, ec);
            }
            else if (entry.is_regular_file()) {
                fs::create_directories(dest.parent_path(), ec);
                if (!ec) {
                    fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
                }
            }
            if (ec) {
                CA_LOGT_ERROR("compile", "cannot copy RT file: {}\n", entry.path().string().c_str());
                return false;
            }
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
           fs::exists(root / "rt.resources", ec) &&
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
        addCandidate(candidates, cwd / "mj2x" / "mj2x-rt");
        addCandidate(candidates, cwd / "mj2x-rt");
    }

    if (argv0 != nullptr && argv0[0] != '\0') {
        fs::path exe = fs::absolute(argv0, ec);
        if (!ec) {
            auto dir = exe.parent_path();
            addCandidate(candidates, dir / "mj2x-rt");
            addCandidate(candidates, dir / ".." / "mj2x-rt");
            addCandidate(candidates, dir / ".." / "share" / "mj2x-rt");
            addCandidate(candidates, dir / ".." / ".." / "mj2x" / "mj2x-rt");
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
        CA_LOGT_ERROR("compile", "xmake project directory is empty.\n");
        return 1;
    }
    if (!fs::exists(config.generatedSourcePath, ec)) {
        CA_LOGT_ERROR("compile", "emitted C source does not exist: {}\n", config.generatedSourcePath.string().c_str());
        return 1;
    }
    const bool useExternalCrt = isCrtRoot(config.crtRoot);

    const fs::path generatedDir = config.projectDir / "src" / "generated";
    const fs::path crtDest      = config.projectDir / "src" / "mj2x-rt";
    fs::create_directories(generatedDir, ec);
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot create xmake project: {}\n", ec.message().c_str());
        return 1;
    }

    const fs::path nativeDest = generatedDir / config.generatedSourcePath.filename();
    auto           sourceAbs  = fs::absolute(config.generatedSourcePath, ec);
    auto           nativeAbs  = fs::absolute(nativeDest, ec);
    if (sourceAbs != nativeAbs) {
        fs::copy_file(
            config.generatedSourcePath, nativeDest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CA_LOGT_ERROR("compile", "cannot copy emitted C source: {}\n", ec.message().c_str());
            return 1;
        }
    }

    if (useExternalCrt) {
        if (!copyRtTree(config.crtRoot, crtDest)) return 1;
    }
    else if (!mj2x::cli::writeRtTree(config.projectDir)) {
        CA_LOGT_ERROR("compile", "embedded mj2x-rt resources are unavailable.\n");
        return 1;
    }

    auto xmakeTemplate  = useExternalCrt ? readText(config.crtRoot / "templates" / "xmake.lua.in")
                                         : mj2x::cli::readEmbeddedText("templates", "/xmake.lua.in");
    auto readmeTemplate = useExternalCrt ? readText(config.crtRoot / "templates" / "README.md.in")
                                         : mj2x::cli::readEmbeddedText("templates", "/README.md.in");
    if (xmakeTemplate.empty() || readmeTemplate.empty()) {
        CA_LOGT_ERROR("compile", "mj2x-rt templates are missing.\n");
        return 1;
    }
    if (!writeProjectTemplate(config.projectDir / "xmake.lua", xmakeTemplate, config.projectName)) {
        return 1;
    }
    if (!writeProjectTemplate(
            config.projectDir / "README.md", readmeTemplate, config.projectName)) {
        return 1;
    }
    if (!writeText(config.projectDir / ".gitignore",
                   "build/\n.xmake/\n*.obj\n*.o\n*.dll\n*.so\n*.dylib\n")) {
        return 1;
    }

    CA_LOGT_INFO("compile", "xmake project written: {}\n", config.projectDir.string().c_str());
    return 0;
}

int generateLlvmXmakeProject(const std::string& projectDirValue,
                             const std::string& llvmOutputPath, bool withRuntimeProtection)
{
    std::error_code ec;
    const fs::path projectDir(projectDirValue);
    if (projectDir.empty()) {
        CA_LOGT_ERROR("compile", "xmake project directory is empty.\n");
        return 1;
    }
    if (!fs::exists(llvmOutputPath, ec)) {
        CA_LOGT_ERROR("compile", "emitted LLVM IR source does not exist: {}\n",
                      llvmOutputPath.c_str());
        return 1;
    }

    fs::create_directories(projectDir, ec);
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot create xmake project: {}\n", ec.message().c_str());
        return 1;
    }

    const fs::path nativeDest = projectDir / "native.ll";
    auto           sourceAbs  = fs::absolute(llvmOutputPath, ec);
    auto           nativeAbs  = fs::absolute(nativeDest, ec);
    if (sourceAbs != nativeAbs) {
        fs::copy_file(llvmOutputPath, nativeDest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CA_LOGT_ERROR("compile", "cannot copy emitted LLVM IR source: {}\n",
                          ec.message().c_str());
            return 1;
        }
    }

    // JDK intrinsic 直调（mj2x_math_* 等）与 --runtime-protection 检查点都来自
    // mj2x-rt：整树随工程分发（模板按需 glob 编译）。
    if (!mj2x::cli::writeRtTree(projectDir)) {
        CA_LOGT_ERROR("compile", "embedded mj2x-rt resources are unavailable.\n");
        return 1;
    }

    if (!writeText(projectDir / "xmake.lua",
                   "target(\"mj2x_native\")\n"
                   "    set_kind(\"shared\")\n"
                   "    on_load(function (target)\n"
                   "        target:set(\"kind\", \"phony\")\n"
                   "    end)\n"
                   "    after_build(function (target)\n"
                   "        import(\"core.base.option\")\n"
                   "        import(\"core.tool.compiler\")\n"
                   "        local suffix = is_host(\"windows\") and \".dll\" or "
                   "(is_host(\"macosx\") and \".dylib\" or \".so\")\n"
                   "        local out = path.join(target:targetdir(), \"mj2x_native\" .. suffix)\n"
                   "        os.mkdir(target:targetdir())\n"
                   "        local args = {\"-shared\", path.join(os.scriptdir(), \"native.ll\")}\n"
                   "        local crtSrc = path.join(os.scriptdir(), \"src/mj2x-rt\")\n"
                   "        if os.isdir(crtSrc) then\n"
                   "            table.insert(args, \"-I\" .. crtSrc)\n"
                   "            local jh = os.getenv(\"JAVA_HOME\")\n"
                   "            if jh and os.isdir(path.join(jh, \"include\")) then\n"
                   "                table.insert(args, \"-I\" .. path.join(jh, \"include\"))\n"
                   "                if os.isdir(path.join(jh, \"include\", \"win32\")) then\n"
                   "                    table.insert(args, \"-I\" .. path.join(jh, \"include\", \"win32\"))\n"
                   "                elseif os.isdir(path.join(jh, \"include\", \"linux\")) then\n"
                   "                    table.insert(args, \"-I\" .. path.join(jh, \"include\", \"linux\"))\n"
                   "                elseif os.isdir(path.join(jh, \"include\", \"darwin\")) then\n"
                   "                    table.insert(args, \"-I\" .. path.join(jh, \"include\", \"darwin\"))\n"
                   "                end\n"
                   "            end\n"
                   "            for _, c in ipairs(os.glob(path.join(crtSrc, \"regc/*.c\"))) do\n"
                   "                table.insert(args, c)\n"
                   "            end\n"
                   "            for _, c in ipairs(os.glob(path.join(os.scriptdir(), "
                   "\"src/mj2x-rt/common/*.c\"))) do\n"
                   "                table.insert(args, c)\n"
                   "            end\n"
                   "        end\n"
                   "        table.insert(args, \"-o\")\n"
                   "        table.insert(args, out)\n"
                   "        os.execv(\"clang\", args)\n"
                   "    end)\n"
                   "target_end()\n")) {
        return 1;
    }
    if (!writeText(projectDir / "README.md",
                   "# mj2x native xmake project (LLVM IR)\n\n"
                   "Build:\n\n"
                   "```sh\n"
                   "xmake\n"
                   "```\n\n"
                   "Requires clang on PATH.\n")) {
        return 1;
    }
    if (!writeText(projectDir / ".gitignore",
                   "build/\n.xmake/\n*.obj\n*.o\n*.dll\n*.so\n*.dylib\n")) {
        return 1;
    }

    CA_LOGT_INFO("compile", "xmake project written: {}\n", projectDir.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

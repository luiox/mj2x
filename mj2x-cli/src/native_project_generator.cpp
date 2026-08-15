#include "native_project_generator.hpp"

#include "embedded_resources.hpp"

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

std::string readEmbeddedText(const char* path)
{
    const auto* resource = findEmbeddedResource(path);
    if (resource == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(resource->data), resource->size);
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
        CA_LOGT_ERROR("compile", "project template is missing %.*s: {}\n", static_cast<int>(projectNameToken.size()),
                projectNameToken.data(),
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

bool copyManifestSources(const fs::path& root, const fs::path& to)
{
    std::error_code ec;
    const auto      manifest = root / "resources.manifest";
    if (!fs::exists(manifest, ec)) {
        CA_LOGT_ERROR("compile", "CRT resource manifest not found: {}\n", manifest.string().c_str());
        return false;
    }

    fs::create_directories(to, ec);
    if (ec) {
        CA_LOGT_ERROR("compile", "cannot create CRT destination: {}\n", ec.message().c_str());
        return false;
    }

    std::ifstream in(manifest);
    std::string   line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("src/", 0) != 0) continue;
        if (line.find("../") != std::string::npos) {
            CA_LOGT_ERROR("compile", "invalid CRT manifest path: {}\n", line.c_str());
            return false;
        }

        const fs::path source = root / line;
        const fs::path dest   = to / fs::path(line).lexically_relative("src");
        if (!fs::exists(source, ec) || !fs::is_regular_file(source, ec)) {
            CA_LOGT_ERROR("compile", "CRT manifest source not found: {}\n", source.string().c_str());
            return false;
        }
        fs::create_directories(dest.parent_path(), ec);
        if (!ec) fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CA_LOGT_ERROR("compile", "cannot copy CRT file: {}\n", ec.message().c_str());
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
           fs::exists(root / "resources.manifest", ec) &&
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
        CA_LOGT_ERROR("compile", "xmake project directory is empty.\n");
        return 1;
    }
    if (!fs::exists(config.generatedSourcePath, ec)) {
        CA_LOGT_ERROR("compile", "emitted C source does not exist: {}\n", config.generatedSourcePath.string().c_str());
        return 1;
    }
    const bool useExternalCrt = isCrtRoot(config.crtRoot);

    const fs::path generatedDir = config.projectDir / "src" / "generated";
    const fs::path crtDest      = config.projectDir / "src" / "mj2x-crt";
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
        if (!copyManifestSources(config.crtRoot, crtDest)) return 1;
    }
    else if (!writeEmbeddedCrtTree(config.projectDir)) {
        CA_LOGT_ERROR("compile", "embedded mj2x-crt resources are unavailable.\n");
        return 1;
    }

    auto xmakeTemplate  = useExternalCrt ? readText(config.crtRoot / "templates" / "xmake.lua.in")
                                         : readEmbeddedText("templates/xmake.lua.in");
    auto readmeTemplate = useExternalCrt ? readText(config.crtRoot / "templates" / "README.md.in")
                                         : readEmbeddedText("templates/README.md.in");
    if (xmakeTemplate.empty() || readmeTemplate.empty()) {
        CA_LOGT_ERROR("compile", "mj2x-crt templates are missing.\n");
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
    // mj2x-crt：整树随工程分发（模板按需 glob 编译）。
    if (!writeEmbeddedCrtTree(projectDir)) {
        CA_LOGT_ERROR("compile", "embedded mj2x-crt resources are unavailable.\n");
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
                   "        local crtSrc = path.join(os.scriptdir(), \"src/mj2x-crt/src\")\n"
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
                   "\"src/mj2x-crt/rt_protect/*.c\"))) do\n"
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

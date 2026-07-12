#include "native_compiler.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#ifndef _WIN32
#    include <sys/wait.h>
#endif

namespace mj2x::cli {
namespace {

std::string quotePath(const std::filesystem::path& path)
{
    std::string value = path.string();
#ifdef _WIN32
    std::string out = "\"";
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '"') {
            out += "\\\"";
        }
        else if (c == '\\' && i + 1 == value.size()) {
            out += "\\\\";
        }
        else {
            out += c;
        }
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        }
        else {
            out += c;
        }
    }
    out += "'";
    return out;
#endif
}

int runQuiet(const std::string& command)
{
    return std::system(command.c_str());
}

bool commandExists(const std::string& command)
{
#ifdef _WIN32
    return runQuiet("where " + command + " >nul 2>nul") == 0;
#else
    return runQuiet("command -v " + command + " >/dev/null 2>&1") == 0;
#endif
}

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

const char* compilerExecutable(CCompilerKind compiler)
{
    switch (compiler) {
    case CCompilerKind::Msvc: return "cl";
    case CCompilerKind::Gcc: return "gcc";
    case CCompilerKind::Clang: return "clang";
    case CCompilerKind::Auto: break;
    }
    return "";
}

std::filesystem::path findVcvarsAll();

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
        fprintf(
            stderr, "[mj2x] ERROR: invalid mj2x-crt source pack: %s\n", crtRoot.string().c_str());
        return false;
    }

    std::ifstream in(manifest);
    if (!in) {
        fprintf(
            stderr, "[mj2x] ERROR: cannot read mj2x-crt manifest: %s\n", manifest.string().c_str());
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
            fprintf(
                stderr, "[mj2x] ERROR: invalid mj2x-crt manifest source path: %s\n", line.c_str());
            return false;
        }

        const auto source    = crtRoot / relative;
        const auto extension = source.extension().string();
        if (extension == ".cpp") {
            fprintf(stderr,
                    "[mj2x] ERROR: --compile does not yet support C++ "
                    "mj2x-crt sources; use "
                    "--generate-xmake.\n");
            return false;
        }
        if (extension != ".c") continue;
        if (!std::filesystem::exists(source, ec)) {
            fprintf(stderr,
                    "[mj2x] ERROR: mj2x-crt manifest source is missing: %s\n",
                    source.string().c_str());
            return false;
        }
        sources.push_back(source);
        foundSource = true;
    }

    if (!foundSource) {
        fprintf(stderr, "[mj2x] ERROR: mj2x-crt manifest has no C sources.\n");
        return false;
    }
    return true;
}

bool canUseMsvc()
{
    return commandExists("cl") || !findVcvarsAll().empty();
}

bool detectCompiler(CCompilerKind requested, CCompilerKind& detected)
{
    if (requested != CCompilerKind::Auto) {
        detected = requested;
        if (requested == CCompilerKind::Msvc) return canUseMsvc();
        return commandExists(compilerExecutable(detected));
    }

#ifdef _WIN32
    bool hasCl             = commandExists("cl");
    bool hasConfiguredMsvc = hasCl && !envValue("INCLUDE").empty();
    bool hasVcvarsMsvc     = !hasConfiguredMsvc && !findVcvarsAll().empty();
    if (hasConfiguredMsvc || hasVcvarsMsvc) {
        detected = CCompilerKind::Msvc;
        return true;
    }
    if (commandExists("clang")) {
        detected = CCompilerKind::Clang;
        return true;
    }
    if (commandExists("gcc")) {
        detected = CCompilerKind::Gcc;
        return true;
    }
    if (hasCl) {
        detected = CCompilerKind::Msvc;
        return true;
    }
#elif defined(__APPLE__)
    if (commandExists("clang")) {
        detected = CCompilerKind::Clang;
        return true;
    }
    if (commandExists("gcc")) {
        detected = CCompilerKind::Gcc;
        return true;
    }
#else
    if (commandExists("gcc")) {
        detected = CCompilerKind::Gcc;
        return true;
    }
    if (commandExists("clang")) {
        detected = CCompilerKind::Clang;
        return true;
    }
#endif

    return false;
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
    std::string javaHomeValue = envValue("JAVA_HOME");
    if (javaHomeValue.empty()) {
        fprintf(stderr,
                "[mj2x] ERROR: JAVA_HOME is not set; required to locate "
                "JNI headers.\n");
        return false;
    }

    std::filesystem::path javaHome(javaHomeValue);
    std::filesystem::path include = javaHome / "include";
#ifdef _WIN32
    std::filesystem::path platformInclude = include / "win32";
#elif defined(__APPLE__)
    std::filesystem::path platformInclude = include / "darwin";
#else
    std::filesystem::path platformInclude = include / "linux";
#endif

    std::error_code ec;
    if (!std::filesystem::exists(include, ec) || !std::filesystem::exists(platformInclude, ec)) {
        fprintf(stderr,
                "[mj2x] ERROR: JNI include paths not found under JAVA_HOME: %s and "
                "%s\n",
                include.string().c_str(),
                platformInclude.string().c_str());
        return false;
    }

    includes.push_back(include);
    includes.push_back(platformInclude);
    return true;
}

std::filesystem::path vcvarsAllPathFromVCTools(const std::string& value)
{
    if (value.empty()) return {};
    std::filesystem::path tools(value);
    std::filesystem::path vcRoot = tools.parent_path().parent_path().parent_path();
    std::filesystem::path vcvars = vcRoot / "Auxiliary" / "Build" / "vcvarsall.bat";
    std::error_code       ec;
    return std::filesystem::exists(vcvars, ec) ? vcvars : std::filesystem::path{};
}

std::filesystem::path findVcvarsAll()
{
    if (auto fromEnv = vcvarsAllPathFromVCTools(envValue("VCToolsInstallDir")); !fromEnv.empty())
        return fromEnv;

    std::vector<std::filesystem::path> roots;
    if (auto programFiles = envValue("ProgramFiles"); !programFiles.empty())
        roots.emplace_back(programFiles);
    if (auto programFilesX86 = envValue("ProgramFiles(x86)"); !programFilesX86.empty())
        roots.emplace_back(programFilesX86);

    const char* editions[] = {"Community", "Professional", "Enterprise", "BuildTools"};
    for (const auto& root : roots) {
        for (const char* edition : editions) {
            std::filesystem::path vcvars = root / "Microsoft Visual Studio" / "2022" / edition /
                                           "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
            std::error_code ec;
            if (std::filesystem::exists(vcvars, ec)) return vcvars;
        }
    }
    return {};
}

int systemExitCode(int rc)
{
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return rc;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return rc;
#endif
}

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
    return false;
}

std::filesystem::path nativeLibraryPathForOutput(const std::string& outputPath)
{
    return nativeLibraryPath(outputPath);
}

int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot)
{
    std::error_code ec;
    if (!std::filesystem::exists(cOutputPath, ec)) {
        fprintf(stderr, "[mj2x] ERROR: emitted C source does not exist: %s\n", cOutputPath.c_str());
        return 1;
    }

    CCompilerKind compiler = CCompilerKind::Auto;
    if (!detectCompiler(requestedCompiler, compiler)) {
        if (requestedCompiler == CCompilerKind::Auto) {
            fprintf(stderr, "[mj2x] ERROR: no supported C compiler found on PATH.\n");
        }
        else {
            fprintf(stderr,
                    "[mj2x] ERROR: C compiler '%s' not found on PATH.\n",
                    compilerExecutable(requestedCompiler));
        }
        return 1;
    }

    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes)) return 1;

    std::vector<std::filesystem::path> sources{std::filesystem::path(cOutputPath)};
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, sources)) return 1;

    std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::ostringstream    command;
    if (compiler == CCompilerKind::Msvc) {
        command << "cl /nologo /LD";
        for (const auto& include : includes) command << " /I" << quotePath(include);
        std::filesystem::path objectDir = libraryPath.parent_path();
        if (objectDir.empty()) objectDir = ".";
        objectDir /= "";
        command << " /Fo" << quotePath(objectDir);
        for (const auto& source : sources) command << " " << quotePath(source);
        command << " /Fe" << quotePath(libraryPath);
        if (envValue("INCLUDE").empty()) {
            auto vcvars = findVcvarsAll();
            if (vcvars.empty()) {
                fprintf(stderr,
                        "[mj2x] ERROR: MSVC compiler was found, but the MSVC "
                        "environment is not "
                        "configured and vcvarsall.bat was not found.\n");
                return 1;
            }
            std::ostringstream wrapped;
            wrapped << "cmd /s /c \"" << quotePath(vcvars) << " x64 >nul && " << command.str()
                    << "\"";
            command.str("");
            command.clear();
            command << wrapped.str();
        }
    }
    else {
#ifdef __APPLE__
        const char* sharedFlag = "-dynamiclib";
#else
        const char* sharedFlag = "-shared";
#endif
        command << compilerExecutable(compiler) << " " << sharedFlag << " -fPIC";
        command << " -pthread";
        for (const auto& include : includes) command << " -I" << quotePath(include);
        command << " -o " << quotePath(libraryPath);
        for (const auto& source : sources) command << " " << quotePath(source);
    }

    printf("[mj2x] Compiling native library: %s\n", libraryPath.string().c_str());
    printf("[mj2x] Compiler command: %s\n", command.str().c_str());
    int rc = std::system(command.str().c_str());
    if (rc != 0) {
        fprintf(stderr, "[mj2x] ERROR: C compiler failed with exit code %d.\n", systemExitCode(rc));
        return 1;
    }
    printf("[mj2x] Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

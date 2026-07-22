#include "native_compiler.hpp"

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

namespace {
std::FILE* const kStdoutStream = stdout;
std::FILE* const kStderrStream = stderr;
}

#include "jdk_finder/jdk_finder.hpp"
#include "libca/process/subprocess.hpp"

namespace mj2x::cli {
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
        fprintf(
            kStderrStream,
            "[mj2x] ERROR: invalid mj2x-crt source pack: %s\n",
            crtRoot.string().c_str());
        return false;
    }

    std::ifstream in(manifest);
    if (!in) {
        fprintf(
            kStderrStream,
            "[mj2x] ERROR: cannot read mj2x-crt manifest: %s\n",
            manifest.string().c_str());
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
                kStderrStream,
                "[mj2x] ERROR: invalid mj2x-crt manifest source path: %s\n",
                line.c_str());
            return false;
        }

        const auto source    = crtRoot / relative;
        const auto extension = source.extension().string();
        if (extension == ".cpp") {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: --compile does not yet support C++ "
                    "mj2x-crt sources; use "
                    "--generate-xmake.\n");
            return false;
        }
        if (extension != ".c") continue;
        if (!std::filesystem::exists(source, ec)) {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: mj2x-crt manifest source is missing: %s\n",
                    source.string().c_str());
            return false;
        }
        sources.push_back(source);
        foundSource = true;
    }

    if (!foundSource) {
        fprintf(kStderrStream, "[mj2x] ERROR: mj2x-crt manifest has no C sources.\n");
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: %s; required to locate JNI headers.\n",
                jdk.reason.c_str());
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot start %s compiler: %s\n",
                compilerName,
                completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: %s compiler failed with exit code %d.\n",
                compilerName,
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
        command.arg("-c");
#ifndef _WIN32
        command.arg("-fPIC");
#endif
        for (const auto& include : includes) command.arg("-I" + include.string());
        command.arg(sources[index].string()).arg("-o").arg(object.string());
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
    return _stricmp(left.c_str(), right) == 0;
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

bool loadMsvcEnvironment(ProcessEnvironment& environment,
                         std::filesystem::path& compilerPath)
{
    if (!envValue("INCLUDE").empty()) {
        compilerPath = "cl";
        return true;
    }

    const auto vcvars = findVcvarsAll();
    if (vcvars.empty()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: MSVC environment is not configured and vcvarsall.bat was not "
                "found.\n");
        return false;
    }
    std::string shellPath = envValue("ComSpec");
    if (shellPath.empty()) shellPath = "cmd.exe";
    ca::process::Command shell(shellPath);
    shell.current_dir(vcvars.parent_path().string());
    shell.env("MJ2X_VCVARS_COMMAND",
              "chcp 65001 >nul && call vcvarsall.bat x64 >nul && set");
    shell.arg("/d").arg("/c").arg("%MJ2X_VCVARS_COMMAND%");
    auto captured = shell.output();
    if (captured.is_err()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot configure MSVC environment: %s\n",
                captured.unwrap_err().to_string().c_str());
        return false;
    }
    auto output = std::move(captured).unwrap();
    if (!output.status.success()) {
        writeProcessOutput(output);
        fprintf(kStderrStream,
                "[mj2x] ERROR: vcvarsall.bat failed with exit code %d.\n",
                output.status.code);
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
    compilerPath = findExecutableOnPath(environmentValue(environment, "PATH"), "cl.exe");
    if (compilerPath.empty()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: vcvarsall.bat completed without making cl.exe available.\n");
        return false;
    }
    return true;
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot create temporary MSVC source: %s\n",
                sourceFile.path().string().c_str());
        return 1;
    }
    source.write(cSource.data(), static_cast<std::streamsize>(cSource.size()));
    source.close();
    if (!source) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot write temporary MSVC source: %s\n",
                sourceFile.path().string().c_str());
        return 1;
    }

    ca::process::Command command(compilerPath.string());
    command.arg("/nologo").arg("/LD").arg("/TC");
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot start MSVC compiler: %s\n",
                completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: MSVC compiler failed with exit code %d.\n",
                output.status.code);
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

int compileLlvmIrSource(const std::string& llvmOutputPath, const std::string& outputPath,
                        LlvmTool tool)
{
    std::error_code ec;
    const std::filesystem::path sourcePath(llvmOutputPath);
    if (!std::filesystem::is_regular_file(sourcePath, ec)) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: emitted LLVM IR source does not exist: %s\n",
                llvmOutputPath.c_str());
        return 1;
    }
    if (!jdk_finder::commandExists("clang")) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: LLVM IR compilation requires clang on PATH.\n");
        return 1;
    }
    if (tool == LlvmTool::Llc && !jdk_finder::commandExists("llc")) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: --llvm-tool llc requires llc on PATH.\n");
        return 1;
    }

    const std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::filesystem::remove(libraryPath, ec);
    ec.clear();

#ifdef __APPLE__
    const char* sharedFlag = "-dynamiclib";
#else
    const char* sharedFlag = "-shared";
#endif

    int result = 1;
    if (tool == LlvmTool::Clang) {
        printf("[mj2x] Compiling LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command command("clang");
        command.arg(sharedFlag).arg(sourcePath.string()).arg("-o").arg(libraryPath.string());
        result = runCompilerCommand(command, "clang");
    }
    else {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        ScopedTemporaryDirectory objectDirectory;
        if (!objectDirectory.create(parent, ".mj2x-llvm-")) {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: cannot create temporary LLVM object directory.\n");
            return 1;
        }
#ifdef _WIN32
        const std::filesystem::path objectPath = objectDirectory.path() / "native.obj";
#else
        const std::filesystem::path objectPath = objectDirectory.path() / "native.o";
#endif
        printf("[mj2x] Compiling LLVM IR object with llc: %s\n", objectPath.string().c_str());
        ca::process::Command llcCommand("llc");
        llcCommand.arg("-filetype=obj").arg(sourcePath.string()).arg("-o").arg(
            objectPath.string());
        if (runCompilerCommand(llcCommand, "llc") != 0) return 1;

        printf("[mj2x] Linking LLVM IR native library with clang: %s\n",
               libraryPath.string().c_str());
        ca::process::Command clangCommand("clang");
        clangCommand.arg(sharedFlag).arg(objectPath.string()).arg("-o").arg(
            libraryPath.string());
        result = runCompilerCommand(clangCommand, "clang");
    }

    if (result != 0) {
        std::filesystem::remove(libraryPath, ec);
        return 1;
    }
    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        std::filesystem::remove(libraryPath, ec);
        fprintf(kStderrStream,
                "[mj2x] ERROR: LLVM compiler succeeded without a native library: %s\n",
                libraryPath.string().c_str());
        return 1;
    }
    printf("[mj2x] Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

int compileNativeSource(const std::string& sourcePath, const std::string& outputPath,
                        CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot,
                        bool useCpp17)
{
    std::error_code ec;
    if (!std::filesystem::exists(sourcePath, ec)) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: emitted native source does not exist: %s\n",
                sourcePath.c_str());
        return 1;
    }

    CCompilerKind compiler = CCompilerKind::Auto;
    if (!detectCompiler(requestedCompiler, compiler)) {
        if (requestedCompiler == CCompilerKind::Auto) {
            fprintf(kStderrStream, "[mj2x] ERROR: no supported C compiler found on PATH.\n");
        }
        else {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: C compiler '%s' not found on PATH.\n",
                    compilerExecutable(requestedCompiler));
        }
        return 1;
    }

    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes)) return 1;

    std::vector<std::filesystem::path> sources{std::filesystem::path(sourcePath)};
    std::vector<std::filesystem::path> crtSources;
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, crtSources)) return 1;

    std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::filesystem::remove(libraryPath, ec);
    ec.clear();
    printf("[mj2x] Compiling native library: %s\n", libraryPath.string().c_str());
    int result = 1;
    std::vector<std::filesystem::path> cppCrtObjects;
    ScopedTemporaryDirectory             cppObjectDirectory;
    if (useCpp17 && compiler != CCompilerKind::Msvc && !crtSources.empty()) {
        std::filesystem::path parent = libraryPath.parent_path();
        if (parent.empty()) parent = ".";
        if (!cppObjectDirectory.create(parent, ".mj2x-cpp-")) {
            fprintf(kStderrStream, "[mj2x] ERROR: cannot create temporary C++ object directory.\n");
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
        command.arg("/nologo").arg("/LD");
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
        fprintf(kStderrStream, "[mj2x] ERROR: MSVC is only supported on Windows.\n");
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: compiler succeeded without a native library: %s\n",
                libraryPath.string().c_str());
        return 1;
    }
    printf("[mj2x] Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot)
{
    return compileNativeSource(cOutputPath, outputPath, requestedCompiler, crtRoot, false);
}

int compileCppSource(const std::string& cppOutputPath, const std::string& outputPath,
                     CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot)
{
    return compileNativeSource(cppOutputPath, outputPath, requestedCompiler, crtRoot, true);
}

int compileCSourceFromPipe(std::string_view cSource, const std::string& outputPath,
                           CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot)
{
    if (cSource.empty()) {
        fprintf(kStderrStream, "[mj2x] ERROR: generated C source is empty.\n");
        return 1;
    }

    CCompilerKind compiler = CCompilerKind::Auto;
    if (!detectCompiler(requestedCompiler, compiler)) {
        if (requestedCompiler == CCompilerKind::Auto) {
            fprintf(kStderrStream, "[mj2x] ERROR: no supported C compiler found on PATH.\n");
        }
        else {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: C compiler '%s' not found on PATH.\n",
                    compilerExecutable(requestedCompiler));
        }
        return 1;
    }
    std::vector<std::filesystem::path> includes;
    if (!jniIncludePaths(includes)) return 1;

    std::vector<std::filesystem::path> sources;
    if (!crtRoot.empty() && !addCrtSources(crtRoot, includes, sources)) return 1;

    const std::filesystem::path libraryPath = nativeLibraryPath(outputPath);
    std::error_code             ec;
    std::filesystem::remove(libraryPath, ec);
    ec.clear();
#ifdef _WIN32
    if (compiler == CCompilerKind::Msvc) {
        printf("[mj2x] Compiling native library with MSVC temporary source fallback: %s\n",
               libraryPath.string().c_str());
        const int rc =
            compileMsvcSourceFromTemporaryFile(cSource, libraryPath, includes, sources);
        if (rc != 0) return rc;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(libraryPath, ec) ||
            std::filesystem::file_size(libraryPath, ec) == 0) {
            fprintf(kStderrStream,
                    "[mj2x] ERROR: MSVC succeeded without a native library: %s\n",
                    libraryPath.string().c_str());
            return 1;
        }
        printf("[mj2x] Native library written: %s\n", libraryPath.string().c_str());
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

    printf("[mj2x] Compiling native library from stdin: %s\n", libraryPath.string().c_str());
    auto spawned = command.spawn();
    if (spawned.is_err()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot start C compiler: %s\n",
                spawned.unwrap_err().to_string().c_str());
        return 1;
    }

    auto child = std::move(spawned).unwrap();
    auto input = child.take_stdin();
    if (!input.has_value()) {
        child.kill();
        child.wait();
        fprintf(kStderrStream, "[mj2x] ERROR: C compiler stdin pipe is unavailable.\n");
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
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot write generated C source to compiler stdin: %s\n",
                written.to_string().c_str());
        return 1;
    }

    auto completed = child.wait_with_output();
    if (completed.is_err()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: cannot wait for C compiler: %s\n",
                completed.unwrap_err().to_string().c_str());
        return 1;
    }
    auto output = std::move(completed).unwrap();
    writeProcessOutput(output);
    if (!output.status.success()) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: C compiler failed with exit code %d.\n",
                output.status.code);
        return 1;
    }

    if (!std::filesystem::is_regular_file(libraryPath, ec) ||
        std::filesystem::file_size(libraryPath, ec) == 0) {
        fprintf(kStderrStream,
                "[mj2x] ERROR: C compiler succeeded without a native library: %s\n",
                libraryPath.string().c_str());
        return 1;
    }
    printf("[mj2x] Native library written: %s\n", libraryPath.string().c_str());
    return 0;
}

}   // namespace mj2x::cli

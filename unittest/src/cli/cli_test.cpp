#include <gmock/gmock.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "jzip/zip_io.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "libca/zip/checksum.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

struct ExecResult
{
    int         exitCode = -1;
    std::string out;
};

bool fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string q(const fs::path& path)
{
    std::string s = path.string();
#ifdef _WIN32
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return "\"" + s + "\"";
}

std::string quoteArg(const std::string& value)
{
    return "\"" + value + "\"";
}

/// Sanitize an inherited PATH for batch scripts:
///   - drop segments that still contain an unexpanded `%VAR%` (e.g.
///     `D:\WorkSpace\morpher\%HOME_NODE%\bin` from a developer shell);
///   - collapse double backslashes in non-UNC segments (`morpher\\bin`), which
///     cmd produces when an undefined variable above expands to empty.
/// Both shield the real `xmake f` from a malformed PATH that crashes xmake 3.0.9.
std::string sanitizePathForBatch(const char* pathEnv)
{
    std::string out;
    std::string seg;
    if (!pathEnv) return out;
    for (const char* p = pathEnv;; ++p) {
        if (*p == ';' || *p == '\0') {
            if (!seg.empty() && seg.find('%') == std::string::npos) {
                if (seg[0] != '\\' || seg.size() < 2 || seg[1] != '\\') {
                    std::string folded;
                    for (size_t i = 0; i < seg.size();) {
                        if (seg[i] == '\\' && i + 1 < seg.size() && seg[i + 1] == '\\') {
                            folded += '\\';
                            i += 2;
                        }
                        else {
                            folded += seg[i];
                            ++i;
                        }
                    }
                    seg = folded;
                }
                if (!out.empty()) out += ';';
                out += seg;
            }
            seg.clear();
            if (*p == '\0') break;
        }
        else {
            seg += *p;
        }
    }
    return out;
}

ExecResult runScript(const std::vector<std::string>& lines)
{
    ExecResult result;
    fs::path   tmpDir = test::getTempOutPath("mj2x_cli_test.keep").parent_path();
#ifdef _WIN32
    fs::path script =
        tmpDir /
        ("mj2x_cli_test_" +
         std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".bat");
    {
        std::ofstream out(script);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        std::string cleanPath = sanitizePathForBatch(std::getenv("PATH"));
        if (!cleanPath.empty()) out << "set \"PATH=" << cleanPath << "\"\n";
        for (const auto& line : lines) out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }
#else
    fs::path script =
        tmpDir / ("mj2x_cli_test_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".sh");
    {
        std::ofstream out(script);
        out << "#!/usr/bin/env sh\n";
        for (const auto& line : lines) out << line << "\n";
    }
#endif

#ifdef _WIN32
    std::string command = q(script) + " 2>&1";
#else
    std::string command = "sh " + q(script) + " 2>&1";
#endif
    FILE* pipe =
#ifdef _WIN32
        _popen(command.c_str(), "rb");
#else
        popen(command.c_str(), "r");
#endif
    if (!pipe) {
        result.out = "failed to run " + script.string();
        return result;
    }
    char buffer[4096];
    while (size_t n = fread(buffer, 1, sizeof(buffer), pipe)) {
        result.out.append(buffer, n);
    }
#ifdef _WIN32
    result.exitCode = _pclose(pipe);
#else
    result.exitCode = pclose(pipe);
#endif
    std::error_code ec;
    fs::remove(script, ec);
    return result;
}

fs::path findCli()
{
    auto path = test::getBuildTargetPath("mj2x-cli");
    if (fileExists(path)) return path;
    return {};
}

bool commandAvailable(const char* command)
{
#ifdef _WIN32
    return std::system((std::string("where ") + command + " >nul 2>nul").c_str()) == 0;
#else
    return std::system((std::string("command -v ") + command + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

fs::path findJavac()
{
    std::vector<fs::path> roots;
    if (const char* javaHome = std::getenv("JAVA_HOME")) {
        if (javaHome[0] != '\0') roots.emplace_back(javaHome);
    }
#ifdef _WIN32
    roots.emplace_back("D:/sdk/java/jdk17");
#endif
    for (const auto& root : roots) {
#ifdef _WIN32
        fs::path javac = root / "bin" / "javac.exe";
#else
        fs::path javac = root / "bin" / "javac";
#endif
        if (fileExists(javac)) return javac;
    }
    return {};
}

/// @brief 定位 zig 工具链：PATH 可解析时返回空（无需注入）；否则按 ZIG_HOME、
/// 常见安装根（D:/sdk 下 zig-* 目录）兜底发现，返回 zig.exe 所在目录。
fs::path findZigDir()
{
    if (commandAvailable("zig")) return {};
    std::vector<fs::path> roots;
    if (const char* zigHome = std::getenv("ZIG_HOME")) {
        if (zigHome[0] != '\0') roots.emplace_back(zigHome);
    }
#ifdef _WIN32
    roots.emplace_back("D:/sdk");
#endif
    for (const auto& root : roots) {
        if (fileExists(root / "zig.exe")) return root;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            if (fileExists(it->path() / "zig.exe")) return it->path();
        }
    }
    return {};
}

/// @brief 把目录前置到当前进程 PATH，使后续 runScript 子进程可见（findZigDir
/// 兜底发现的配套；仅影响本测试进程）。
void prependToProcessPath(const fs::path& dir)
{
#ifdef _WIN32
    const char* separator = ";";
#else
    const char* separator = ":";
#endif
    const char* current = std::getenv("PATH");
    const std::string updated =
        dir.string() + (current ? (std::string(separator) + current) : std::string());
#ifdef _WIN32
    _putenv(("PATH=" + updated).c_str());
#else
    setenv("PATH", updated.c_str(), 1);
#endif
}

std::string classPath(const fs::path& first, const fs::path& second)
{
#ifdef _WIN32
    return first.string() + ";" + second.string();
#else
    return first.string() + ":" + second.string();
#endif
}

bool hasAutoNativeCompiler()
{
#ifdef _WIN32
    return commandAvailable("cl") || commandAvailable("clang") || commandAvailable("gcc") ||
           commandAvailable("zig");
#elif defined(__APPLE__)
    return commandAvailable("clang") || commandAvailable("gcc") || commandAvailable("zig");
#else
    return commandAvailable("gcc") || commandAvailable("clang") || commandAvailable("zig");
#endif
}

fs::path nativeLibraryPathFor(const fs::path& outputPath)
{
#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    return outputPath.parent_path() / (outputPath.stem().string() + ext);
}

void addStaticIntMethod(jbytecode::tree::ClassNode& cn, const char* name, int opcode,
                        bool nativeAnnotated)
{
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    std::vector<Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static(name),
                                           ZUtf8StringRef::from_static("(II)I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    if (nativeAnnotated) {
        mn->visibleAnnotations.push_back(
            mn->create<AnnotationNode>(mn->arena(), ZUtf8StringRef::from_static("Lmj2x/Native;")));
    }
    mn->maxStack  = 2;
    mn->maxLocals = 2;
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(opcode));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn.methods.push_back(mn.release());
}

std::vector<ca::u8> tinyClassBytes(const char* className)
{
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    auto            cn = std::make_unique<ClassNode>();
    Utf8StringArena arena;
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->name      = arena.intern(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<Utf8StringRef> noExceptions;
    auto mn       = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static("add"),
                                           ZUtf8StringRef::from_static("(II)I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    mn->maxStack  = 2;
    mn->maxLocals = 2;
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(Opcodes::IADD));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn->methods.push_back(mn.release());

    ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&writer);
    return writer.toByteArray();
}

std::vector<ca::u8> selectableClassBytes(const char* className, bool annotateFirstMethod)
{
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    auto            cn = std::make_unique<ClassNode>();
    Utf8StringArena arena;
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->name      = arena.intern(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    addStaticIntMethod(*cn, "annotatedAdd", Opcodes::IADD, annotateFirstMethod);
    addStaticIntMethod(*cn, "plainSub", Opcodes::ISUB, false);

    ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&writer);
    return writer.toByteArray();
}

fs::path writeTinyClass()
{
    auto     bytes = tinyClassBytes("CliRegCTest");
    fs::path path  = test::getTempOutPath("mj2x_cli_regc_input.class");

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path writeSelectableClass(const char* className, const std::string& suffix,
                              bool annotateFirstMethod)
{
    auto     bytes = selectableClassBytes(className, annotateFirstMethod);
    fs::path path  = test::getTempOutPath(suffix);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path compileProvidedAnnotationSample(const fs::path& javac)
{
    fs::path dir = test::getTempOutPath("mj2x_cli_annotation_java.keep").parent_path() /
                   "mj2x_cli_annotation_java";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    fs::path sampleSource = dir / "CliProvidedAnnotationSample.java";
    {
        std::ofstream out(sampleSource);
        out << "public class CliProvidedAnnotationSample {\n";
        out << "  @mj2x.Native\n";
        out << "  public static int annotated(int a, int b) { return a + b; }\n";
        out << "  public static int plain(int a, int b) { return a - b; }\n";
        out << "}\n";
    }

    fs::path annotationSource = test::getTopProjectPath() / "mj2x" / "mj2x-annotations" / "src" /
                                "main" / "java" / "mj2x" / "Native.java";
    auto result = runScript({
        q(javac) + " -d " + q(dir) + " " + q(annotationSource) + " " + q(sampleSource),
    });
    EXPECT_EQ(result.exitCode, 0) << result.out;
    return dir / "CliProvidedAnnotationSample.class";
}

std::unique_ptr<jzip::LocalFileHeader> makeJarEntry(const std::string&         name,
                                                    const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    ca::zip::Crc32 crc;
    crc.update(data.data(), data.size());
    local->setFileName(name);
    local->setGeneralPurposeBitFlag(0x0800);
    local->setCompressionMethod(0);
    local->setCrc32(crc.value());
    local->setCompressedSize(static_cast<ca::i64>(data.size()));
    local->setUncompressedSize(static_cast<ca::i64>(data.size()));
    local->setFileData(data);
    local->setOffset(0);
    return local;
}

struct TestJarEntry
{
    std::string         name;
    std::vector<ca::u8> data;
};

fs::path writeJarWithEntries(const std::string& suffix, const std::vector<TestJarEntry>& entries)
{
    fs::path         path = test::getTempOutPath(suffix);
    jzip::ZipArchive archive;
    for (const auto& entry : entries) {
        archive.addPart(makeJarEntry(entry.name, entry.data));
    }
    auto bytes = jzip::ZipIO::writeDirect(archive);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path writeTinyJar()
{
    return writeJarWithEntries("mj2x_cli_input.jar",
                               {
                                   {"demo/CliJarA.class", tinyClassBytes("demo/CliJarA")},
                                   {"demo/CliJarB.class", tinyClassBytes("demo/CliJarB")},
                                   {"assets/config.txt", std::vector<ca::u8>{'o', 'k'}},
                               });
}

fs::path writeSignedJar()
{
    return writeJarWithEntries("mj2x_cli_signed.jar",
                               {
                                   {"demo/CliJarA.class", tinyClassBytes("demo/CliJarA")},
                                   {"META-INF/MORPHER.SF", std::vector<ca::u8>{'s', 'i', 'g'}},
                               });
}

fs::path writeNoClassJar()
{
    return writeJarWithEntries(
        "mj2x_cli_no_class.jar",
        {
            {"META-INF/MANIFEST.MF", std::vector<ca::u8>{'M', 'a', 'n', 'i', 'f', 'e', 's', 't'}},
            {"assets/config.txt", std::vector<ca::u8>{'o', 'k'}},
        });
}

fs::path writeEmptyJar()
{
    return writeJarWithEntries("mj2x_cli_empty.jar", {});
}

std::vector<ca::u8> readJarEntry(const fs::path& jarPath, const std::string& name)
{
    std::ifstream       in(jarPath, std::ios::binary | std::ios::ate);
    auto                size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto        archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    const auto* local   = archive.getLocalFileByName(name);
    if (local == nullptr) return {};
    return jzip::ZipIO::decompressLocalFile(*local);
}

std::unique_ptr<jbytecode::tree::ClassNode> readClassFromBytes(std::vector<ca::u8>&      bytes,
                                                               ca::str::Utf8StringArena& arena)
{
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    EXPECT_LE(bytes.size(), static_cast<size_t>(std::numeric_limits<ca::i32>::max()));
    auto        cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

std::unique_ptr<jbytecode::tree::ClassNode> readClassFile(const fs::path&           path,
                                                          ca::str::Utf8StringArena& arena)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in);
    auto                size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return readClassFromBytes(bytes, arena);
}

jbytecode::tree::MethodNode* findMethod(jbytecode::tree::ClassNode& cn, const char* name)
{
    for (auto* method : cn.methods) {
        if (method->name.to_std_string() == name) return method;
    }
    return nullptr;
}

std::string readText(const fs::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> readCrtManifest(const fs::path& manifest)
{
    std::ifstream            in(manifest);
    std::vector<std::string> paths;
    std::string              line;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto last = line.find_last_not_of(" \t\r");
        paths.push_back(line.substr(first, last - first + 1));
    }
    return paths;
}

/// 后端 → 期望分发的 mj2x-rt 挂载集(embedded_rt.hpp rtFoldersForBackend 的
/// 独立复述,分发契约变化时两侧同步)。
const std::vector<std::string>& expectedRtMounts(const std::string& backend)
{
    static const std::vector<std::string> kStackC{"stackc"};
    static const std::vector<std::string> kRegC{"common", "regc"};
    static const std::vector<std::string> kVmc4{"common", "vmc4"};
    static const std::vector<std::string> kLlvm{"common", "regc", "llvmir"};
    static const std::vector<std::string> kNone;
    if (backend == "stackc") return kStackC;
    if (backend == "regc") return kRegC;
    if (backend == "vmc4") return kVmc4;
    if (backend == "llvm") return kLlvm;
    return kNone;
}

/// 按 rt.resources 清单逐挂载核对生成工程:分发面内逐文件字节比对,
/// 分发面外逐文件断言缺席。
void compareRtTreePerBackend(const fs::path& projectDir, const fs::path& crtRoot,
                             const std::string& backend)
{
    const auto                     expected = expectedRtMounts(backend);
    std::vector<std::pair<std::string, fs::path>> mounts;
    for (const auto& entry : readCrtManifest(crtRoot / "rt.resources")) {
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string mount = entry.substr(0, eq);
        if (mount == "templates") continue;  // 模板不随树展开
        mounts.emplace_back(mount, crtRoot / entry.substr(eq + 1));
    }
    ASSERT_FALSE(mounts.empty());
    for (const auto& [mount, mountDir] : mounts) {
        const bool distributed =
            std::find(expected.begin(), expected.end(), mount) != expected.end();
        for (const auto& item : fs::recursive_directory_iterator(mountDir)) {
            if (!item.is_regular_file()) continue;
            const fs::path relative   = item.path().lexically_relative(mountDir);
            const fs::path copiedPath = projectDir / "src" / "mj2x-rt" / mount / relative;
            if (!distributed) {
                EXPECT_FALSE(fileExists(copiedPath)) << mount << "/" << relative.string();
                continue;
            }
            ASSERT_TRUE(fileExists(copiedPath)) << relative.string();
            EXPECT_EQ(readText(copiedPath), readText(item.path())) << relative.string();
        }
    }
}

void writeText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

size_t countOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

std::vector<ca::u8> readFileBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in);
    auto                size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

/// 生成 before/after 的逐行差异摘要（最多 maxDiff 行），供 xmake 配置守卫的
/// 失败信息使用。conf 行序由 xmake 写入时决定，逐位对齐足够定位被翻转的开关。
std::string diffSummary(const std::string& before, const std::string& after, size_t maxDiff)
{
    const auto splitLines = [](const std::string& text) {
        std::vector<std::string> lines;
        size_t                   start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (end == text.size()) break;
            start = end + 1;
        }
        return lines;
    };
    const auto beforeLines = splitLines(before);
    const auto afterLines  = splitLines(after);
    std::string out;
    size_t      shown = 0;
    size_t      i     = 0;
    const auto  overflow = [&out, &shown, maxDiff]() {
        if (shown >= maxDiff) {
            out += "...（其余差异省略）\n";
            return true;
        }
        return false;
    };
    for (; i < beforeLines.size() && i < afterLines.size(); ++i) {
        if (beforeLines[i] == afterLines[i]) continue;
        if (overflow()) return out;
        out += "-" + beforeLines[i] + "\n+" + afterLines[i] + "\n";
        ++shown;
    }
    for (; i < beforeLines.size(); ++i) {
        if (overflow()) return out;
        out += "-" + beforeLines[i] + "\n";
        ++shown;
    }
    for (; i < afterLines.size(); ++i) {
        if (overflow()) return out;
        out += "+" + afterLines[i] + "\n";
        ++shown;
    }
    return out;
}

/// 快照仓库根 xmake 状态目录下的全部平台配置：扫描 <top>/.xmake/<plat>/<arch>/
/// xmake.conf，返回 {相对路径 → 文本内容}。状态目录不存在时返回空表（守卫据此
/// 整体跳过比对，兼容从未在本机配置过根工程的 CI 环境）。
std::map<std::string, std::string> snapshotRepoXmakeConfs()
{
    std::map<std::string, std::string> snapshot;
    const fs::path                     stateRoot = test::getTopProjectPath() / ".xmake";
    std::error_code                    ec;
    if (!fs::is_directory(stateRoot, ec)) return snapshot;
    for (const auto& plat : fs::directory_iterator(stateRoot, ec)) {
        std::error_code platEc;
        if (!plat.is_directory(platEc)) continue;
        for (const auto& arch : fs::directory_iterator(plat.path(), ec)) {
            std::error_code   archEc;
            if (!arch.is_directory(archEc)) continue;
            const fs::path conf = arch.path() / "xmake.conf";
            std::error_code confEc;
            if (!fs::is_regular_file(conf, confEc)) continue;
            snapshot[conf.lexically_relative(test::getTopProjectPath()).generic_string()] =
                readText(conf);
        }
    }
    return snapshot;
}

}   // namespace

/// CliTest 套件公共 fixture（issue #892 守卫）：套件内每个用例运行前后比对
/// 仓库根 xmake 配置（<top>/.xmake/<plat>/<arch>/xmake.conf 全量快照），内容
/// 被改写/删除/新增即在收尾阶段失败并输出差异摘要——把"跑一次全量测试静默
/// 重置开发者 xmake 配置"从 mtime 人工对时变成显式测试失败。
///
/// 背景：xmake 3.x 传 -P 时只切换工程加载点，配置存储目录跟随 CWD（config.lua
/// 的 _use_workingdir 语义）——任何在仓库根 CWD 下触发的 `xmake f` 都会把根
/// 工程配置重置为默认开关。本套件驱动真实 xmake 的用例（独立编译 / CLI
/// --generate-xmake --compile / --c-compiler xmake）全部纳入守卫覆盖；测试
/// 自身在 test/ 下的临时工程配置不位于仓库根 .xmake，不受守卫干扰。
class CliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::error_code ec;
        rootStateExists_ = fs::is_directory(test::getTopProjectPath() / ".xmake", ec);
        if (rootStateExists_) confsBefore_ = snapshotRepoXmakeConfs();
    }

    void TearDown() override
    {
        if (!rootStateExists_) return;   // 根状态目录不存在：跳过比对
        const auto after = snapshotRepoXmakeConfs();
        for (const auto& entry : confsBefore_) {
            const auto it = after.find(entry.first);
            if (it == after.end()) {
                ADD_FAILURE() << "仓库根 xmake 配置在测试运行期间被删除: " << entry.first;
                continue;
            }
            if (it->second != entry.second) {
                ADD_FAILURE() << "仓库根 xmake 配置在测试运行期间被改写（issue #892）: "
                              << entry.first << "\n--- 期望（测试前）\n+++ 实际（测试后）\n"
                              << diffSummary(entry.second, it->second, 20);
            }
        }
        for (const auto& entry : after) {
            if (confsBefore_.find(entry.first) == confsBefore_.end()) {
                ADD_FAILURE() << "测试运行期间新增了仓库根 xmake 配置: " << entry.first;
            }
        }
    }

private:
    std::map<std::string, std::string> confsBefore_;
    bool                               rootStateExists_ = false;
};

TEST(CliTest, NativeAnnotationOnlyCompilesAnnotatedMethods)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass =
        writeSelectableClass("CliAnnotationSelect", "mj2x_cli_annotation_select_input.class", true);
    fs::path outClass = test::getTempOutPath("mj2x_cli_annotation_select_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_annotation_select_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto                     cn        = readClassFile(outClass, arena);
    auto*                    annotated = findMethod(*cn, "annotatedAdd");
    auto*                    plain     = findMethod(*cn, "plainSub");
    ASSERT_NE(annotated, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE((annotated->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_EQ(annotated->instructions.size(), 0);
    EXPECT_FALSE((plain->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_GT(plain->instructions.size(), 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
    EXPECT_NE(source.find("{(char*)\"annotatedAdd\", (char*)\"(II)I\""), std::string::npos)
        << source;
    EXPECT_EQ(source.find("{(char*)\"plainSub\""), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_CliAnnotationSelect_annotatedAdd"), std::string::npos) << source;
}

TEST(CliTest, NativeAnnotationWithNoMatchesMarksNoMethods)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass =
        writeSelectableClass("CliAnnotationNone", "mj2x_cli_annotation_none_input.class", false);
    fs::path outClass = test::getTempOutPath("mj2x_cli_annotation_none_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_annotation_none_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto                     cn     = readClassFile(outClass, arena);
    auto*                    first  = findMethod(*cn, "annotatedAdd");
    auto*                    second = findMethod(*cn, "plainSub");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE((first->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_FALSE((second->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_EQ(source.find("{(char*)\"annotatedAdd\""), std::string::npos) << source;
    EXPECT_EQ(source.find("{(char*)\"plainSub\""), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_CliAnnotationNone_annotatedAdd"), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_CliAnnotationNone_plainSub"), std::string::npos) << source;
}

TEST(CliTest, NativeMethodPrefixIsRepeatableAndSelectsMatchingMethods)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass =
        writeSelectableClass("CliPrefixSelect", "mj2x_cli_prefix_select_input.class", false);
    fs::path outClass = test::getTempOutPath("mj2x_cli_prefix_select_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_prefix_select_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-method-prefix " + quoteArg("CliPrefixSelect.annotated") +
            " --native-method-prefix " + quoteArg("CliPrefixSelect.plain"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto                     cn        = readClassFile(outClass, arena);
    auto*                    annotated = findMethod(*cn, "annotatedAdd");
    auto*                    plain     = findMethod(*cn, "plainSub");
    ASSERT_NE(annotated, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE((annotated->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_TRUE((plain->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("{(char*)\"annotatedAdd\", (char*)\"(II)I\""), std::string::npos)
        << source;
    EXPECT_NE(source.find("{(char*)\"plainSub\", (char*)\"(II)I\""), std::string::npos) << source;
}

TEST(CliTest, ProvidedJavaAnnotationCanSelectMethods)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    fs::path javac = findJavac();
    if (javac.empty()) {
        GTEST_SKIP() << "javac not found; set JAVA_HOME";
    }

    fs::path inClass = compileProvidedAnnotationSample(javac);
    ASSERT_TRUE(fileExists(inClass));
    fs::path outClass = test::getTempOutPath("mj2x_cli_provided_annotation_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_provided_annotation_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto                     cn        = readClassFile(outClass, arena);
    auto*                    annotated = findMethod(*cn, "annotated");
    auto*                    plain     = findMethod(*cn, "plain");
    ASSERT_NE(annotated, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE((annotated->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_FALSE((plain->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("{(char*)\"annotated\", (char*)\"(II)I\""), std::string::npos) << source;
    EXPECT_EQ(source.find("{(char*)\"plain\""), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_CliProvidedAnnotationSample_annotated"), std::string::npos)
        << source;
}

TEST(CliTest, RegCBackendFlagEmitsRegCSource)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass  = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_regc_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_regc_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --backend regc",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outClass));
    ASSERT_TRUE(fileExists(cFile));

    std::string source = readText(cFile);
    EXPECT_NE(source.find("Generated by mj2x RegCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("regc_CliRegCTest_add__II_I"), std::string::npos) << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, LlvmBackendFlagEmitsTextualIrAndMarksStaticMethodNative)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path inClass  = writeTinyClass();
    const fs::path outClass = test::getTempOutPath("mj2x_cli_llvm_output.class");
    const fs::path llvmFile = test::getTempOutPath("mj2x_cli_llvm_output.ll");
    const auto result = runScript({q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                                   " --backend llvm --emit-llvm " + q(llvmFile)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outClass));
    ASSERT_TRUE(fileExists(llvmFile));

    const std::string source = readText(llvmFile);
    EXPECT_NE(source.find("; ModuleID = 'mj2x_llvm'"), std::string::npos) << source;
    EXPECT_NE(source.find("define internal i32 @mj2x_llvm_Java_CliRegCTest_add__II"),
              std::string::npos)
        << source;
    EXPECT_NE(source.find("define dllexport i32 @Java_CliRegCTest_add__II"), std::string::npos)
        << source;

    ca::str::Utf8StringArena arena;
    auto classNode = readClassFile(outClass, arena);
    auto* add = findMethod(*classNode, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE((add->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_EQ(add->instructions.size(), 0);
}

#endif  // !MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, StackCBackendFlagAndLegacyAliasEmitStackCSource)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "cbackend"}) {
        fs::path inClass = writeTinyClass();
        fs::path outClass =
            test::getTempOutPath(std::string("mj2x_cli_") + backendName + "_output.class");
        fs::path cFile = test::getTempOutPath(std::string("mj2x_cli_") + backendName + "_output.c");

        auto result = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
                " --backend " + backendName,
        });
        ASSERT_EQ(result.exitCode, 0) << result.out;
        ASSERT_TRUE(fileExists(outClass));
        ASSERT_TRUE(fileExists(cFile));

        std::string source = readText(cFile);
        EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos) << source;
        EXPECT_NE(source.find("union jvalue"), std::string::npos) << source;
    }
}

TEST(CliTest, JarInputEmitsJarAndCSource)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inJar  = writeTinyJar();
    fs::path outJar = test::getTempOutPath("mj2x_cli_output.jar");
    fs::path cFile  = test::getTempOutPath("mj2x_cli_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outJar));
    ASSERT_TRUE(fileExists(cFile));

    auto resource = readJarEntry(outJar, "assets/config.txt");
    EXPECT_EQ(resource, std::vector<ca::u8>({'o', 'k'}));
    auto cloaderClass = readJarEntry(outJar, "mj2x/cloader/Loader.class");
    EXPECT_FALSE(cloaderClass.empty());

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn  = readClassFromBytes(classBytes, arena);
    auto*                    add = findMethod(*cn, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE((add->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    auto* bridge = findMethod(*cn, "__mj2x_registerClass");
    EXPECT_EQ(bridge, nullptr);
    EXPECT_NE(findMethod(*cn, "<clinit>"), nullptr);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
    EXPECT_NE(source.find("{(char*)\"add\", (char*)\"(II)I\""), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_demo_CliJarA_add"), std::string::npos) << source;
}

TEST(CliTest, RegCJarInputEmitsJarAndRegCSource)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inJar  = writeTinyJar();
    fs::path outJar = test::getTempOutPath("mj2x_cli_regc_output.jar");
    fs::path cFile  = test::getTempOutPath("mj2x_cli_regc_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) +
            " --backend regc",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outJar));
    ASSERT_TRUE(fileExists(cFile));

    auto resource = readJarEntry(outJar, "assets/config.txt");
    EXPECT_EQ(resource, std::vector<ca::u8>({'o', 'k'}));

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn = readClassFromBytes(classBytes, arena);
    // RegC 用 RegisterNatives：受保护类除原方法外还注入一个 <clinit>，
    // 其内调 mj2x/cloader/Loader.registerNativesForClass 触发该类的 native 绑定。
    ASSERT_EQ(cn->methods.size(), 2u);
    bool hasNativeAdd = false;
    bool hasClinit    = false;
    for (auto* m : cn->methods) {
        if (m->name.to_std_string() == "add") {
            EXPECT_TRUE((m->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
            hasNativeAdd = true;
        }
        else if (m->name.to_std_string() == "<clinit>") {
            hasClinit = true;
        }
    }
    EXPECT_TRUE(hasNativeAdd);
    EXPECT_TRUE(hasClinit);
    // mj2x/cloader/Loader 桥接类作为 JAR 资源注入（JNI_OnLoad 绑其 registerNativesForClass）。
    EXPECT_GT(readJarEntry(outJar, "mj2x/cloader/Loader.class").size(), 0u);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("Generated by mj2x RegCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("regc_demo_CliJarA_add__II_I"), std::string::npos) << source;
    // RegC 现用 RegisterNatives 动态绑定（不再 Java_<mangled> 符号导出）。
    EXPECT_NE(source.find("JNIEXPORT jint JNICALL JNI_OnLoad"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
    EXPECT_NE(source.find("__mj2x_c_"), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_demo_CliJarA_add"), std::string::npos) << source;
    EXPECT_NE(source.find("#include \"regc/mj2x_regc_crt.h\""), std::string::npos) << source;
    EXPECT_EQ(countOccurrences(source, "#ifndef MJ2X_REGC_INTRINSIC_ABI_DECLARED"), 1u) << source;
    EXPECT_EQ(countOccurrences(source, "typedef void* mj2x_ref;"), 0u) << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
}

TEST(CliTest, JsonConfigProvidesDefaultsAndCommandLineOverrides)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path    inClass        = writeTinyClass();
    fs::path    configOutClass = test::getTempOutPath("mj2x_cli_config_default_output.class");
    fs::path    configCFile    = test::getTempOutPath("mj2x_cli_config_default_output.c");
    fs::path    overrideOut    = test::getTempOutPath("mj2x_cli_config_override_output.class");
    fs::path    overrideCFile  = test::getTempOutPath("mj2x_cli_config_override_output.c");
    fs::path    configPath     = test::getTempOutPath("mj2x_cli_config.json");
    std::string configSource   = "{\n"
                                 "  \"input\": \"" +
                               inClass.generic_string() +
                               "\",\n"
                               "  \"output\": \"" +
                               configOutClass.generic_string() +
                               "\",\n"
                               "  \"emit_c\": \"" +
                               configCFile.generic_string() +
                               "\",\n"
                               "  \"backend\": \"stackc\",\n"
                               "  \"dump_mir\": true\n"
                               "}\n";
    writeText(configPath, configSource);

    auto result = runScript({
        q(cli) + " --config " + q(configPath) + " -o " + q(overrideOut) + " --emit-c " +
            q(overrideCFile) + " --backend regc",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(overrideOut));
    EXPECT_TRUE(fileExists(overrideCFile));
    EXPECT_FALSE(fileExists(configOutClass));
    EXPECT_FALSE(fileExists(configCFile));

    std::string source = readText(overrideCFile);
    EXPECT_NE(source.find("Generated by mj2x RegCBackend"), std::string::npos) << source;
}

TEST(CliTest, JsonConfigSupportsCppOutputAndCommandLineCanReplaceIt)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path inClass = writeTinyClass();
    const fs::path configOut = test::getTempOutPath("mj2x_cli_config_cpp_output.class");
    const fs::path configCpp = test::getTempOutPath("mj2x_cli_config_cpp_output.cpp");
    const fs::path overrideOut = test::getTempOutPath("mj2x_cli_config_cpp_override.class");
    const fs::path overrideC = test::getTempOutPath("mj2x_cli_config_cpp_override.c");
    const fs::path configPath = test::getTempOutPath("mj2x_cli_config_cpp.json");
    writeText(configPath,
              "{\n"
              "  \"input\": \"" +
                  inClass.generic_string() +
                  "\",\n"
                  "  \"output\": \"" +
                  configOut.generic_string() +
                  "\",\n"
                  "  \"emit_cpp\": \"" +
                  configCpp.generic_string() +
                  "\",\n"
                  "  \"backend\": \"regc\"\n"
                  "}\n");

    auto result = runScript({q(cli) + " --config " + q(configPath)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(configCpp)) << result.out;
    const std::string cpp = readText(configCpp);
    EXPECT_NE(cpp.find("env->"), std::string::npos) << cpp;

    result = runScript({q(cli) + " --config " + q(configPath) + " -o " + q(overrideOut) +
                        " --emit-c " + q(overrideC)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(overrideC)) << result.out;
    EXPECT_NE(readText(overrideC).find("Generated by mj2x RegCBackend"), std::string::npos);
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, JsonConfigSupportsLlvmOutput)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path inClass = writeTinyClass();
    const fs::path outClass = test::getTempOutPath("mj2x_cli_config_llvm_output.class");
    const fs::path llvmFile = test::getTempOutPath("mj2x_cli_config_llvm_output.ll");
    const fs::path configPath = test::getTempOutPath("mj2x_cli_config_llvm.json");
    writeText(configPath,
              "{\n"
              "  \"input\": \"" +
                  inClass.generic_string() +
                  "\",\n"
                  "  \"output\": \"" +
                  outClass.generic_string() +
                  "\",\n"
                  "  \"emit_llvm\": \"" +
                  llvmFile.generic_string() +
                  "\",\n"
                  "  \"backend\": \"llvm\"\n"
                  "}\n");

    const auto result = runScript({q(cli) + " --config " + q(configPath)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(llvmFile));
    EXPECT_NE(readText(llvmFile).find("define dllexport i32 @Java_CliRegCTest_add__II"),
              std::string::npos);
}

#endif  // !MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, ClassInputRepeatableForStackCAndRegC)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "regc"}) {
        fs::path inClass = writeTinyClass();
        fs::path outClassA =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_a.class");
        fs::path outClassB =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_b.class");
        fs::path cFileA =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_a.c");
        fs::path cFileB =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_b.c");

        auto first = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClassA) + " --emit-c " + q(cFileA) +
                " --backend " + backendName,
        });
        ASSERT_EQ(first.exitCode, 0) << first.out;
        auto second = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClassB) + " --emit-c " + q(cFileB) +
                " --backend " + backendName,
        });
        ASSERT_EQ(second.exitCode, 0) << second.out;

        EXPECT_EQ(readFileBytes(outClassA), readFileBytes(outClassB));
        EXPECT_EQ(readText(cFileA), readText(cFileB));
    }
}

TEST(CliTest, JarInputRepeatableForStackCAndRegC)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "regc"}) {
        fs::path inJar = writeTinyJar();
        fs::path outJarA =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_a.jar");
        fs::path outJarB =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_b.jar");
        fs::path cFileA =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_a.c");
        fs::path cFileB =
            test::getTempOutPath(std::string("mj2x_cli_repeat_") + backendName + "_b.c");

        auto first = runScript({
            q(cli) + " -i " + q(inJar) + " -o " + q(outJarA) + " --emit-c " + q(cFileA) +
                " --backend " + backendName,
        });
        ASSERT_EQ(first.exitCode, 0) << first.out;
        auto second = runScript({
            q(cli) + " -i " + q(inJar) + " -o " + q(outJarB) + " --emit-c " + q(cFileB) +
                " --backend " + backendName,
        });
        ASSERT_EQ(second.exitCode, 0) << second.out;

        EXPECT_EQ(readText(cFileA), readText(cFileB));
        EXPECT_EQ(readJarEntry(outJarA, "demo/CliJarA.class"),
                  readJarEntry(outJarB, "demo/CliJarA.class"));
        EXPECT_EQ(readJarEntry(outJarA, "demo/CliJarB.class"),
                  readJarEntry(outJarB, "demo/CliJarB.class"));
        EXPECT_EQ(readJarEntry(outJarA, "assets/config.txt"),
                  readJarEntry(outJarB, "assets/config.txt"));
        if (std::string(backendName) == "stackc") {
            EXPECT_EQ(readJarEntry(outJarA, "mj2x/cloader/Loader.class"),
                      readJarEntry(outJarB, "mj2x/cloader/Loader.class"));
        }
    }
}

TEST(CliTest, GenerateCMakeProjectEmitsStandaloneFiles)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass    = writeTinyClass();
    fs::path outClass   = test::getTempOutPath("mj2x_cli_cmake_project_output.class");
    fs::path projectDir = test::getTempOutPath("mj2x_cli_cmake_project.keep").parent_path() /
                          "mj2x_cli_cmake_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-cmake " + q(projectDir),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outClass));
    EXPECT_TRUE(fileExists(projectDir / "native.c")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "CMakeLists.txt")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / ".gitignore")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "README.md")) << result.out;

    // StackC CMake 工程同样按后端分发 mj2x-rt(仅 stackc 挂载)并注入后端宏。
    EXPECT_TRUE(fileExists(projectDir / "src" / "mj2x-rt" / "stackc" / "mj2x_stackc_runtime.c"))
        << result.out;

    std::string cmake = readText(projectDir / "CMakeLists.txt");
    EXPECT_NE(cmake.find("find_package(JNI REQUIRED COMPONENTS JVM)"), std::string::npos)
        << cmake;
    EXPECT_NE(cmake.find("add_library(mj2x_native SHARED native.c ${MJ2X_CRT_SOURCES})"),
              std::string::npos)
        << cmake;
    EXPECT_NE(cmake.find("src/mj2x-rt/*.c"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("target_compile_definitions(mj2x_native PRIVATE "
                         "MJ2X_RT_BACKEND_STACKC=1)"),
              std::string::npos)
        << cmake;

    std::string source = readText(projectDir / "native.c");
    EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
}

TEST(CliTest, GenerateCMakeProjectForRegcCppCopiesCrt)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path inClass = writeTinyClass();
    const fs::path outClass = test::getTempOutPath("mj2x_cli_cmake_cpp_output.class");
    const fs::path sourcePath = test::getTempOutPath("mj2x_cli_cmake_cpp_native.cpp");
    const fs::path projectDir =
        test::getTempOutPath("mj2x_cli_cmake_cpp_project.keep").parent_path() /
        "mj2x_cli_cmake_cpp_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    auto result = runScript({q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                             " --backend regc --emit-cpp " + q(sourcePath) +
                             " --generate-cmake " + q(projectDir)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "mj2x_cli_cmake_cpp_native.cpp")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "src" / "mj2x-rt" / "regc" / "mj2x_regc_crt.h"))
        << result.out;
    const std::string cmake = readText(projectDir / "CMakeLists.txt");
    EXPECT_NE(cmake.find("project(mj2x_native C CXX)"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("MJ2X_CRT_SOURCES"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("src/mj2x-rt/*.c"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("target_compile_definitions(mj2x_native PRIVATE "
                         "MJ2X_RT_BACKEND_REGC=1)"),
              std::string::npos)
        << cmake;
    EXPECT_NE(cmake.find("CMAKE_CXX_STANDARD 17"), std::string::npos) << cmake;

    if (!commandAvailable("cmake")) return;
    const fs::path buildDir = projectDir / "build";
    result = runScript({"cmake -S " + q(projectDir) + " -B " + q(buildDir)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({"cmake --build " + q(buildDir) + " --config Release"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
}

TEST(CliTest, GenerateXmakeProjectEmitsStandaloneCrtProject)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass    = writeTinyClass();
    fs::path outClass   = test::getTempOutPath("mj2x_cli_xmake_project_output.class");
    fs::path projectDir = test::getTempOutPath("mj2x_cli_xmake_project.keep").parent_path() /
                          "mj2x_cli_xmake_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " + q(projectDir),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outClass));
    EXPECT_TRUE(fileExists(projectDir / "xmake.lua")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / ".gitignore")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "README.md")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "src" / "generated" / "native.c")) << result.out;
    // StackC 分发面:仅 stackc 挂载(rtFoldersForBackend),regc/common 不随工程分发。
    EXPECT_TRUE(fileExists(projectDir / "src" / "mj2x-rt" / "stackc" / "mj2x_stackc_runtime.c"))
        << result.out;
    EXPECT_FALSE(fileExists(projectDir / "src" / "mj2x-rt" / "regc")) << result.out;
    EXPECT_FALSE(fileExists(projectDir / "src" / "mj2x-rt" / "common")) << result.out;

    std::string xmake = readText(projectDir / "xmake.lua");
    EXPECT_NE(xmake.find("target(\"mj2x_native\")"), std::string::npos) << xmake;
    EXPECT_NE(xmake.find("src/generated/native.c"), std::string::npos) << xmake;
    EXPECT_NE(xmake.find("src/generated/native.cpp"), std::string::npos) << xmake;
    EXPECT_NE(xmake.find("src/mj2x-rt/**.c"), std::string::npos) << xmake;
    EXPECT_NE(xmake.find("src/mj2x-rt/**.cpp"), std::string::npos) << xmake;
    EXPECT_EQ(xmake, readText(test::getResourcePath("native_project/xmake.lua.golden")));
    EXPECT_EQ(readText(projectDir / "README.md"),
              readText(test::getResourcePath("native_project/README.md.golden")));
    EXPECT_EQ(readText(projectDir / ".gitignore"),
              readText(test::getResourcePath("native_project/gitignore.golden")));

    std::string source = readText(projectDir / "src" / "generated" / "native.c");
    EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
}

TEST(CliTest, GenerateXmakeProjectUsesEmbeddedCrtOutsideRepo)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeTinyClass();
    fs::path outDir  = test::getTempOutPath("mj2x_cli_xmake_embedded.keep").parent_path() /
                      "mj2x_cli_xmake_embedded";
    fs::path        projectDir = outDir / "project";
    fs::path        outClass   = outDir / "output.class";
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    fs::path isolatedCli = outDir / cli.filename();
    fs::copy_file(cli, isolatedCli, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

#ifdef _WIN32
    std::string poisonCrt = "set MJ2X_CRT_DIR=" + (outDir / "missing-crt").string();
    std::string cdOutDir  = "cd /d " + q(outDir);
#else
    std::string poisonCrt = "MJ2X_CRT_DIR=" + q(outDir / "missing-crt") + "; export MJ2X_CRT_DIR";
    std::string cdOutDir  = "cd " + q(outDir);
#endif

    auto result = runScript({
        poisonCrt,
        cdOutDir,
        q(isolatedCli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " +
            q(projectDir),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "xmake.lua")) << result.out;

    fs::path repoRoot = test::getTopProjectPath();
    fs::path crtRoot  = repoRoot / "mj2x" / "mj2x-rt";
    // StackC 分发面:仅 stackc 挂载逐文件字节比对,其余挂载断言缺席。
    compareRtTreePerBackend(projectDir, crtRoot, "stackc");

    // RegC 分发面:common+regc 比对,stackc/vmc4/llvmir 缺席(同一隔离 exe,
    // 嵌入 bundle 全量在而分发按后端裁剪)。
    fs::path regcProjectDir = outDir / "project_regc";
    fs::path regcOutClass   = outDir / "output_regc.class";
    auto     regcResult     = runScript({
        poisonCrt,
        cdOutDir,
        q(isolatedCli) + " -i " + q(inClass) + " -o " + q(regcOutClass) +
            " --backend regc --generate-xmake " + q(regcProjectDir),
    });
    ASSERT_EQ(regcResult.exitCode, 0) << regcResult.out;
    compareRtTreePerBackend(regcProjectDir, crtRoot, "regc");

    std::string xmake = readText(projectDir / "xmake.lua");
    EXPECT_NE(xmake.find("target(\"mj2x_native\")"), std::string::npos) << xmake;
    EXPECT_EQ(xmake.find("@PROJECT_NAME@"), std::string::npos) << xmake;
#ifndef _WIN32
    EXPECT_NE(xmake.find("add_syslinks(\"pthread\", \"m\")"), std::string::npos) << xmake;
#endif
    EXPECT_EQ(xmake, readText(test::getResourcePath("native_project/xmake.lua.golden")));
    EXPECT_EQ(readText(projectDir / "README.md"),
              readText(test::getResourcePath("native_project/README.md.golden")));
    EXPECT_EQ(readText(projectDir / ".gitignore"),
              readText(test::getResourcePath("native_project/gitignore.golden")));

    if (commandAvailable("xmake")) {
        // Git Bash（MSYS）下 xmake 会把平台自动探测为 mingw 导致 "cannot get
        // program for cc"——Windows 分支显式锁定 windows + msvc 工具链
        // （与 native_project_builder 的子进程调用一致）。
        //
        // xmake 3.x 的 -P 只切换工程加载点，配置存储目录跟随 CWD（config.lua
        // 的 _use_workingdir 语义，xmake issue #3342）：测试进程 CWD 是仓库根，
        // 直接 `xmake f -P <绝对路径>` 会把仓库根工程的
        // .xmake/<plat>/<arch>/xmake.conf 重置为默认开关（issue #892）。
        // 因此先 cd 进生成的临时工程目录再用 -P .，把配置读写锁定在工程内，
        // 与产品侧 native_project_builder 的 current_dir + -P . 契约一致。
#ifdef _WIN32
        auto standaloneBuild = runScript({
            "cd /d " + q(projectDir),
            "xmake f -P . -m release -p windows --toolchain=msvc",
            "xmake build -P . mj2x_native",
        });
#else
        auto standaloneBuild = runScript({
            "cd " + q(projectDir),
            "xmake f -P . -m release",
            "xmake build -P . mj2x_native",
        });
#endif
        ASSERT_EQ(standaloneBuild.exitCode, 0) << standaloneBuild.out;
    }
}

TEST(CliTest, GenerateXmakeProjectPerBackendMountsAndDefines)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_rt_dispatch.keep").parent_path() / "mj2x_cli_rt_dispatch";
    std::error_code ec;
    fs::remove_all(workDir, ec);

    const fs::path crtRoot = test::getTopProjectPath() / "mj2x" / "mj2x-rt";
    // 四后端分发面 + 唯一后端宏:C 工程宏经模板 add_defines,LLVM 内联脚本
    // 直接挂在 clang 命令上。
    const std::vector<std::pair<std::string, std::string>> cases{
        {"stackc", "add_defines(\"MJ2X_RT_BACKEND_STACKC=1\")"},
        {"regc", "add_defines(\"MJ2X_RT_BACKEND_REGC=1\")"},
        {"vmc4", "add_defines(\"MJ2X_RT_BACKEND_VMC4=1\")"},
        {"llvm", "-DMJ2X_RT_BACKEND_LLVM=1"},
    };
    for (const auto& [backend, defineLine] : cases) {
        const fs::path projectDir = workDir / ("proj_" + backend);
        const fs::path inClass    = writeTinyClass();
        const fs::path outClass =
            test::getTempOutPath("mj2x_cli_rt_dispatch_" + backend + ".class");
        std::string command = q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                              " --backend " + backend + " --generate-xmake " + q(projectDir);
        if (backend == "llvm") command += " --emit-llvm " + q(projectDir / "native.ll");
        const auto result = runScript({command});
        ASSERT_EQ(result.exitCode, 0) << result.out;
        compareRtTreePerBackend(projectDir, crtRoot, backend);
        const std::string xmake = readText(projectDir / "xmake.lua");
        EXPECT_NE(xmake.find(defineLine), std::string::npos) << backend << "\n" << xmake;
    }
}

TEST(CliTest, GenerateXmakeProjectRetainsCppSourceArtifact)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path inClass = writeTinyClass();
    const fs::path outClass = test::getTempOutPath("mj2x_cli_xmake_cpp_output.class");
    const fs::path sourcePath = test::getTempOutPath("mj2x_cli_xmake_cpp_native.cpp");
    const fs::path projectDir =
        test::getTempOutPath("mj2x_cli_xmake_cpp_project.keep").parent_path() /
        "mj2x_cli_xmake_cpp_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    const auto result = runScript({q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                                   " --backend regc --emit-cpp " + q(sourcePath) +
                                   " --generate-xmake " + q(projectDir)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(sourcePath)) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "src" / "generated" / "mj2x_cli_xmake_cpp_native.cpp"))
        << result.out;
    EXPECT_FALSE(fileExists(projectDir / "src" / "generated" / "native.c")) << result.out;
    EXPECT_NE(readText(projectDir / "xmake.lua").find("src/generated/native.cpp"),
              std::string::npos);
}

TEST(CliTest, GenerateXmakeProjectForRegCJarInjectsNativeLoader)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inJar      = writeTinyJar();
    fs::path outJar     = test::getTempOutPath("mj2x_cli_xmake_regc_output.jar");
    fs::path projectDir = test::getTempOutPath("mj2x_cli_xmake_regc_project.keep").parent_path() /
                          "mj2x_cli_xmake_regc_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --backend regc --generate-xmake " +
            q(projectDir),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outJar));
    EXPECT_TRUE(fileExists(projectDir / "xmake.lua")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "src" / "generated" / "native.c")) << result.out;

    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn     = readClassFromBytes(classBytes, arena);
    auto*                    clinit = findMethod(*cn, "<clinit>");
    ASSERT_NE(clinit, nullptr);
    bool callsLoader = false;
    for (auto* insn : clinit->instructions) {
        auto* call = ca::core::tag_cast::dyn_cast<jbytecode::tree::MethodInsnNode>(insn);
        if (call == nullptr) continue;
        callsLoader = callsLoader || (call->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
                                      call->name.to_std_string() == "load");
    }
    EXPECT_TRUE(callsLoader);
}

TEST(CliTest, GenerateXmakeAndCompileBuildsNativeLibrary)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("xmake")) GTEST_SKIP() << "xmake is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0')
        GTEST_SKIP() << "JAVA_HOME is not set";

    const fs::path inClass = writeTinyClass();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_xmake_compile.keep").parent_path() /
        "mj2x_cli_xmake_compile";
    const fs::path projectDir    = workDir / "project";
    const fs::path outClass      = workDir / "output.class";
    const fs::path nativeLibrary = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " +
            q(projectDir) + " --compile",
    });

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "xmake.lua")) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    EXPECT_GT(fs::file_size(nativeLibrary), 0U);
}

TEST(CliTest, XmakeCompilerBuildsRegCJarAndCleansTemporaryProject)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("xmake")) GTEST_SKIP() << "xmake is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0')
        GTEST_SKIP() << "JAVA_HOME is not set";

    const fs::path inJar = writeTinyJar();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_xmake_regc.keep").parent_path() /
        "mj2x_cli_xmake_regc";
    const fs::path outJar        = workDir / "output.jar";
    const fs::path nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) +
            " --backend regc --compile --c-compiler xmake",
    });

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    EXPECT_FALSE(readJarEntry(outJar, "mj2x/runtime/NativeLoader.class").empty()) << result.out;
    EXPECT_FALSE(readJarEntry(
                     outJar,
                     "META-INF/mj2x/native/" + nativeLibrary.filename().string())
                     .empty())
        << result.out;
    for (const auto& entry : fs::directory_iterator(workDir))
        EXPECT_NE(entry.path().filename().string().rfind(".mj2x-xmake-", 0), 0U)
            << entry.path().string();
}

TEST(CliTest, XmakeCompilerRejectsFailedOrAmbiguousBuildsAndCleansReadonlyTemporaryProject)
{
    const fs::path cli       = findCli();
    const fs::path fakeXmake = test::getBuildTargetPath("mj2x_fake_xmake");
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!fileExists(fakeXmake)) GTEST_SKIP() << "mj2x_fake_xmake is not built";

    const fs::path inClass = writeTinyClass();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_fake_xmake.keep").parent_path() /
        "mj2x cli fake xmake";
    const fs::path toolDir    = workDir / "tool";
    const fs::path projectDir = workDir / "project";
    const fs::path outClass   = workDir / "output.class";
#ifdef _WIN32
    const fs::path xmakeExecutable = toolDir / "xmake.exe";
#else
    const fs::path xmakeExecutable = toolDir / "xmake";
#endif
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(toolDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy_file(fakeXmake, xmakeExecutable, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
#ifndef _WIN32
    fs::permissions(xmakeExecutable,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);
    ASSERT_FALSE(ec) << ec.message();
#endif

    std::vector<std::string> configureFailureLines;
#ifdef _WIN32
    configureFailureLines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    configureFailureLines.push_back("set \"MJ2X_FAKE_XMAKE_FAIL=configure\"");
#else
    configureFailureLines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    configureFailureLines.push_back("export MJ2X_FAKE_XMAKE_FAIL=configure");
#endif
    configureFailureLines.push_back(
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " +
        q(projectDir) + " --compile");
    auto configureFailure = runScript(configureFailureLines);
    EXPECT_NE(configureFailure.exitCode, 0);
    EXPECT_NE(configureFailure.out.find("xmake configure failed with exit code 21"),
              std::string::npos)
        << configureFailure.out;

    fs::remove_all(projectDir, ec);
    std::vector<std::string> missingArtifactLines;
#ifdef _WIN32
    missingArtifactLines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    missingArtifactLines.push_back("set \"MJ2X_FAKE_XMAKE_FAIL=\"");
    missingArtifactLines.push_back("set \"MJ2X_FAKE_XMAKE_ARTIFACT=none\"");
#else
    missingArtifactLines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    missingArtifactLines.push_back("unset MJ2X_FAKE_XMAKE_FAIL");
    missingArtifactLines.push_back("export MJ2X_FAKE_XMAKE_ARTIFACT=none");
#endif
    missingArtifactLines.push_back(
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " +
        q(projectDir) + " --compile");
    auto missingArtifact = runScript(missingArtifactLines);
    EXPECT_NE(missingArtifact.exitCode, 0);
    EXPECT_NE(missingArtifact.out.find("produced 0 matching native libraries"),
              std::string::npos)
        << missingArtifact.out;

    const fs::path failedJar       = workDir / "failed-output.jar";
    const fs::path failedLibrary   = nativeLibraryPathFor(failedJar);
    const fs::path failureInputJar = writeTinyJar();
    {
        std::ofstream stale(failedLibrary, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stale) << failedLibrary.string();
        stale << "stale native library";
    }
    std::vector<std::string> buildFailureLines;
#ifdef _WIN32
    buildFailureLines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    buildFailureLines.push_back("set \"MJ2X_FAKE_XMAKE_FAIL=build\"");
    buildFailureLines.push_back("set \"MJ2X_FAKE_XMAKE_ARTIFACT=\"");
    buildFailureLines.push_back("set \"MJ2X_FAKE_XMAKE_READONLY_SOURCE=1\"");
#else
    buildFailureLines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    buildFailureLines.push_back("export MJ2X_FAKE_XMAKE_FAIL=build");
    buildFailureLines.push_back("unset MJ2X_FAKE_XMAKE_ARTIFACT");
    buildFailureLines.push_back("export MJ2X_FAKE_XMAKE_READONLY_SOURCE=1");
#endif
    buildFailureLines.push_back(q(cli) + " -i " + q(failureInputJar) + " -o " + q(failedJar) +
                               " --backend regc --compile --c-compiler xmake");
    auto buildFailure = runScript(buildFailureLines);
    EXPECT_NE(buildFailure.exitCode, 0);
    EXPECT_NE(buildFailure.out.find("xmake build failed with exit code 22"), std::string::npos)
        << buildFailure.out;
    EXPECT_FALSE(fileExists(failedLibrary)) << buildFailure.out;
    if (fileExists(failedJar)) {
        EXPECT_TRUE(readJarEntry(
                        failedJar,
                        "META-INF/mj2x/native/" + failedLibrary.filename().string())
                        .empty())
            << buildFailure.out;
    }
    for (const auto& entry : fs::directory_iterator(workDir))
        EXPECT_NE(entry.path().filename().string().rfind(".mj2x-xmake-", 0), 0U)
            << entry.path().string();

    fs::remove_all(projectDir, ec);
    const fs::path ambiguousOut     = workDir / "ambiguous-output.class";
    const fs::path ambiguousLibrary = nativeLibraryPathFor(ambiguousOut);
    {
        std::ofstream stale(ambiguousLibrary, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stale) << ambiguousLibrary.string();
        stale << "stale native library";
    }
    std::vector<std::string> ambiguousArtifactLines;
#ifdef _WIN32
    ambiguousArtifactLines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    ambiguousArtifactLines.push_back("set \"MJ2X_FAKE_XMAKE_FAIL=\"");
    ambiguousArtifactLines.push_back("set \"MJ2X_FAKE_XMAKE_ARTIFACT=multiple\"");
#else
    ambiguousArtifactLines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    ambiguousArtifactLines.push_back("unset MJ2X_FAKE_XMAKE_FAIL");
    ambiguousArtifactLines.push_back("export MJ2X_FAKE_XMAKE_ARTIFACT=multiple");
#endif
    ambiguousArtifactLines.push_back(
        q(cli) + " -i " + q(inClass) + " -o " + q(ambiguousOut) + " --generate-xmake " +
        q(projectDir) + " --compile");
    auto ambiguousArtifact = runScript(ambiguousArtifactLines);
    EXPECT_NE(ambiguousArtifact.exitCode, 0);
    EXPECT_NE(ambiguousArtifact.out.find("produced 2 matching native libraries"),
              std::string::npos)
        << ambiguousArtifact.out;
    EXPECT_FALSE(fileExists(ambiguousLibrary)) << ambiguousArtifact.out;
}

TEST(CliTest, CompileFailsClearlyWhenJavaHomeMissing)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass  = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_compile_no_java_home_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_compile_no_java_home_output.c");

#ifdef _WIN32
    std::string unsetJavaHome = "set JAVA_HOME=";
#else
    std::string unsetJavaHome = "unset JAVA_HOME";
#endif

    auto result = runScript({
        unsetJavaHome,
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --compile",
    });
    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.out.find("required to locate JNI headers"), std::string::npos) << result.out;
}

TEST(CliTest, CompileEmitsNativeLibraryWhenToolchainIsAvailable)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!hasAutoNativeCompiler()) {
        GTEST_SKIP() << "no supported C compiler is available on PATH";
    }

    fs::path        inClass       = writeTinyClass();
    fs::path        outClass      = test::getTempOutPath("mj2x_cli_compile_output.class");
    fs::path        cFile         = test::getTempOutPath("mj2x_cli_compile_output.c");
    fs::path        nativeLibrary = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
}

TEST(CliTest, PipeCompileWithClangDoesNotWriteCSource)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!commandAvailable("clang")) {
        GTEST_SKIP() << "clang is not available on PATH";
    }

    const fs::path inClass = writeTinyClass();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_pipe_compile.keep").parent_path() /
        "mj2x_cli_pipe_compile";
    const fs::path outClass      = workDir / "output.class";
    const fs::path nativeLibrary = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
            " --compile --compile-mode pipe --c-compiler clang",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(outClass)) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    for (const auto& entry : fs::recursive_directory_iterator(workDir)) {
        EXPECT_NE(entry.path().extension(), ".c") << entry.path().string();
    }
}

TEST(CliTest, FakeZigUsesCcForFileAndPipeModes)
{
    const fs::path cli     = findCli();
    const fs::path fakeZig = test::getBuildTargetPath("mj2x_fake_zig");
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!fileExists(fakeZig)) GTEST_SKIP() << "mj2x_fake_zig is not built";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0')
        GTEST_SKIP() << "JAVA_HOME is not set";

    const fs::path inClass = writeTinyClass();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_pipe_fake_zig.keep").parent_path() /
        "mj2x_cli_pipe_fake_zig";
    const fs::path toolDir       = workDir / "tool";
    const fs::path outClass      = workDir / "output.class";
    const fs::path nativeLibrary = nativeLibraryPathFor(outClass);
    const fs::path logFile       = workDir / "zig-args.txt";
    const fs::path sourceFile    = workDir / "zig-stdin.txt";
#ifdef _WIN32
    const fs::path zigExecutable = toolDir / "zig.exe";
#else
    const fs::path zigExecutable = toolDir / "zig";
#endif
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(toolDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy_file(fakeZig, zigExecutable, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
#ifndef _WIN32
    fs::permissions(zigExecutable,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);
    ASSERT_FALSE(ec) << ec.message();
#endif

    std::vector<std::string> lines;
#ifdef _WIN32
    lines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    lines.push_back("set \"MJ2X_FAKE_ZIG_LOG=" + logFile.string() + "\"");
    lines.push_back("set \"MJ2X_FAKE_ZIG_SOURCE=" + sourceFile.string() + "\"");
#else
    lines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    lines.push_back("export MJ2X_FAKE_ZIG_LOG=" + q(logFile));
    lines.push_back("export MJ2X_FAKE_ZIG_SOURCE=" + q(sourceFile));
#endif
    lines.push_back(q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                    " --compile --compile-mode pipe --c-compiler zig");
    auto result = runScript(lines);

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    const std::string log = readText(logFile);
    EXPECT_EQ(log.rfind("cc\n", 0), 0U) << log;
    EXPECT_NE(log.find("-x\nc\n-\n"), std::string::npos) << log;
    EXPECT_NE(readText(sourceFile).find("JNIEXPORT"), std::string::npos);
    for (const auto& entry : fs::recursive_directory_iterator(workDir))
        EXPECT_NE(entry.path().extension(), ".c") << entry.path().string();

    std::ofstream staleLibrary(nativeLibrary, std::ios::binary | std::ios::trunc);
    staleLibrary << "stale native library";
    staleLibrary.close();
    lines.clear();
#ifdef _WIN32
    lines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    lines.push_back("set \"MJ2X_FAKE_ZIG_SKIP_OUTPUT=1\"");
#else
    lines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    lines.push_back("export MJ2X_FAKE_ZIG_SKIP_OUTPUT=1");
#endif
    lines.push_back(q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                    " --compile --compile-mode pipe --c-compiler zig");
    result = runScript(lines);
    EXPECT_NE(result.exitCode, 0) << result.out;
    EXPECT_FALSE(fileExists(nativeLibrary)) << result.out;

    const fs::path fileDir           = workDir / "file-mode";
    const fs::path fileOutClass      = fileDir / "output.class";
    const fs::path fileNativeLibrary = nativeLibraryPathFor(fileOutClass);
    const fs::path generatedSource   = fileDir / "generated.c";
    const fs::path fileLog           = fileDir / "zig-args.txt";
    const fs::path fileStdin         = fileDir / "zig-stdin.txt";
    fs::create_directories(fileDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    lines.clear();
#ifdef _WIN32
    lines.push_back("set \"PATH=" + toolDir.string() + ";%PATH%\"");
    lines.push_back("set \"MJ2X_FAKE_ZIG_LOG=" + fileLog.string() + "\"");
    lines.push_back("set \"MJ2X_FAKE_ZIG_SOURCE=" + fileStdin.string() + "\"");
#else
    lines.push_back("export PATH=" + q(toolDir) + ":$PATH");
    lines.push_back("export MJ2X_FAKE_ZIG_LOG=" + q(fileLog));
    lines.push_back("export MJ2X_FAKE_ZIG_SOURCE=" + q(fileStdin));
#endif
    lines.push_back(q(cli) + " -i " + q(inClass) + " -o " + q(fileOutClass) +
                    " --emit-c " + q(generatedSource) +
                    " --compile --compile-mode file --c-compiler zig");
    result = runScript(lines);

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(fileNativeLibrary)) << result.out;
    EXPECT_TRUE(fileExists(generatedSource)) << result.out;
    const std::string fileArguments = readText(fileLog);
    EXPECT_EQ(fileArguments.rfind("cc\n", 0), 0U) << fileArguments;
    EXPECT_NE(fileArguments.find(generatedSource.string()), std::string::npos) << fileArguments;
    EXPECT_TRUE(readText(fileStdin).empty());
}

TEST(CliTest, RealZigCompilesClassAndJarAndRunsJvm)
{
    const fs::path cli   = findCli();
    const fs::path javac = findJavac();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (javac.empty()) GTEST_SKIP() << "javac not found; set JAVA_HOME";
    const fs::path zigDir = findZigDir();
    if (zigDir.empty() && !commandAvailable("zig")) GTEST_SKIP() << "zig is not available on PATH";
    if (!zigDir.empty()) prependToProcessPath(zigDir);

#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif
    if (!fileExists(java)) GTEST_SKIP() << "java executable is unavailable beside javac";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_real_zig.keep").parent_path() / "mj2x_cli_real_zig";
    const fs::path classDir = workDir / "class-input";
    const fs::path classOut = workDir / "class-output";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(classDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_directories(classOut, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path classSource  = classDir / "CliZigClass.java";
    const fs::path driverSource = classDir / "Driver.java";
    {
        std::ofstream out(classSource);
        ASSERT_TRUE(out) << classSource.string();
        out << "public class CliZigClass {\n"
               "  public static int add(int a, int b) { return a + b; }\n"
               "}\n";
    }
    {
        std::ofstream out(driverSource);
        ASSERT_TRUE(out) << driverSource.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliZigClass.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -d " + q(classDir) + " " + q(classSource) + " " +
                             q(driverSource)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(classDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    const fs::path inputClass    = classDir / "CliZigClass.class";
    const fs::path outputClass   = classOut / "CliZigClass.class";
    const fs::path generatedC    = classOut / "native.c";
    const fs::path nativeLibrary = nativeLibraryPathFor(outputClass);
    result = runScript({q(cli) + " -i " + q(inputClass) + " -o " + q(outputClass) + " --emit-c " +
                        q(generatedC) + " --compile --compile-mode file --c-compiler zig"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(generatedC)) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    fs::copy_file(classDir / "Driver.class", classOut / "Driver.class",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    std::vector<std::string> nativeClassRun;
#ifdef _WIN32
    nativeClassRun.push_back("set \"MORPHER_DLL=" + nativeLibrary.string() + "\"");
#else
    nativeClassRun.push_back("export MORPHER_DLL=" + q(nativeLibrary));
#endif
    nativeClassRun.push_back(q(java) + " -Xverify:all -cp " + q(classOut) + " Driver");
    result = runScript(nativeClassRun);
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    const fs::path jarDir = workDir / "jar";
    fs::create_directories(jarDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    const fs::path inputJar  = writeTinyJar();
    const fs::path outputJar = jarDir / "output.jar";
    const fs::path jarDriver = jarDir / "Driver.java";
    {
        std::ofstream out(jarDriver);
        ASSERT_TRUE(out) << jarDriver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    System.out.print(demo.CliJarA.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    result = runScript({q(javac) + " -cp " + q(inputJar) + " -d " + q(jarDir) + " " +
                        q(jarDriver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(inputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    const fs::path jarNativeLibrary = nativeLibraryPathFor(outputJar);
    result = runScript({q(cli) + " -i " + q(inputJar) + " -o " + q(outputJar) +
                        " --backend regc --compile --compile-mode pipe --c-compiler zig"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(jarNativeLibrary)) << result.out;
    EXPECT_FALSE(readJarEntry(outputJar,
                              "META-INF/mj2x/native/" + jarNativeLibrary.filename().string())
                     .empty())
        << result.out;

    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(outputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");
}

TEST(CliTest, CompileRegCJarWithEncryptedNativeLibRunsJvm)
{
    const fs::path cli   = findCli();
    const fs::path javac = findJavac();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (javac.empty()) GTEST_SKIP() << "javac not found; set JAVA_HOME";
    if (!hasAutoNativeCompiler()) GTEST_SKIP() << "no supported C compiler is available on PATH";

#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif
    if (!fileExists(java)) GTEST_SKIP() << "java executable is unavailable beside javac";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_enc_lib.keep").parent_path() / "mj2x_cli_enc_lib";
    std::error_code ec;
    fs::remove_all(workDir, ec);

    const fs::path jarDir = workDir / "jar";
    fs::create_directories(jarDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path inputJar  = writeTinyJar();
    const fs::path outputJar = jarDir / "output_enc.jar";
    const fs::path jarDriver = jarDir / "Driver.java";
    {
        std::ofstream out(jarDriver);
        ASSERT_TRUE(out) << jarDriver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    System.out.print(demo.CliJarA.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -cp " + q(inputJar) + " -d " + q(jarDir) + " " +
                             q(jarDriver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(inputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    // 加密存储形态：--native-lib-key 时嵌入入口加 .enc 后缀，内容 XOR 加密。
    const fs::path nativeLibrary = nativeLibraryPathFor(outputJar);
    const fs::path cFile         = workDir / "enc_output.c";
    result = runScript({q(cli) + " -i " + q(inputJar) + " -o " + q(outputJar) +
                        " --emit-c " + q(cFile) + " --backend regc --compile --native-lib-key 1234abcd"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;

    const std::string encEntry =
        "META-INF/mj2x/native/" + nativeLibrary.filename().string() + ".enc";
    auto encBytes = readJarEntry(outputJar, encEntry);
    ASSERT_FALSE(encBytes.empty()) << result.out << "（加密资源入口 " << encEntry << " 缺失）";
    // 静态提取的 blob ≠ 明文 dll。
    auto plainBytes = readFileBytes(nativeLibrary);
    ASSERT_FALSE(plainBytes.empty());
    EXPECT_NE(encBytes, plainBytes) << "加密 blob 与明文不应一致";

    // 解密往返：C++ 侧 xorStreamBytes 解出 == 明文（与 NativeLoader 字节码同算法）。
    auto decrypted = encBytes;
    mj2x::xorStreamBytes(decrypted.data(), decrypted.size(), 0x1234ABCDu);
    EXPECT_EQ(decrypted, plainBytes) << "XOR 解密往返与打包侧算法不一致";

    // 运行加密 jar：NativeLoader 检测 .enc 后缀 → 解密 → System.load。
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(outputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");
}

TEST(CliTest, CompileRegCJarWithNativeLibPackHookRunsJvm)
{
    // --native-lib-pack-hook：嵌入前外部处理钩子（sepacker 加壳对接的代码级
    // 预留接口）。验证：hook 被执行、{in} 占位符替换正确、产物照常嵌入运行；
    // 非零退出码 → CLI 失败。
    const fs::path cli   = findCli();
    const fs::path javac = findJavac();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (javac.empty()) GTEST_SKIP() << "javac not found; set JAVA_HOME";
    if (!hasAutoNativeCompiler()) GTEST_SKIP() << "no supported C compiler is available on PATH";

#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif
    if (!fileExists(java)) GTEST_SKIP() << "java executable is unavailable beside javac";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_pack_hook.keep").parent_path() / "mj2x_cli_pack_hook";
    std::error_code ec;
    fs::remove_all(workDir, ec);

    const fs::path jarDir = workDir / "jar";
    fs::create_directories(jarDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path inputJar  = writeTinyJar();
    const fs::path outputJar = jarDir / "output_hook.jar";
    const fs::path jarDriver = jarDir / "Driver.java";
    {
        std::ofstream out(jarDriver);
        ASSERT_TRUE(out) << jarDriver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    System.out.print(demo.CliJarA.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -cp " + q(inputJar) + " -d " + q(jarDir) + " " +
                             q(jarDriver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;

    // hook：把编译产物复制到标记位置（验证执行 + {in} 替换），不改动原文件。
    // 注意：hookCmd 是命令字符串（含 /y 等 shell 语法），不能走 q()（q 经
    // fs::path 规范化会把 / 转成 \）；手动加引号。
    const fs::path markerDll = workDir / "hook_marker.dll";
#ifdef _WIN32
    const std::string hookCmd      = "copy /y {in} " + markerDll.string();
#else
    const std::string hookCmd      = "cp {in} " + markerDll.string();
#endif
    const std::string quotedHook   = "\"" + hookCmd + "\"";

    const fs::path nativeLibrary = nativeLibraryPathFor(outputJar);
    const fs::path cFile         = workDir / "hook_output.c";
    result = runScript({q(cli) + " -i " + q(inputJar) + " -o " + q(outputJar) +
                        " --emit-c " + q(cFile) + " --backend regc --compile" +
                        " --native-lib-pack-hook " + quotedHook});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    EXPECT_TRUE(fileExists(markerDll)) << "hook 未执行或 {in} 占位符未替换";
    EXPECT_GT(fs::file_size(markerDll, ec), 0u) << "hook 复制产物为空";

    // 运行 hook 产物 jar：正常加载（hook 未破坏产物）。
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(outputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    // hook 失败路径：非零退出 → CLI 报错。
    const fs::path badJar = jarDir / "output_bad_hook.jar";
    const fs::path badC   = workDir / "bad_hook.c";
    result = runScript({q(cli) + " -i " + q(inputJar) + " -o " + q(badJar) +
                        " --emit-c " + q(badC) + " --backend regc --compile" +
                        " --native-lib-pack-hook " + q("exit 1")});
    EXPECT_NE(result.exitCode, 0) << "hook 非零退出应使 CLI 失败";
}

TEST(CliTest, RealZigCompilesRegcCppClassAndRunsJvm)
{
    const fs::path cli   = findCli();
    const fs::path javac = findJavac();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (javac.empty()) GTEST_SKIP() << "javac not found; set JAVA_HOME";
    const fs::path zigDir = findZigDir();
    if (zigDir.empty() && !commandAvailable("zig")) GTEST_SKIP() << "zig is not available on PATH";
    if (!zigDir.empty()) prependToProcessPath(zigDir);

#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif
    if (!fileExists(java)) GTEST_SKIP() << "java executable is unavailable beside javac";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_real_zig_cpp.keep").parent_path() / "mj2x_cli_real_zig_cpp";
    const fs::path inputDir  = workDir / "input";
    const fs::path outputDir = workDir / "output";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(inputDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_directories(outputDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path source = inputDir / "CliCppRegC.java";
    const fs::path driver = inputDir / "Driver.java";
    {
        std::ofstream out(source);
        ASSERT_TRUE(out) << source.string();
        out << "public class CliCppRegC {\n"
               "  public static int add(int a, int b) { return a + b; }\n"
               "}\n";
    }
    {
        std::ofstream out(driver);
        ASSERT_TRUE(out) << driver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliCppRegC.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -d " + q(inputDir) + " " + q(source) + " " +
                             q(driver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(inputDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    const fs::path inputClass    = inputDir / "CliCppRegC.class";
    const fs::path outputClass   = outputDir / "CliCppRegC.class";
    const fs::path generatedCpp  = outputDir / "native.cpp";
    const fs::path nativeLibrary = nativeLibraryPathFor(outputClass);
    result = runScript({q(cli) + " -i " + q(inputClass) + " -o " + q(outputClass) +
                        " --backend regc --emit-cpp " + q(generatedCpp) +
                        " --compile --c-compiler zig"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(generatedCpp)) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    const std::string cpp = readText(generatedCpp);
    EXPECT_NE(cpp.find("env->"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("extern \"C\" JNIEXPORT"), std::string::npos) << cpp;
    EXPECT_EQ(cpp.find("(*env)->"), std::string::npos) << cpp;

    fs::copy_file(inputDir / "Driver.class", outputDir / "Driver.class",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::vector<std::string> nativeRun;
#ifdef _WIN32
    nativeRun.push_back("set \"MORPHER_DLL=" + nativeLibrary.string() + "\"");
#else
    nativeRun.push_back("export MORPHER_DLL=" + q(nativeLibrary));
#endif
    nativeRun.push_back(q(java) + " -Xverify:all -cp " + q(outputDir) + " Driver");
    result = runScript(nativeRun);
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    const fs::path jarDir = workDir / "jar";
    fs::create_directories(jarDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    const fs::path inputJar       = writeTinyJar();
    const fs::path outputJar      = jarDir / "output.jar";
    const fs::path jarSource      = jarDir / "native.cpp";
    const fs::path jarDriver      = jarDir / "Driver.java";
    const fs::path jarNativeLibrary = nativeLibraryPathFor(outputJar);
    {
        std::ofstream out(jarDriver);
        ASSERT_TRUE(out) << jarDriver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    System.out.print(demo.CliJarA.add(20, 22));\n"
               "  }\n"
               "}\n";
    }
    result = runScript({q(javac) + " -cp " + q(inputJar) + " -d " + q(jarDir) + " " +
                        q(jarDriver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(inputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    result = runScript({q(cli) + " -i " + q(inputJar) + " -o " + q(outputJar) +
                        " --backend regc --emit-cpp " + q(jarSource) +
                        " --compile --c-compiler zig"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(jarNativeLibrary)) << result.out;
    EXPECT_FALSE(readJarEntry(outputJar,
                              "META-INF/mj2x/native/" + jarNativeLibrary.filename().string())
                     .empty())
        << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(classPath(outputJar, jarDir)) +
                        " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, LlvmClangAndLlcCompileLoopSwitchAndRunJvm)
{
    const fs::path cli   = findCli();
    const fs::path javac = findJavac();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (javac.empty()) GTEST_SKIP() << "javac not found; set JAVA_HOME";
    if (!commandAvailable("clang")) GTEST_SKIP() << "clang is not available on PATH";
    if (!commandAvailable("llc")) GTEST_SKIP() << "llc is not available on PATH";

#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif
    if (!fileExists(java)) GTEST_SKIP() << "java executable is unavailable beside javac";

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_llvm_e2e.keep").parent_path() / "mj2x_cli_llvm_e2e";
    const fs::path inputDir = workDir / "input";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(inputDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path source = inputDir / "CliLlvm.java";
    const fs::path driver = inputDir / "Driver.java";
    {
        std::ofstream out(source);
        ASSERT_TRUE(out) << source.string();
        out << "public class CliLlvm {\n"
               "  public static int score(int n) {\n"
               "    int sum = 0;\n"
               "    for (int i = 0; i < n; i++) sum += i;\n"
               "    switch (n) {\n"
               "      case 5: return sum + 7;\n"
               "      case 8: return sum + 3;\n"
               "      default: return sum - 1;\n"
               "    }\n"
               "  }\n"
               "  public static int helper(int a, int b) {\n"
               "    return (a << 3) + (b >>> 1);\n"
               "  }\n"
               "  public static int compute(int x) {\n"
               "    return helper(x, 2) ^ (x >> 2);\n"
               "  }\n"
               "  public static int divide(int a, int b) { return a / b; }\n"
               "  public static int remainder(int a, int b) { return a % b; }\n"
               "  public static long factorial(int n) {\n"
               "    return n <= 1 ? 1 : n * factorial(n - 1);\n"
               "  }\n"
               "  public static int f2i(float f) { return (int) f; }\n"
               "}\n";
    }
    {
        std::ofstream out(driver);
        ASSERT_TRUE(out) << driver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliLlvm.score(5) + \"-\" + CliLlvm.compute(10) + \"-\"\n"
               "        + CliLlvm.divide(100, 7) + \"-\" + CliLlvm.remainder(100, 7) + \"-\"\n"
               "        + CliLlvm.factorial(10) + \"-\" + CliLlvm.f2i(1.5e10f) + \"-\"\n"
               "        + CliLlvm.f2i(Float.NaN) + \"-\");\n"
               "    System.out.print(CliLlvm.divide(-7, 2) + \"-\" + CliLlvm.remainder(-7, 2)\n"
               "        + \"-\" + CliLlvm.divide(Integer.MIN_VALUE, -1) + \"-\"\n"
               "        + CliLlvm.remainder(Integer.MIN_VALUE, -1) + \"-\");\n"
               "    try {\n"
               "      CliLlvm.divide(1, 0);\n"
               "      System.out.print(\"no-ex\");\n"
               "    } catch (ArithmeticException e) {\n"
               "      System.out.print(\"ae\");\n"
               "    }\n"
               "  }\n"
               "}\n";
    }

    auto result = runScript({q(javac) + " -d " + q(inputDir) + " " + q(source) + " " +
                             q(driver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(inputDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "17-83-14-2-3628800-2147483647-0--3--1--2147483648-0-ae");

    for (const char* llvmTool : {"clang", "llc"}) {
        const fs::path outputDir = workDir / llvmTool;
        const fs::path outputClass = outputDir / "CliLlvm.class";
        const fs::path llvmSource = outputDir / "native.ll";
        const fs::path nativeLibrary = nativeLibraryPathFor(outputClass);
        fs::create_directories(outputDir, ec);
        ASSERT_FALSE(ec) << ec.message();

        result = runScript({q(cli) + " -i " + q(inputDir / "CliLlvm.class") + " -o " +
                            q(outputClass) + " --backend llvm --emit-llvm " + q(llvmSource) +
                            " --compile --llvm-tool " + llvmTool});
        ASSERT_EQ(result.exitCode, 0) << result.out;
        ASSERT_TRUE(fileExists(llvmSource)) << result.out;
        ASSERT_TRUE(fileExists(nativeLibrary)) << result.out;
        const std::string llvm = readText(llvmSource);
        EXPECT_NE(llvm.find("br i1"), std::string::npos) << llvm;
        EXPECT_NE(llvm.find("switch i32"), std::string::npos) << llvm;
        EXPECT_NE(llvm.find("call i32 @mj2x_llvm_Java_CliLlvm_helper__II"),
                  std::string::npos)
            << llvm;
        EXPECT_NE(llvm.find("shl i32"), std::string::npos) << llvm;
        EXPECT_NE(llvm.find("sdiv i32"), std::string::npos) << llvm;
        EXPECT_NE(llvm.find("call void @mj2x_llvm_throw_arith(ptr %env)"), std::string::npos)
            << llvm;
        EXPECT_NE(llvm.find("call i32 @llvm.fptosi.sat.i32.f32"), std::string::npos) << llvm;
        EXPECT_NE(llvm.find("call i64 @mj2x_llvm_Java_CliLlvm_factorial__I"),
                  std::string::npos)
            << llvm;

        fs::copy_file(inputDir / "Driver.class", outputDir / "Driver.class",
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();
        std::vector<std::string> nativeRun;
#ifdef _WIN32
        nativeRun.push_back("set \"MORPHER_DLL=" + nativeLibrary.string() + "\"");
#else
        nativeRun.push_back("export MORPHER_DLL=" + q(nativeLibrary));
#endif
        nativeRun.push_back(q(java) + " -Xverify:all -cp " + q(outputDir) + " Driver");
        result = runScript(nativeRun);
        ASSERT_EQ(result.exitCode, 0) << result.out;
        EXPECT_EQ(result.out, "17-83-14-2-3628800-2147483647-0--3--1--2147483648-0-ae");
    }
}

#endif  // !MJ2X_DISABLE_LLVM_BACKEND
TEST(CliTest, MsvcCompilesRegcCppSource)
{
#ifndef _WIN32
    GTEST_SKIP() << "MSVC is only available on Windows";
#else
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_msvc_cpp.keep").parent_path() / "mj2x_cli_msvc_cpp";
    const fs::path outClass = workDir / "output.class";
    const fs::path source   = workDir / "native.cpp";
    const fs::path library  = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto result = runScript({q(cli) + " -i " + q(writeTinyClass()) + " -o " +
                                   q(outClass) + " --backend regc --emit-cpp " + q(source) +
                                   " --compile --c-compiler msvc"});
    if (result.exitCode != 0 &&
        (result.out.find("C compiler 'cl' not found") != std::string::npos ||
         result.out.find("vcvarsall.bat was not found") != std::string::npos)) {
        GTEST_SKIP() << "MSVC toolchain is not available";
    }
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(source)) << result.out;
    EXPECT_TRUE(fileExists(library)) << result.out;
#endif
}

TEST(CliTest, LlvmPipeCompileAndProjectGeneration)
{
#if defined(MJ2X_DISABLE_LLVM_BACKEND)
    GTEST_SKIP() << "LLVM backend is disabled";
#else
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("clang")) GTEST_SKIP() << "clang is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    const fs::path javac = findJavac();
    if (javac.empty()) GTEST_SKIP() << "javac is not available";
#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_llvm_pipe.keep").parent_path() / "mj2x_cli_llvm_pipe";
    const fs::path source = workDir / "CliLlvmPipe.java";
    const fs::path driver = workDir / "Driver.java";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream out(source);
        ASSERT_TRUE(out) << source.string();
        out << "public class CliLlvmPipe {\n"
               "  public static int add(int a, int b) { return a + b; }\n"
               "}\n";
    }
    {
        std::ofstream out(driver);
        ASSERT_TRUE(out) << driver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliLlvmPipe.add(40, 2));\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -d " + q(workDir) + " " + q(source) + " " + q(driver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    result = runScript({q(java) + " -Xverify:all -cp " + q(workDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    // 1) --compile-mode pipe：无 --emit-llvm，LLVM IR 经 clang stdin 编译。
    const fs::path pipeDir = workDir / "pipe";
    fs::create_directories(pipeDir, ec);
    const fs::path pipeClass = pipeDir / "CliLlvmPipe.class";
    result = runScript({q(cli) + " -i " + q(workDir / "CliLlvmPipe.class") + " -o " +
                        q(pipeClass) + " --backend llvm --compile --compile-mode pipe "
                        "--llvm-tool clang"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    const fs::path pipeLibrary = nativeLibraryPathFor(pipeClass);
    ASSERT_TRUE(fileExists(pipeLibrary)) << result.out;
    fs::copy_file(workDir / "Driver.class", pipeDir / "Driver.class",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::vector<std::string> pipeRun;
#ifdef _WIN32
    pipeRun.push_back("set \"MORPHER_DLL=" + pipeLibrary.string() + "\"");
#else
    pipeRun.push_back("export MORPHER_DLL=" + q(pipeLibrary));
#endif
    pipeRun.push_back(q(java) + " -Xverify:all -cp " + q(pipeDir) + " Driver");
    result = runScript(pipeRun);
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "42");

    // 2) --generate-cmake：工程含 native.ll + CMakeLists.txt；clang 直接编译运行。
    const fs::path cmakeDir = workDir / "cmake_proj";
    fs::create_directories(cmakeDir, ec);
    const fs::path cmakeClass = cmakeDir / "CliLlvmPipe.class";
    result = runScript({q(cli) + " -i " + q(workDir / "CliLlvmPipe.class") + " -o " +
                        q(cmakeClass) + " --backend llvm --emit-llvm " +
                        q(cmakeDir / "native.ll") + " --generate-cmake " + q(cmakeDir)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(cmakeDir / "native.ll")) << result.out;
    ASSERT_TRUE(fileExists(cmakeDir / "CMakeLists.txt")) << result.out;
    {
        const fs::path cmakeLibrary = nativeLibraryPathFor(cmakeClass);
        result = runScript({q("clang") + " -shared " + q(cmakeDir / "native.ll") + " -o " +
                            q(cmakeLibrary)});
        ASSERT_EQ(result.exitCode, 0) << result.out;
        fs::copy_file(workDir / "Driver.class", cmakeDir / "Driver.class",
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();
        std::vector<std::string> cmakeRun;
#ifdef _WIN32
        cmakeRun.push_back("set \"MORPHER_DLL=" + cmakeLibrary.string() + "\"");
#else
        cmakeRun.push_back("export MORPHER_DLL=" + q(cmakeLibrary));
#endif
        cmakeRun.push_back(q(java) + " -Xverify:all -cp " + q(cmakeDir) + " Driver");
        result = runScript(cmakeRun);
        ASSERT_EQ(result.exitCode, 0) << result.out;
        EXPECT_EQ(result.out, "42");
    }

    // 3) --generate-xmake：工程含 native.ll + xmake.lua。
    const fs::path xmakeDir = workDir / "xmake_proj";
    fs::create_directories(xmakeDir, ec);
    result = runScript({q(cli) + " -i " + q(workDir / "CliLlvmPipe.class") + " -o " +
                        q(xmakeDir / "CliLlvmPipe.class") + " --backend llvm --emit-llvm " +
                        q(xmakeDir / "native.ll") + " --generate-xmake " + q(xmakeDir)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(xmakeDir / "native.ll")) << result.out;
    EXPECT_TRUE(fileExists(xmakeDir / "xmake.lua")) << result.out;
    // LLVM 分发面:common+regc+llvmir 三挂载,llvmir 提供 mj2x_llvm_* 符号实现
    // (此前 llvmir 不随工程分发,生成的工程 mj2x_llvm_* 全部未解析)。
    EXPECT_TRUE(fileExists(xmakeDir / "src" / "mj2x-rt" / "llvmir" / "mj2x_llvmir_runtime.c"))
        << result.out;
    EXPECT_TRUE(fileExists(xmakeDir / "src" / "mj2x-rt" / "regc" / "mj2x_regc_intrinsic.c"))
        << result.out;
    EXPECT_FALSE(fileExists(xmakeDir / "src" / "mj2x-rt" / "stackc")) << result.out;
    {
        const std::string script = readText(xmakeDir / "xmake.lua");
        EXPECT_NE(script.find("\"llvmir\""), std::string::npos) << script;
    }
#endif
}

TEST(CliTest, LlvmArraycopyAndExceptionE2E)
{
#if defined(MJ2X_DISABLE_LLVM_BACKEND)
    GTEST_SKIP() << "LLVM backend is disabled";
#else
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("clang")) GTEST_SKIP() << "clang is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    const fs::path javac = findJavac();
    if (javac.empty()) GTEST_SKIP() << "javac is not available";
#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_llvm_e2e.keep").parent_path() / "mj2x_cli_llvm_e2e";
    const fs::path source = workDir / "CliLlvmE2E.java";
    const fs::path driver = workDir / "Driver.java";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream out(source);
        ASSERT_TRUE(out) << source.string();
        out << "public class CliLlvmE2E {\n"
               "  public static int copy() {\n"
               "    int[] src = {1, 2, 3, 4, 5};\n"
               "    int[] dst = new int[5];\n"
               "    System.arraycopy(src, 1, dst, 0, 3);\n"
               "    return dst[0] * 100 + dst[1] * 10 + dst[2];\n"
               "  }\n"
               "  public static int copyBad() {\n"
               "    int[] src = {1};\n"
               "    int[] dst = new int[1];\n"
               "    try { System.arraycopy(src, 5, dst, 0, 1); return -1; }\n"
               "    catch (ArrayIndexOutOfBoundsException e) { return 7; }\n"
               "  }\n"
               "  public static int tryCatch() {\n"
               "    try { return 100 / 0; }\n"
               "    catch (ArithmeticException e) { return 9; }\n"
               "  }\n"
               "  public static int locked(int n) {\n"
               "    synchronized (CliLlvmE2E.class) { return n * 2; }\n"
               "  }\n"
               "  public static String greet() { return \"hi\"; }\n"
               "}\n";
    }
    {
        std::ofstream out(driver);
        ASSERT_TRUE(out) << driver.string();
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliLlvmE2E.copy());\n"
               "    System.out.print(\"-\" + CliLlvmE2E.copyBad());\n"
               "    System.out.print(\"-\" + CliLlvmE2E.tryCatch());\n"
               "    System.out.print(\"-\" + CliLlvmE2E.locked(21));\n"
               "    System.out.print(\"-\" + CliLlvmE2E.greet());\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -d " + q(workDir) + " " + q(source) + " " + q(driver)});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    // 未加载原生库时走 JVM 解释执行，先建立基线。
    result = runScript({q(java) + " -Xverify:all -cp " + q(workDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "234-7-9-42-hi");

    // LLVM pipe 编译 + 原生执行。
    const fs::path pipeDir = workDir / "out";
    fs::create_directories(pipeDir, ec);
    const fs::path pipeClass = pipeDir / "CliLlvmE2E.class";
    result = runScript({q(cli) + " -i " + q(workDir / "CliLlvmE2E.class") + " -o " +
                        q(pipeClass) + " --backend llvm --compile --compile-mode pipe "
                        "--llvm-tool clang"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    const fs::path pipeLibrary = nativeLibraryPathFor(pipeClass);
    ASSERT_TRUE(fileExists(pipeLibrary)) << result.out;
    fs::copy_file(workDir / "Driver.class", pipeDir / "Driver.class",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::vector<std::string> pipeRun;
#ifdef _WIN32
    pipeRun.push_back("set \"MORPHER_DLL=" + pipeLibrary.string() + "\"");
#else
    pipeRun.push_back("export MORPHER_DLL=" + q(pipeLibrary));
#endif
    pipeRun.push_back(q(java) + " -Xverify:all -cp " + q(pipeDir) + " Driver");
    result = runScript(pipeRun);
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "234-7-9-42-hi");
#endif
}

/// @brief 程序化构造带自定义 bootstrap invokedynamic 的类（javac 无法产生该形态；
/// pipeline 级 InvokeDynamicPreprocessor 不展开 → indy:<idx> 运行时解析）。
///
/// 方法面：
/// - add(II)I：bootstrap 解析到的目标。
/// - greet(String)String：REF 返回目标（Y1：REF 结果透传）。
/// - bsm(Lookup,String,MethodType)CallSite：无常量参数 bootstrap。
/// - bsmArgs(Lookup,String,MethodType,String,I,MethodHandle)CallSite：带
///   String/int/MethodHandle 常量参数（exercise 常量物化三形态）。
/// - bsmRef(Lookup,String,MethodType)CallSite：解析 greet（REF 站点）。
/// - bsmFail(Lookup,String,MethodType)CallSite：恒抛 IllegalStateException
///   （Y2：bootstrap 失败异常不得被 NPE 掩盖）。
/// - run(II)I / runArgs(II)I：indy 站点（无 bsm 常量 / String·int·MethodHandle 常量）。
/// - runRef(String)String：REF 返回 indy 站点。
/// - runFail(II)I：失败 bootstrap indy 站点。
std::vector<ca::u8> buildCustomIndyClass(ca::str::Utf8StringArena& arena)
{
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    const auto invokestatic = [&arena](jbytecode::tree::MethodNode* m, const char* owner,
                                       const char* name, const char* desc) {
        m->visitMethodInsn(Opcodes::INVOKESTATIC, arena.intern(owner), arena.intern(name),
                           arena.intern(desc), false);
    };

    auto* add = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, arena.intern("add"),
                               arena.intern("(II)I"), ca::str::Utf8StringRef(), {});
    add->visitCode();
    add->visitVarInsn(Opcodes::ILOAD, 0);
    add->visitVarInsn(Opcodes::ILOAD, 1);
    add->visitInsn(Opcodes::IADD);
    add->visitInsn(Opcodes::IRETURN);
    add->visitMaxs(0, 0);
    add->visitEnd();

    const char* bsmDesc =
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;)Ljava/lang/invoke/CallSite;";
    auto* bsm = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, arena.intern("bsm"),
                               arena.intern(bsmDesc), ca::str::Utf8StringRef(), {});
    bsm->visitCode();
    bsm->visitTypeInsn(Opcodes::NEW, arena.intern("java/lang/invoke/ConstantCallSite"));
    bsm->visitInsn(Opcodes::DUP);
    invokestatic(bsm, "java/lang/invoke/MethodHandles", "lookup",
                 "()Ljava/lang/invoke/MethodHandles$Lookup;");
    bsm->visitLdcInsn(Type::getObjectType(arena.intern("CliLlvmIndy")));
    bsm->visitLdcInsn(std::string("add"));
    bsm->visitVarInsn(Opcodes::ALOAD, 2);   // 站点 MethodType（indy 机制传入）
    bsm->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                         arena.intern("java/lang/invoke/MethodHandles$Lookup"),
                         arena.intern("findStatic"),
                         arena.intern("(Ljava/lang/Class;Ljava/lang/String;"
                                      "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/"
                                      "MethodHandle;"),
                         false);
    bsm->visitMethodInsn(Opcodes::INVOKESPECIAL,
                         arena.intern("java/lang/invoke/ConstantCallSite"),
                         arena.intern("<init>"), arena.intern("(Ljava/lang/invoke/MethodHandle;)V"),
                         false);
    bsm->visitInsn(Opcodes::ARETURN);
    bsm->visitMaxs(0, 0);
    bsm->visitEnd();

    const char* bsmArgsDesc =
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;Ljava/lang/String;ILjava/lang/invoke/MethodHandle;)Ljava/lang/invoke/"
        "CallSite;";
    auto* bsmArgs = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   arena.intern("bsmArgs"), arena.intern(bsmArgsDesc),
                                   ca::str::Utf8StringRef(), {});
    bsmArgs->visitCode();
    bsmArgs->visitTypeInsn(Opcodes::NEW, arena.intern("java/lang/invoke/ConstantCallSite"));
    bsmArgs->visitInsn(Opcodes::DUP);
    invokestatic(bsmArgs, "java/lang/invoke/MethodHandles", "lookup",
                 "()Ljava/lang/invoke/MethodHandles$Lookup;");
    bsmArgs->visitLdcInsn(Type::getObjectType(arena.intern("CliLlvmIndy")));
    bsmArgs->visitLdcInsn(std::string("add"));
    bsmArgs->visitVarInsn(Opcodes::ALOAD, 2);   // 站点 MethodType
    bsmArgs->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             arena.intern("java/lang/invoke/MethodHandles$Lookup"),
                             arena.intern("findStatic"),
                             arena.intern("(Ljava/lang/Class;Ljava/lang/String;"
                                          "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/"
                                          "MethodHandle;"),
                             false);
    bsmArgs->visitMethodInsn(Opcodes::INVOKESPECIAL,
                             arena.intern("java/lang/invoke/ConstantCallSite"),
                             arena.intern("<init>"),
                             arena.intern("(Ljava/lang/invoke/MethodHandle;)V"),
                             false);
    bsmArgs->visitInsn(Opcodes::ARETURN);
    bsmArgs->visitMaxs(0, 0);
    bsmArgs->visitEnd();

    const auto makeIndyMethod = [&arena, &bsm, &bsmArgs](const char* methodName,
                                                         const char* siteName,
                                                         const std::vector<ca::core::Any>& args,
                                                         const Handle& bootstrap) {
        auto* run = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   arena.intern(methodName), arena.intern("(II)I"),
                                   ca::str::Utf8StringRef(), {});
        run->visitCode();
        run->visitVarInsn(Opcodes::ILOAD, 0);
        run->visitVarInsn(Opcodes::ILOAD, 1);
        run->visitInvokeDynamicInsn(arena.intern(siteName), arena.intern("(II)I"), bootstrap,
                                    args);
        run->visitInsn(Opcodes::IRETURN);
        run->visitMaxs(0, 0);
        run->visitEnd();
        return run;
    };

    // greet(String)String：REF 返回目标（"hi " + s）。
    auto* greet = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 arena.intern("greet"),
                                 arena.intern("(Ljava/lang/String;)Ljava/lang/String;"),
                                 ca::str::Utf8StringRef(), {});
    greet->visitCode();
    greet->visitLdcInsn(std::string("hi "));
    greet->visitVarInsn(Opcodes::ALOAD, 0);
    greet->visitMethodInsn(Opcodes::INVOKEVIRTUAL, arena.intern("java/lang/String"),
                           arena.intern("concat"),
                           arena.intern("(Ljava/lang/String;)Ljava/lang/String;"), false);
    greet->visitInsn(Opcodes::ARETURN);
    greet->visitMaxs(0, 0);
    greet->visitEnd();

    // bsmRef：解析 greet（站点 MethodType 即 (String)String）。结构与 bsm 同构。
    auto* bsmRef = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                  arena.intern("bsmRef"), arena.intern(bsmDesc),
                                  ca::str::Utf8StringRef(), {});
    bsmRef->visitCode();
    bsmRef->visitTypeInsn(Opcodes::NEW, arena.intern("java/lang/invoke/ConstantCallSite"));
    bsmRef->visitInsn(Opcodes::DUP);
    invokestatic(bsmRef, "java/lang/invoke/MethodHandles", "lookup",
                 "()Ljava/lang/invoke/MethodHandles$Lookup;");
    bsmRef->visitLdcInsn(Type::getObjectType(arena.intern("CliLlvmIndy")));
    bsmRef->visitLdcInsn(std::string("greet"));
    bsmRef->visitVarInsn(Opcodes::ALOAD, 2);
    bsmRef->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                            arena.intern("java/lang/invoke/MethodHandles$Lookup"),
                            arena.intern("findStatic"),
                            arena.intern("(Ljava/lang/Class;Ljava/lang/String;"
                                         "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/"
                                         "MethodHandle;"),
                            false);
    bsmRef->visitMethodInsn(Opcodes::INVOKESPECIAL,
                            arena.intern("java/lang/invoke/ConstantCallSite"),
                            arena.intern("<init>"),
                            arena.intern("(Ljava/lang/invoke/MethodHandle;)V"), false);
    bsmRef->visitInsn(Opcodes::ARETURN);
    bsmRef->visitMaxs(0, 0);
    bsmRef->visitEnd();

    // bsmFail：恒抛 IllegalStateException（bootstrap 失败路径）。
    auto* bsmFail = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   arena.intern("bsmFail"), arena.intern(bsmDesc),
                                   ca::str::Utf8StringRef(), {});
    bsmFail->visitCode();
    bsmFail->visitTypeInsn(Opcodes::NEW, arena.intern("java/lang/IllegalStateException"));
    bsmFail->visitInsn(Opcodes::DUP);
    bsmFail->visitLdcInsn(std::string("boom"));
    bsmFail->visitMethodInsn(Opcodes::INVOKESPECIAL,
                             arena.intern("java/lang/IllegalStateException"),
                             arena.intern("<init>"), arena.intern("(Ljava/lang/String;)V"),
                             false);
    bsmFail->visitInsn(Opcodes::ATHROW);
    bsmFail->visitMaxs(0, 0);
    bsmFail->visitEnd();

    // runRef(String)String：REF 返回 indy 站点。
    auto* runRef = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                  arena.intern("runRef"),
                                  arena.intern("(Ljava/lang/String;)Ljava/lang/String;"),
                                  ca::str::Utf8StringRef(), {});
    runRef->visitCode();
    runRef->visitVarInsn(Opcodes::ALOAD, 0);
    runRef->visitInvokeDynamicInsn(
        arena.intern("runRef"),
        arena.intern("(Ljava/lang/String;)Ljava/lang/String;"),
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("CliLlvmIndy"), arena.intern("bsmRef"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                            "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;")),
        {});
    runRef->visitInsn(Opcodes::ARETURN);
    runRef->visitMaxs(0, 0);
    runRef->visitEnd();

    // runFail(II)I：失败 bootstrap indy 站点。
    auto* runFail = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   arena.intern("runFail"), arena.intern("(II)I"),
                                   ca::str::Utf8StringRef(), {});
    runFail->visitCode();
    runFail->visitVarInsn(Opcodes::ILOAD, 0);
    runFail->visitVarInsn(Opcodes::ILOAD, 1);
    runFail->visitInvokeDynamicInsn(
        arena.intern("runFail"), arena.intern("(II)I"),
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("CliLlvmIndy"), arena.intern("bsmFail"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                            "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;")),
        {});
    runFail->visitInsn(Opcodes::IRETURN);
    runFail->visitMaxs(0, 0);
    runFail->visitEnd();
    const Handle bsmHandle(
        Opcodes::H_INVOKESTATIC, arena.intern("CliLlvmIndy"), arena.intern("bsm"),
        arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                     "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;"));
    const Handle bsmArgsHandle(
        Opcodes::H_INVOKESTATIC, arena.intern("CliLlvmIndy"), arena.intern("bsmArgs"),
        arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                     "Ljava/lang/invoke/MethodType;Ljava/lang/String;ILjava/lang/invoke/"
                     "MethodHandle;)Ljava/lang/invoke/CallSite;"));
    const Handle addHandle(Opcodes::H_INVOKESTATIC, arena.intern("CliLlvmIndy"),
                           arena.intern("add"), arena.intern("(II)I"));
    auto* run = makeIndyMethod("run", "run", {}, bsmHandle);
    std::vector<ca::core::Any> argsWithConstants;
    argsWithConstants.emplace_back(arena.intern("tag"));
    argsWithConstants.emplace_back(7);
    argsWithConstants.emplace_back(addHandle);
    auto* runArgs = makeIndyMethod("runArgs", "runArgs", argsWithConstants, bsmArgsHandle);

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = arena.intern("CliLlvmIndy");
    cn.superName = arena.intern("java/lang/Object");
    cn.methods.push_back(add);
    cn.methods.push_back(greet);
    cn.methods.push_back(bsm);
    cn.methods.push_back(bsmArgs);
    cn.methods.push_back(bsmRef);
    cn.methods.push_back(bsmFail);
    cn.methods.push_back(run);
    cn.methods.push_back(runArgs);
    cn.methods.push_back(runRef);
    cn.methods.push_back(runFail);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

TEST(CliTest, LlvmIndyCustomBootstrapE2E)
{
#if defined(MJ2X_DISABLE_LLVM_BACKEND)
    GTEST_SKIP() << "LLVM backend is disabled";
#else
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("clang")) GTEST_SKIP() << "clang is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    const fs::path javac = findJavac();
    if (javac.empty()) GTEST_SKIP() << "javac is not available";
#ifdef _WIN32
    const fs::path java = javac.parent_path() / "java.exe";
#else
    const fs::path java = javac.parent_path() / "java";
#endif

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_llvm_indy.keep").parent_path() / "mj2x_cli_llvm_indy";
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    // 1. 程序化构造 CliLlvmIndy.class（自定义 bootstrap invokedynamic）。
    ca::str::Utf8StringArena arena;
    const auto               classBytes = buildCustomIndyClass(arena);
    {
        std::ofstream out(workDir / "CliLlvmIndy.class", std::ios::binary);
        ASSERT_TRUE(out) << (workDir / "CliLlvmIndy.class").string();
        out.write(reinterpret_cast<const char*>(classBytes.data()),
                  static_cast<std::streamsize>(classBytes.size()));
    }
    {
        std::ofstream out(workDir / "Driver.java");
        ASSERT_TRUE(out) << (workDir / "Driver.java").string();
        // runRef：REF 返回 indy 站点（Y1）。runFail：失败 bootstrap（Y2）——
        // JVM 基线把 bsm 异常包成 BootstrapMethodError(cause)，native 侧重抛
        // 原始异常；解包归一化后两边都应看到 IllegalStateException（而非被
        // call_object(null) 的 NPE 掩盖）。
        out << "public class Driver {\n"
               "  public static void main(String[] args) {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.print(CliLlvmIndy.run(3, 4));\n"
               "    System.out.print(\"-\" + CliLlvmIndy.runArgs(10, 32));\n"
               "    System.out.print(\"-\" + CliLlvmIndy.add(5, 6));\n"
               "    System.out.print(\"-\" + CliLlvmIndy.runRef(\"vm\"));\n"
               "    System.out.print(\"-\" + failKind());\n"
               "  }\n"
               "  static String failKind() {\n"
               "    try {\n"
               "      CliLlvmIndy.runFail(1, 2);\n"
               "      return \"none\";\n"
               "    } catch (Throwable t) {\n"
               "      while (t instanceof java.lang.BootstrapMethodError && t.getCause() != "
               "null) {\n"
               "        t = t.getCause();\n"
               "      }\n"
               "      return t.getClass().getName();\n"
               "    }\n"
               "  }\n"
               "}\n";
    }
    auto result = runScript({q(javac) + " -cp " + q(workDir) + " -d " + q(workDir) + " " +
                             q(workDir / "Driver.java")});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    // JVM 基线（未加载原生库）。
    result = runScript({q(java) + " -Xverify:all -cp " + q(workDir) + " Driver"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_EQ(result.out, "7-42-11-hi vm-java.lang.IllegalStateException");

    // 2. LLVM 原生执行：indy 运行时解析（自定义 bootstrap）+ 原生 add 经
    //    MethodHandle 调用。
    const auto runNativeVariant = [&](const std::string& suffix,
                                      const std::string& extraArgs) {
        const fs::path outDir = workDir / suffix;
        fs::create_directories(outDir, ec);
        const fs::path outClass = outDir / "CliLlvmIndy.class";
        const auto     rc = runScript({q(cli) + " -i " + q(workDir / "CliLlvmIndy.class") +
                                       " -o " + q(outClass) + " --backend llvm --compile "
                                       "--compile-mode pipe --llvm-tool clang " + extraArgs});
        ASSERT_EQ(rc.exitCode, 0) << rc.out;
        const fs::path library = nativeLibraryPathFor(outClass);
        ASSERT_TRUE(fileExists(library)) << rc.out;
        fs::copy_file(workDir / "Driver.class", outDir / "Driver.class",
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();
        std::vector<std::string> runLines;
#ifdef _WIN32
        runLines.push_back("set \"MORPHER_DLL=" + library.string() + "\"");
#else
        runLines.push_back("export MORPHER_DLL=" + q(library));
#endif
        runLines.push_back(q(java) + " -Xverify:all -cp " + q(outDir) + " Driver");
        const auto runResult = runScript(runLines);
        ASSERT_EQ(runResult.exitCode, 0) << runResult.out;
        EXPECT_EQ(runResult.out, "7-42-11-hi vm-java.lang.IllegalStateException");
    };

    runNativeVariant("plain", "");
    // --runtime-protection：检查点注入 + mj2x-rt rt_protect 源随 .ll 一起编译；
    // step 放大避免概率触发（无调试器信号时 ~1/step 触发率）。
    runNativeVariant("protected",
                     "--runtime-protection fake --runtime-protection-step 1000000");
    // --regc-obfuscate：R4 变换（MBA 等）作用于 RegIR 后发射。
    runNativeVariant("obfuscated", "--regc-obfuscate");
#endif
}

TEST(CliTest, ClangCompilesRegcCppSource)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("clang++")) GTEST_SKIP() << "clang++ is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_clang_cpp.keep").parent_path() / "mj2x_cli_clang_cpp";
    const fs::path outClass = workDir / "output.class";
    const fs::path source   = workDir / "native.cpp";
    const fs::path library  = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto result = runScript({q(cli) + " -i " + q(writeTinyClass()) + " -o " +
                                   q(outClass) + " --backend regc --emit-cpp " + q(source) +
                                   " --compile --c-compiler clang"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(source)) << result.out;
    EXPECT_TRUE(fileExists(library)) << result.out;
}

TEST(CliTest, XmakeCompilesRegcCppProject)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    if (!commandAvailable("xmake")) GTEST_SKIP() << "xmake is not available on PATH";
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }

    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_xmake_cpp_compile.keep").parent_path() /
        "mj2x_cli_xmake_cpp_compile";
    const fs::path outClass   = workDir / "output.class";
    const fs::path sourcePath = workDir / "native.cpp";
    const fs::path projectDir = workDir / "project";
    const fs::path library    = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto result = runScript({q(cli) + " -i " + q(writeTinyClass()) + " -o " +
                                   q(outClass) + " --backend regc --emit-cpp " + q(sourcePath) +
                                   " --generate-xmake " + q(projectDir) + " --compile"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "src" / "generated" / "native.cpp")) << result.out;
    EXPECT_TRUE(fileExists(library)) << result.out;
}

TEST(CliTest, PipeCompileWithMsvcRemovesTemporaryCSource)
{
#ifndef _WIN32
    GTEST_SKIP() << "MSVC is only available on Windows";
#else
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }

    const fs::path inClass = writeTinyClass();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_pipe_msvc.keep").parent_path() /
        "mj2x_cli_pipe_msvc";
    const fs::path outClass      = workDir / "output.class";
    const fs::path nativeLibrary = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
            " --compile --compile-mode pipe --c-compiler msvc",
    });
    if (result.exitCode != 0 &&
        (result.out.find("C compiler 'cl' not found") != std::string::npos ||
         result.out.find("vcvarsall.bat was not found") != std::string::npos)) {
        GTEST_SKIP() << "MSVC toolchain is not available";
    }

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(outClass)) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    for (const auto& entry : fs::recursive_directory_iterator(workDir)) {
        EXPECT_NE(entry.path().extension(), ".c") << entry.path().string();
    }
#endif
}

TEST(CliTest, CompileJarEmbedsNativeLoaderAndLibraryWhenToolchainIsAvailable)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!hasAutoNativeCompiler()) {
        GTEST_SKIP() << "no supported C compiler is available on PATH";
    }

    fs::path        inJar         = writeTinyJar();
    fs::path        outJar        = test::getTempOutPath("mj2x_cli_compile_output.jar");
    fs::path        cFile         = test::getTempOutPath("mj2x_cli_compile_output.c");
    fs::path        nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) + " --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;

    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;
    auto cloaderClass = readJarEntry(outJar, "mj2x/cloader/Loader.class");
    EXPECT_FALSE(cloaderClass.empty()) << result.out;
    auto nativeResource =
        readJarEntry(outJar, "META-INF/mj2x/native/" + nativeLibrary.filename().string());
    EXPECT_FALSE(nativeResource.empty()) << result.out;

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn     = readClassFromBytes(classBytes, arena);
    auto*                    clinit = findMethod(*cn, "<clinit>");
    ASSERT_NE(clinit, nullptr);
    bool callsLoader = false;
    for (auto* insn : clinit->instructions.toArray()) {
        if (insn->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN) continue;
        auto* call  = static_cast<jbytecode::tree::MethodInsnNode*>(insn);
        callsLoader = callsLoader || (call->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
                                      call->name.to_std_string() == "load");
    }
    // native 库加载已收敛到 Loader.<clinit>（转换类 clinit 只 registerNativesForClass）。
    EXPECT_FALSE(callsLoader);
    auto                     loaderBytes = readJarEntry(outJar, "mj2x/cloader/Loader.class");
    ca::str::Utf8StringArena loaderArena;
    auto                     loaderCn = readClassFromBytes(loaderBytes, loaderArena);
    auto*                    loaderInit = findMethod(*loaderCn, "<clinit>");
    ASSERT_NE(loaderInit, nullptr);
    bool loaderCallsLoad = false;
    for (auto* insn : loaderInit->instructions.toArray()) {
        if (insn->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN) continue;
        auto* call           = static_cast<jbytecode::tree::MethodInsnNode*>(insn);
        loaderCallsLoad = loaderCallsLoad ||
                          (call->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
                           call->name.to_std_string() == "load");
    }
    EXPECT_TRUE(loaderCallsLoad);
}

TEST(CliTest, CompileRegCJarEmbedsNativeLoaderAndLibraryWhenToolchainIsAvailable)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!hasAutoNativeCompiler()) {
        GTEST_SKIP() << "no supported C compiler is available on PATH";
    }

    fs::path        inJar         = writeTinyJar();
    fs::path        outJar        = test::getTempOutPath("mj2x_cli_compile_regc_output.jar");
    fs::path        cFile         = test::getTempOutPath("mj2x_cli_compile_regc_output.c");
    fs::path        nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) +
            " --backend regc --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;

    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;
    auto nativeResource =
        readJarEntry(outJar, "META-INF/mj2x/native/" + nativeLibrary.filename().string());
    EXPECT_FALSE(nativeResource.empty()) << result.out;

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn     = readClassFromBytes(classBytes, arena);
    auto*                    add    = findMethod(*cn, "add");
    auto*                    clinit = findMethod(*cn, "<clinit>");
    ASSERT_NE(add, nullptr);
    ASSERT_NE(clinit, nullptr);
    EXPECT_TRUE((add->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    bool callsLoader = false;
    for (auto* insn : clinit->instructions.toArray()) {
        if (insn->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN) continue;
        auto* call  = static_cast<jbytecode::tree::MethodInsnNode*>(insn);
        callsLoader = callsLoader || (call->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
                                      call->name.to_std_string() == "load");
    }
    EXPECT_TRUE(callsLoader);
}

// VMC4 复用 RegisterNatives + mj2x/cloader/Loader 桥接：jar 模式必须注入
// Loader.class（修复前缺失 → 运行期 NoClassDefFoundError: mj2x/cloader/Loader）。
TEST(CliTest, CompileVmc4JarEmbedsNativeLoaderAndLibraryWhenToolchainIsAvailable)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!hasAutoNativeCompiler()) {
        GTEST_SKIP() << "no supported C compiler is available on PATH";
    }

    fs::path        inJar         = writeTinyJar();
    fs::path        outJar        = test::getTempOutPath("mj2x_cli_compile_vmc4_output.jar");
    fs::path        cFile         = test::getTempOutPath("mj2x_cli_compile_vmc4_output.c");
    fs::path        nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) +
            " --backend vmc4 --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;

    // 桥接类 + 运行时 loader + native 资源都在 jar 内。
    auto cloaderClass = readJarEntry(outJar, "mj2x/cloader/Loader.class");
    EXPECT_FALSE(cloaderClass.empty()) << result.out;
    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;
    auto nativeResource =
        readJarEntry(outJar, "META-INF/mj2x/native/" + nativeLibrary.filename().string());
    EXPECT_FALSE(nativeResource.empty()) << result.out;

    // 方法被标 native（转 VM）。
    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn  = readClassFromBytes(classBytes, arena);
    auto*                    add = findMethod(*cn, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE((add->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
}

TEST(CliTest, PipeCompileRegCJarEmbedsNativeLoaderAndLibraryWithClang)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }
    if (!commandAvailable("clang")) {
        GTEST_SKIP() << "clang is not available on PATH";
    }

    const fs::path inJar = writeTinyJar();
    const fs::path workDir =
        test::getTempOutPath("mj2x_cli_pipe_regc.keep").parent_path() /
        "mj2x_cli_pipe_regc";
    const fs::path outJar        = workDir / "output.jar";
    const fs::path nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) +
            " --backend regc --compile --compile-mode pipe --c-compiler clang",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
    for (const auto& entry : fs::recursive_directory_iterator(workDir)) {
        EXPECT_NE(entry.path().extension(), ".c") << entry.path().string();
    }

    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;
    auto nativeResource =
        readJarEntry(outJar, "META-INF/mj2x/native/" + nativeLibrary.filename().string());
    EXPECT_FALSE(nativeResource.empty()) << result.out;

    auto                     classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto                     cn  = readClassFromBytes(classBytes, arena);
    auto*                    add = findMethod(*cn, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE((add->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
}

TEST(CliTest, CompileRegCWithMsvcSupportsOutputDirectoryContainingSpaces)
{
#ifndef _WIN32
    GTEST_SKIP() << "MSVC is only available on Windows";
#else
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    if (std::getenv("JAVA_HOME") == nullptr || std::getenv("JAVA_HOME")[0] == '\0') {
        GTEST_SKIP() << "JAVA_HOME is not set";
    }

    std::error_code ec;
    fs::path        workDir =
        test::getTempOutPath("mj2x_cli_msvc_space.keep").parent_path() / "mj2x cli msvc space";
    fs::remove_all(workDir, ec);
    ec.clear();
    fs::create_directories(workDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::path inJar         = writeTinyJar();
    fs::path outJar        = workDir / "output archive.jar";
    fs::path cFile         = workDir / "generated source.c";
    fs::path nativeLibrary = nativeLibraryPathFor(outJar);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) +
            " --backend regc --compile --c-compiler msvc",
    });
    if (result.exitCode != 0 &&
        (result.out.find("C compiler 'cl' not found") != std::string::npos ||
         result.out.find("vcvarsall.bat was not found") != std::string::npos)) {
        GTEST_SKIP() << "MSVC toolchain is not available";
    }

    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
#endif
}

/// 每语言一组 CLI 负路径期望文案(issue #888:文案断言参数化语言)。
struct CliLangCase
{
    std::string langArgs;   // 附加到每条命令;空 = 默认 zh_CN
    std::string usage;
    std::string invalidJsonCfg;
    std::string unknownCfgKey;
    std::string invalidCfgBackend;
    std::string unknownBackend;
    std::string backendNeedsOutput;
    std::string compileNeedsOutput;
    std::string annotationMust;
    std::string mutex;
    std::string unknownCompiler;
    std::string unknownCompileMode;
    std::string cppNeedsRegc;
    std::string cppNeedsCppExt;
    std::string unknownRtProtection;
    std::string rtProtectionNeedsNative;
    std::string llvmNeedsBackend;
    std::string backendLlvmNeedsEmit;
    std::string llExtRequired;
    std::string pipeNoEmit;
    std::string llvmNoCCompiler;
    std::string unknownLlvmTool;
    std::string compileModeNeedsCompile;
    std::string xmakeNoPipe;
    std::string directCompiler;
};

class CliLangTest : public ::testing::TestWithParam<CliLangCase>
{
};

TEST_P(CliLangTest, NegativeArgumentsFail)
{
    const CliLangCase tc = GetParam();
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }
    // 每条命令附加语言参数(空串 = 默认 zh_CN)。
    const std::string cliCmd = q(cli) + (tc.langArgs.empty() ? "" : " " + tc.langArgs);

    fs::path inClass  = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_negative_output.class");
    fs::path cFile    = test::getTempOutPath("mj2x_cli_negative_output.c");
    fs::path llvmFile = test::getTempOutPath("mj2x_cli_negative_output.ll");

    auto noArgs = runScript({cliCmd});
    EXPECT_NE(noArgs.exitCode, 0);
    EXPECT_NE(noArgs.out.find(tc.usage), std::string::npos) << noArgs.out;

    fs::path invalidJson = test::getTempOutPath("mj2x_cli_invalid_config.json");
    writeText(invalidJson, "{ nope");
    auto badJsonConfig = runScript({
        cliCmd + " --config " + q(invalidJson),
    });
    EXPECT_NE(badJsonConfig.exitCode, 0);
    EXPECT_NE(badJsonConfig.out.find(tc.invalidJsonCfg), std::string::npos)
        << badJsonConfig.out;

    fs::path unknownKeyJson = test::getTempOutPath("mj2x_cli_unknown_key_config.json");
    writeText(unknownKeyJson, "{\"input\":\"x.class\",\"unknown\":true}");
    auto unknownConfigKey = runScript({
        cliCmd + " --config " + q(unknownKeyJson),
    });
    EXPECT_NE(unknownConfigKey.exitCode, 0);
    EXPECT_NE(unknownConfigKey.out.find(tc.unknownCfgKey), std::string::npos)
        << unknownConfigKey.out;

    fs::path badBackendJson = test::getTempOutPath("mj2x_cli_bad_backend_config.json");
    writeText(badBackendJson, "{\"backend\":\"nope\"}");
    auto badBackendConfig = runScript({
        cliCmd + " --config " + q(badBackendJson),
    });
    EXPECT_NE(badBackendConfig.exitCode, 0);
    EXPECT_NE(badBackendConfig.out.find(tc.invalidCfgBackend), std::string::npos)
        << badBackendConfig.out;

    auto unknownBackend = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --backend nope",
    });
    EXPECT_NE(unknownBackend.exitCode, 0);
    EXPECT_NE(unknownBackend.out.find(tc.unknownBackend), std::string::npos)
        << unknownBackend.out;

    auto backendWithoutEmit = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --backend regc",
    });
    EXPECT_NE(backendWithoutEmit.exitCode, 0);
    EXPECT_NE(backendWithoutEmit.out.find(tc.backendNeedsOutput), std::string::npos)
        << backendWithoutEmit.out;

    auto compileWithoutEmit = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --compile",
    });
    EXPECT_NE(compileWithoutEmit.exitCode, 0);
    EXPECT_NE(compileWithoutEmit.out.find(tc.compileNeedsOutput), std::string::npos)
        << compileWithoutEmit.out;

    auto badAnnotationDescriptor = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-annotation " + quoteArg("mj2x.Native"),
    });
    EXPECT_NE(badAnnotationDescriptor.exitCode, 0);
    EXPECT_NE(badAnnotationDescriptor.out.find(tc.annotationMust), std::string::npos)
        << badAnnotationDescriptor.out;

    auto mutuallyExclusiveSelectors = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --native-annotation " + quoteArg("Lmj2x/Native;") + " --native-method-prefix " +
            quoteArg("CliRegCTest.add"),
    });
    EXPECT_NE(mutuallyExclusiveSelectors.exitCode, 0);
    EXPECT_NE(mutuallyExclusiveSelectors.out.find(tc.mutex), std::string::npos)
        << mutuallyExclusiveSelectors.out;

    fs::path mutuallyExclusiveJson =
        test::getTempOutPath("mj2x_cli_mutually_exclusive_config.json");
    writeText(mutuallyExclusiveJson,
              "{"
              "\"input\":\"" +
                  inClass.generic_string() +
                  "\","
                  "\"output\":\"" +
                  outClass.generic_string() +
                  "\","
                  "\"emit_c\":\"" +
                  cFile.generic_string() +
                  "\","
                  "\"native_annotation\":\"Lmj2x/Native;\","
                  "\"native_method_prefixes\":[\"CliRegCTest.add\"]"
                  "}");
    auto mutuallyExclusiveConfig = runScript({
        cliCmd + " --config " + q(mutuallyExclusiveJson),
    });
    EXPECT_NE(mutuallyExclusiveConfig.exitCode, 0);
    EXPECT_NE(mutuallyExclusiveConfig.out.find(tc.mutex), std::string::npos)
        << mutuallyExclusiveConfig.out;

    auto badCompiler = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --compile --c-compiler nope",
    });
    EXPECT_NE(badCompiler.exitCode, 0);
    EXPECT_NE(badCompiler.out.find(tc.unknownCompiler), std::string::npos)
        << badCompiler.out;

    auto badCompileMode = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --compile --compile-mode nope",
    });
    EXPECT_NE(badCompileMode.exitCode, 0);
    EXPECT_NE(badCompileMode.out.find(tc.unknownCompileMode), std::string::npos)
        << badCompileMode.out;

    auto cppWithoutRegc = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-cpp " +
            q(test::getTempOutPath("mj2x_cli_negative_output.cpp")),
    });
    EXPECT_NE(cppWithoutRegc.exitCode, 0);
    EXPECT_NE(cppWithoutRegc.out.find(tc.cppNeedsRegc), std::string::npos)
        << cppWithoutRegc.out;

    auto cppWithWrongExtension = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --backend regc --emit-cpp " +
            q(test::getTempOutPath("mj2x_cli_negative_output.c")),
    });
    EXPECT_NE(cppWithWrongExtension.exitCode, 0);
    EXPECT_NE(cppWithWrongExtension.out.find(tc.cppNeedsCppExt), std::string::npos)
        << cppWithWrongExtension.out;

    auto badRuntimeProtection = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --backend regc --emit-c " +
            q(test::getTempOutPath("mj2x_cli_negative_output.c")) +
            " --runtime-protection nope",
    });
    EXPECT_NE(badRuntimeProtection.exitCode, 0);
    EXPECT_NE(badRuntimeProtection.out.find(tc.unknownRtProtection),
              std::string::npos)
        << badRuntimeProtection.out;

    auto rtProtectionWrongBackend = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(test::getTempOutPath("mj2x_cli_negative_output.c")) +
            " --runtime-protection fake",
    });
    EXPECT_NE(rtProtectionWrongBackend.exitCode, 0);
    EXPECT_NE(rtProtectionWrongBackend.out.find(tc.rtProtectionNeedsNative),
              std::string::npos)
        << rtProtectionWrongBackend.out;

#ifndef MJ2X_DISABLE_LLVM_BACKEND
    auto llvmWithoutBackend = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-llvm " + q(llvmFile),
    });
    EXPECT_NE(llvmWithoutBackend.exitCode, 0);
    EXPECT_NE(llvmWithoutBackend.out.find(tc.llvmNeedsBackend), std::string::npos)
        << llvmWithoutBackend.out;

    auto llvmWithoutEmit = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --backend llvm",
    });
    EXPECT_NE(llvmWithoutEmit.exitCode, 0);
    EXPECT_NE(llvmWithoutEmit.out.find(tc.backendLlvmNeedsEmit), std::string::npos)
        << llvmWithoutEmit.out;

    auto llvmWrongExtension = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --backend llvm --emit-llvm " + q(cFile),
    });
    EXPECT_NE(llvmWrongExtension.exitCode, 0);
    EXPECT_NE(llvmWrongExtension.out.find(tc.llExtRequired), std::string::npos)
        << llvmWrongExtension.out;

    auto llvmPipeWithEmit = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --backend llvm --compile --compile-mode pipe --emit-llvm " + q(llvmFile),
    });
    EXPECT_NE(llvmPipeWithEmit.exitCode, 0);
    EXPECT_NE(llvmPipeWithEmit.out.find(tc.pipeNoEmit), std::string::npos)
        << llvmPipeWithEmit.out;

    auto llvmCCompiler = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --backend llvm --emit-llvm " + q(llvmFile) + " --compile --c-compiler clang",
    });
    EXPECT_NE(llvmCCompiler.exitCode, 0);
    EXPECT_NE(llvmCCompiler.out.find(tc.llvmNoCCompiler), std::string::npos)
        << llvmCCompiler.out;

    auto badLlvmTool = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --llvm-tool nope",
    });
    EXPECT_NE(badLlvmTool.exitCode, 0);
    EXPECT_NE(badLlvmTool.out.find(tc.unknownLlvmTool), std::string::npos)
        << badLlvmTool.out;

    auto llvmProject = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --backend llvm --emit-llvm " + q(llvmFile) + " --generate-cmake " +
            q(test::getTempOutPath("mj2x_cli_negative_llvm_project")) + " --generate-xmake " +
            q(test::getTempOutPath("mj2x_cli_negative_llvm_project_x")),
    });
    EXPECT_NE(llvmProject.exitCode, 0);
    EXPECT_NE(llvmProject.out.find(tc.mutex), std::string::npos) << llvmProject.out;
#else
    // v2.1 LLVM 后端关闭：--backend llvm / --emit-llvm / --llvm-tool 均被 CLI 拒绝。
    auto llvmDisabled = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --backend llvm",
    });
    EXPECT_NE(llvmDisabled.exitCode, 0);
    EXPECT_NE(llvmDisabled.out.find(tc.unknownBackend), std::string::npos)
        << llvmDisabled.out;
#endif

    auto compileModeWithoutCompile = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --compile-mode pipe",
    });
    EXPECT_NE(compileModeWithoutCompile.exitCode, 0);
    EXPECT_NE(compileModeWithoutCompile.out.find(tc.compileModeNeedsCompile),
              std::string::npos)
        << compileModeWithoutCompile.out;

    auto pipeWithEmit = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) +
            " --compile --compile-mode pipe",
    });
    EXPECT_NE(pipeWithEmit.exitCode, 0);
    EXPECT_NE(pipeWithEmit.out.find(tc.pipeNoEmit), std::string::npos)
        << pipeWithEmit.out;

    auto xmakeWithPipe = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) +
            " --compile --compile-mode pipe --c-compiler xmake",
    });
    EXPECT_NE(xmakeWithPipe.exitCode, 0);
    EXPECT_NE(xmakeWithPipe.out.find(tc.xmakeNoPipe),
              std::string::npos)
        << xmakeWithPipe.out;

    const fs::path conflictingXmakeProject =
        test::getTempOutPath("mj2x_cli_conflicting_xmake_project");
    auto directCompilerWithXmake = runScript({
        cliCmd + " -i " + q(inClass) + " -o " + q(outClass) + " --generate-xmake " +
            q(conflictingXmakeProject) + " --compile --c-compiler clang",
    });
    EXPECT_NE(directCompilerWithXmake.exitCode, 0);
    EXPECT_NE(directCompilerWithXmake.out.find(tc.directCompiler), std::string::npos)
        << directCompilerWithXmake.out;

    auto missingInput = runScript({
        cliCmd + " -i " + q(test::getTempOutPath("mj2x_cli_missing_input.class")) + " -o " +
            q(outClass) + " --emit-c " + q(cFile),
    });
    EXPECT_NE(missingInput.exitCode, 0);
    EXPECT_NE(missingInput.out.find("load failed"), std::string::npos) << missingInput.out;
}

INSTANTIATE_TEST_SUITE_P(
    CliLangs, CliLangTest,
    ::testing::Values(
        CliLangCase{
            "",
            "用法:",
            "无效 JSON 配置",
            "未知配置键",
            "配置键 backend 的取值无效",
            "--backend 的取值未知",
            "--backend 需要 --emit-c",
            "--compile 需要 --emit-c",
            "--native-annotation 必须是",
            "互斥",
            "--c-compiler 的取值未知",
            "--compile-mode 的取值未知",
            "需要 --backend regc",
            "需要 .cpp 输出路径",
            "--runtime-protection 的取值未知",
            "需要 --backend regc、vmc4 或 llvm",
            "需要 --backend llvm",
            "--backend llvm 需要 --emit-llvm",
            "需要 .ll 输出路径",
            "不能与 --emit-c",
            "不能与 --c-compiler 组合",
            "--llvm-tool 的取值未知",
            "--compile-mode 需要 --compile",
            "不支持 --compile-mode pipe",
            "直接 --c-compiler",
        },
        CliLangCase{
            "--lang en_US",
            "Usage:",
            "invalid JSON config",
            "unknown config key",
            "invalid value for config key backend",
            "Unknown --backend value",
            "--backend requires --emit-c",
            "--compile requires --emit-c",
            "--native-annotation must be",
            "mutually exclusive",
            "Unknown --c-compiler value",
            "Unknown --compile-mode value",
            "requires --backend regc",
            "requires a .cpp output path",
            "Unknown value for --runtime-protection",
            "requires --backend regc, vmc4, or llvm",
            "requires --backend llvm",
            "--backend llvm requires --emit-llvm",
            "requires a .ll output path",
            "cannot be combined with --emit-c",
            "cannot be combined with --c-compiler",
            "Unknown --llvm-tool value",
            "--compile-mode requires --compile",
            "does not support --compile-mode pipe",
            "direct --c-compiler",
        }));

TEST(CliTest, BadJarInputsFailClosed)
{
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path outJar = test::getTempOutPath("mj2x_cli_bad_jar_output.jar");
    fs::path cFile  = test::getTempOutPath("mj2x_cli_bad_jar_output.c");

    auto signedJar = runScript({
        q(cli) + " -i " + q(writeSignedJar()) + " -o " + q(outJar) + " --emit-c " + q(cFile),
    });
    EXPECT_NE(signedJar.exitCode, 0);
    EXPECT_NE(signedJar.out.find("signed JAR metadata"), std::string::npos) << signedJar.out;

    auto noClassJar = runScript({
        q(cli) + " -i " + q(writeNoClassJar()) + " -o " + q(outJar) + " --emit-c " + q(cFile),
    });
    EXPECT_NE(noClassJar.exitCode, 0);
    EXPECT_NE(noClassJar.out.find("no root class entries"), std::string::npos) << noClassJar.out;

    auto emptyJar = runScript({
        q(cli) + " -i " + q(writeEmptyJar()) + " -o " + q(outJar) + " --emit-c " + q(cFile),
    });
    EXPECT_NE(emptyJar.exitCode, 0);
    EXPECT_NE(emptyJar.out.find("no root class entries"), std::string::npos) << emptyJar.out;
}

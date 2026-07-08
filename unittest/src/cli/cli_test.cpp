#include <gmock/gmock.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "jzip/zip_io.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "zip/checksum.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

struct ExecResult {
    int exitCode = -1;
    std::string out;
};

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string q(const fs::path& path) {
    std::string s = path.string();
#ifdef _WIN32
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return "\"" + s + "\"";
}

std::string quoteArg(const std::string& value) {
    return "\"" + value + "\"";
}

ExecResult runScript(const std::vector<std::string>& lines) {
    ExecResult result;
    fs::path tmpDir = test::getTempOutPath("mj2x_cli_test.keep").parent_path();
#ifdef _WIN32
    fs::path script = tmpDir / ("mj2x_cli_test_" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".bat");
    {
        std::ofstream out(script);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        for (const auto& line : lines) out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }
#else
    fs::path script = tmpDir / ("mj2x_cli_test_" +
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

fs::path findCli() {
    auto path = test::getBuildTargetPath("mj2x-cli");
    if (fileExists(path)) return path;
    return {};
}

bool commandAvailable(const char* command) {
#ifdef _WIN32
    return std::system((std::string("where ") + command + " >nul 2>nul").c_str()) == 0;
#else
    return std::system((std::string("command -v ") + command + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

fs::path findJavac() {
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

bool hasAutoNativeCompiler() {
#ifdef _WIN32
    return commandAvailable("cl");
#elif defined(__APPLE__)
    return commandAvailable("clang") || commandAvailable("gcc");
#else
    return commandAvailable("gcc") || commandAvailable("clang");
#endif
}

fs::path nativeLibraryPathFor(const fs::path& outputPath) {
#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    return outputPath.parent_path() / (outputPath.stem().string() + ext);
}

void addStaticIntMethod(
    jbytecode::tree::ClassNode& cn,
    const char* name,
    int opcode,
    bool nativeAnnotated) {
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    std::vector<Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        ZUtf8StringRef::from_static(name),
        ZUtf8StringRef::from_static("(II)I"),
        ZUtf8StringRef::from_static(""),
        noExceptions);
    if (nativeAnnotated) {
        mn->visibleAnnotations.push_back(
            new AnnotationNode(ZUtf8StringRef::from_static("Lmj2x/Native;")));
    }
    mn->maxStack = 2;
    mn->maxLocals = 2;
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(opcode));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn.methods.push_back(mn.release());
}

std::vector<ca::u8> tinyClassBytes(const char* className) {
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    auto cn = std::make_unique<ClassNode>();
    Utf8StringArena arena;
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = arena.intern(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        ZUtf8StringRef::from_static("add"),
        ZUtf8StringRef::from_static("(II)I"),
        ZUtf8StringRef::from_static(""),
        noExceptions);
    mn->maxStack = 2;
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

std::vector<ca::u8> selectableClassBytes(
    const char* className,
    bool annotateFirstMethod) {
    using namespace ca::str;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    auto cn = std::make_unique<ClassNode>();
    Utf8StringArena arena;
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = arena.intern(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    addStaticIntMethod(*cn, "annotatedAdd", Opcodes::IADD, annotateFirstMethod);
    addStaticIntMethod(*cn, "plainSub", Opcodes::ISUB, false);

    ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&writer);
    return writer.toByteArray();
}

fs::path writeTinyClass() {
    auto bytes = tinyClassBytes("CliRegCTest");
    fs::path path = test::getTempOutPath("mj2x_cli_regc_input.class");

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path writeSelectableClass(const char* className, const std::string& suffix, bool annotateFirstMethod) {
    auto bytes = selectableClassBytes(className, annotateFirstMethod);
    fs::path path = test::getTempOutPath(suffix);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path compileProvidedAnnotationSample(const fs::path& javac) {
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

    fs::path annotationSource = test::getTopProjectPath() /
        "mj2x" / "mj2x-annotations" / "src" / "main" / "java" / "mj2x" / "Native.java";
    auto result = runScript({
        q(javac) + " -d " + q(dir) + " " + q(annotationSource) + " " + q(sampleSource),
    });
    EXPECT_EQ(result.exitCode, 0) << result.out;
    return dir / "CliProvidedAnnotationSample.class";
}

std::unique_ptr<jzip::LocalFileHeader> makeJarEntry(
    const std::string& name,
    const std::vector<ca::u8>& data) {
    auto local = std::make_unique<jzip::LocalFileHeader>();
    zip::Crc32 crc;
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

struct TestJarEntry {
    std::string name;
    std::vector<ca::u8> data;
};

fs::path writeJarWithEntries(const std::string& suffix, const std::vector<TestJarEntry>& entries) {
    fs::path path = test::getTempOutPath(suffix);
    jzip::ZipArchive archive;
    for (const auto& entry : entries) {
        archive.addPart(makeJarEntry(entry.name, entry.data));
    }
    auto bytes = jzip::ZipIO::writeDirect(archive);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

fs::path writeTinyJar() {
    return writeJarWithEntries("mj2x_cli_input.jar", {
        {"demo/CliJarA.class", tinyClassBytes("demo/CliJarA")},
        {"demo/CliJarB.class", tinyClassBytes("demo/CliJarB")},
        {"assets/config.txt", std::vector<ca::u8>{'o', 'k'}},
    });
}

fs::path writeSignedJar() {
    return writeJarWithEntries("mj2x_cli_signed.jar", {
        {"demo/CliJarA.class", tinyClassBytes("demo/CliJarA")},
        {"META-INF/MORPHER.SF", std::vector<ca::u8>{'s', 'i', 'g'}},
    });
}

fs::path writeNoClassJar() {
    return writeJarWithEntries("mj2x_cli_no_class.jar", {
        {"META-INF/MANIFEST.MF", std::vector<ca::u8>{'M', 'a', 'n', 'i', 'f', 'e', 's', 't'}},
        {"assets/config.txt", std::vector<ca::u8>{'o', 'k'}},
    });
}

fs::path writeEmptyJar() {
    return writeJarWithEntries("mj2x_cli_empty.jar", {});
}

std::vector<ca::u8> readJarEntry(const fs::path& jarPath, const std::string& name) {
    std::ifstream in(jarPath, std::ios::binary | std::ios::ate);
    auto size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    const auto* local = archive.getLocalFileByName(name);
    if (local == nullptr) return {};
    return jzip::ZipIO::decompressLocalFile(*local);
}

std::unique_ptr<jbytecode::tree::ClassNode> readClassFromBytes(
    std::vector<ca::u8>& bytes,
    ca::str::Utf8StringArena& arena) {
    using namespace jbytecode::core;
    using namespace jbytecode::tree;

    EXPECT_LE(bytes.size(), static_cast<size_t>(std::numeric_limits<ca::i32>::max()));
    auto cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

std::unique_ptr<jbytecode::tree::ClassNode> readClassFile(
    const fs::path& path,
    ca::str::Utf8StringArena& arena) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in);
    auto size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return readClassFromBytes(bytes, arena);
}

jbytecode::tree::MethodNode* findMethod(jbytecode::tree::ClassNode& cn, const char* name) {
    for (auto* method : cn.methods) {
        if (method->name.to_std_string() == name)
            return method;
    }
    return nullptr;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<ca::u8> readFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in);
    auto size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

}  // namespace

TEST(CliTest, NativeAnnotationOnlyCompilesAnnotatedMethods) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeSelectableClass(
        "CliAnnotationSelect", "mj2x_cli_annotation_select_input.class", true);
    fs::path outClass = test::getTempOutPath("mj2x_cli_annotation_select_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_annotation_select_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto cn = readClassFile(outClass, arena);
    auto* annotated = findMethod(*cn, "annotatedAdd");
    auto* plain = findMethod(*cn, "plainSub");
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

TEST(CliTest, NativeAnnotationWithNoMatchesMarksNoMethods) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeSelectableClass(
        "CliAnnotationNone", "mj2x_cli_annotation_none_input.class", false);
    fs::path outClass = test::getTempOutPath("mj2x_cli_annotation_none_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_annotation_none_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto cn = readClassFile(outClass, arena);
    auto* first = findMethod(*cn, "annotatedAdd");
    auto* second = findMethod(*cn, "plainSub");
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

TEST(CliTest, ProvidedJavaAnnotationCanSelectMethods) {
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
    fs::path cFile = test::getTempOutPath("mj2x_cli_provided_annotation_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --native-annotation " + quoteArg("Lmj2x/Native;"),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;

    ca::str::Utf8StringArena arena;
    auto cn = readClassFile(outClass, arena);
    auto* annotated = findMethod(*cn, "annotated");
    auto* plain = findMethod(*cn, "plain");
    ASSERT_NE(annotated, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE((annotated->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);
    EXPECT_FALSE((plain->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("{(char*)\"annotated\", (char*)\"(II)I\""), std::string::npos)
        << source;
    EXPECT_EQ(source.find("{(char*)\"plain\""), std::string::npos)
        << source;
    EXPECT_EQ(source.find("Java_CliProvidedAnnotationSample_annotated"), std::string::npos)
        << source;
}

TEST(CliTest, RegCBackendFlagEmitsRegCSource) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_regc_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_regc_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) + " --backend regc",
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

TEST(CliTest, StackCBackendFlagAndLegacyAliasEmitStackCSource) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "cbackend"}) {
        fs::path inClass = writeTinyClass();
        fs::path outClass = test::getTempOutPath(
            std::string("mj2x_cli_") + backendName + "_output.class");
        fs::path cFile = test::getTempOutPath(
            std::string("mj2x_cli_") + backendName + "_output.c");

        auto result = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
                q(cFile) + " --backend " + backendName,
        });
        ASSERT_EQ(result.exitCode, 0) << result.out;
        ASSERT_TRUE(fileExists(outClass));
        ASSERT_TRUE(fileExists(cFile));

        std::string source = readText(cFile);
        EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos)
            << source;
        EXPECT_NE(source.find("union jvalue"), std::string::npos) << source;
    }
}

TEST(CliTest, JarInputEmitsJarAndCSource) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inJar = writeTinyJar();
    fs::path outJar = test::getTempOutPath("mj2x_cli_output.jar");
    fs::path cFile = test::getTempOutPath("mj2x_cli_output.c");

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

    auto classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto cn = readClassFromBytes(classBytes, arena);
    auto* add = findMethod(*cn, "add");
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

TEST(CliTest, RegCJarInputEmitsJarAndRegCSource) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inJar = writeTinyJar();
    fs::path outJar = test::getTempOutPath("mj2x_cli_regc_output.jar");
    fs::path cFile = test::getTempOutPath("mj2x_cli_regc_output.c");

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " + q(cFile) + " --backend regc",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outJar));
    ASSERT_TRUE(fileExists(cFile));

    auto resource = readJarEntry(outJar, "assets/config.txt");
    EXPECT_EQ(resource, std::vector<ca::u8>({'o', 'k'}));

    auto classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto cn = readClassFromBytes(classBytes, arena);
    ASSERT_EQ(cn->methods.size(), 1u);
    EXPECT_TRUE((cn->methods[0]->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("Generated by mj2x RegCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("regc_demo_CliJarA_add__II_I"), std::string::npos) << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
}

TEST(CliTest, ClassInputRepeatableForStackCAndRegC) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "regc"}) {
        fs::path inClass = writeTinyClass();
        fs::path outClassA = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_a.class");
        fs::path outClassB = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_b.class");
        fs::path cFileA = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_a.c");
        fs::path cFileB = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_b.c");

        auto first = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClassA) + " --emit-c " +
                q(cFileA) + " --backend " + backendName,
        });
        ASSERT_EQ(first.exitCode, 0) << first.out;
        auto second = runScript({
            q(cli) + " -i " + q(inClass) + " -o " + q(outClassB) + " --emit-c " +
                q(cFileB) + " --backend " + backendName,
        });
        ASSERT_EQ(second.exitCode, 0) << second.out;

        EXPECT_EQ(readFileBytes(outClassA), readFileBytes(outClassB));
        EXPECT_EQ(readText(cFileA), readText(cFileB));
    }
}

TEST(CliTest, JarInputRepeatableForStackCAndRegC) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    for (const char* backendName : {"stackc", "regc"}) {
        fs::path inJar = writeTinyJar();
        fs::path outJarA = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_a.jar");
        fs::path outJarB = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_b.jar");
        fs::path cFileA = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_a.c");
        fs::path cFileB = test::getTempOutPath(
            std::string("mj2x_cli_repeat_") + backendName + "_b.c");

        auto first = runScript({
            q(cli) + " -i " + q(inJar) + " -o " + q(outJarA) + " --emit-c " +
                q(cFileA) + " --backend " + backendName,
        });
        ASSERT_EQ(first.exitCode, 0) << first.out;
        auto second = runScript({
            q(cli) + " -i " + q(inJar) + " -o " + q(outJarB) + " --emit-c " +
                q(cFileB) + " --backend " + backendName,
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

TEST(CliTest, GenerateCMakeProjectEmitsStandaloneFiles) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_cmake_project_output.class");
    fs::path projectDir = test::getTempOutPath("mj2x_cli_cmake_project.keep").parent_path() /
        "mj2x_cli_cmake_project";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) +
            " --generate-cmake " + q(projectDir),
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    ASSERT_TRUE(fileExists(outClass));
    EXPECT_TRUE(fileExists(projectDir / "native.c")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "CMakeLists.txt")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / ".gitignore")) << result.out;
    EXPECT_TRUE(fileExists(projectDir / "README.md")) << result.out;

    std::string cmake = readText(projectDir / "CMakeLists.txt");
    EXPECT_NE(cmake.find("find_package(JNI REQUIRED)"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("add_library(mj2x_native SHARED native.c)"), std::string::npos) << cmake;

    std::string source = readText(projectDir / "native.c");
    EXPECT_NE(source.find("Generated by mj2x StackCBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
}

TEST(CliTest, CompileFailsClearlyWhenJavaHomeMissing) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_compile_no_java_home_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_compile_no_java_home_output.c");

#ifdef _WIN32
    std::string unsetJavaHome = "set JAVA_HOME=";
#else
    std::string unsetJavaHome = "unset JAVA_HOME";
#endif

    auto result = runScript({
        unsetJavaHome,
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --compile",
    });
    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.out.find("JAVA_HOME is not set"), std::string::npos) << result.out;
}

TEST(CliTest, CompileEmitsNativeLibraryWhenToolchainIsAvailable) {
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

    fs::path inClass = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_compile_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_compile_output.c");
    fs::path nativeLibrary = nativeLibraryPathFor(outClass);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;
}

TEST(CliTest, CompileJarEmbedsNativeLoaderAndLibraryWhenToolchainIsAvailable) {
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

    fs::path inJar = writeTinyJar();
    fs::path outJar = test::getTempOutPath("mj2x_cli_compile_output.jar");
    fs::path cFile = test::getTempOutPath("mj2x_cli_compile_output.c");
    fs::path nativeLibrary = nativeLibraryPathFor(outJar);
    std::error_code ec;
    fs::remove(nativeLibrary, ec);

    auto result = runScript({
        q(cli) + " -i " + q(inJar) + " -o " + q(outJar) + " --emit-c " +
            q(cFile) + " --compile",
    });
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_TRUE(fileExists(nativeLibrary)) << result.out;

    auto loaderClass = readJarEntry(outJar, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(loaderClass.empty()) << result.out;
    auto cloaderClass = readJarEntry(outJar, "mj2x/cloader/Loader.class");
    EXPECT_FALSE(cloaderClass.empty()) << result.out;
    auto nativeResource = readJarEntry(
        outJar,
        "META-INF/mj2x/native/" + nativeLibrary.filename().string());
    EXPECT_FALSE(nativeResource.empty()) << result.out;

    auto classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto cn = readClassFromBytes(classBytes, arena);
    auto* clinit = findMethod(*cn, "<clinit>");
    ASSERT_NE(clinit, nullptr);
    bool callsLoader = false;
    for (auto* insn : clinit->instructions.toArray()) {
        if (insn->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN)
            continue;
        auto* call = static_cast<jbytecode::tree::MethodInsnNode*>(insn);
        callsLoader = callsLoader ||
            (call->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
             call->name.to_std_string() == "load");
    }
    EXPECT_TRUE(callsLoader);
}

TEST(CliTest, NegativeArgumentsFail) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path inClass = writeTinyClass();
    fs::path outClass = test::getTempOutPath("mj2x_cli_negative_output.class");
    fs::path cFile = test::getTempOutPath("mj2x_cli_negative_output.c");

    auto noArgs = runScript({q(cli)});
    EXPECT_NE(noArgs.exitCode, 0);
    EXPECT_NE(noArgs.out.find("Usage:"), std::string::npos) << noArgs.out;

    auto unknownBackend = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " + q(cFile) + " --backend nope",
    });
    EXPECT_NE(unknownBackend.exitCode, 0);
    EXPECT_NE(unknownBackend.out.find("Unknown --backend value"), std::string::npos)
        << unknownBackend.out;

    auto backendWithoutEmit = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --backend regc",
    });
    EXPECT_NE(backendWithoutEmit.exitCode, 0);
    EXPECT_NE(backendWithoutEmit.out.find("--backend requires --emit-c"), std::string::npos)
        << backendWithoutEmit.out;

    auto compileWithoutEmit = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --compile",
    });
    EXPECT_NE(compileWithoutEmit.exitCode, 0);
    EXPECT_NE(compileWithoutEmit.out.find("--compile requires --emit-c"), std::string::npos)
        << compileWithoutEmit.out;

    auto badAnnotationDescriptor = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --native-annotation " + quoteArg("mj2x.Native"),
    });
    EXPECT_NE(badAnnotationDescriptor.exitCode, 0);
    EXPECT_NE(badAnnotationDescriptor.out.find("--native-annotation must be"), std::string::npos)
        << badAnnotationDescriptor.out;

    auto badCompiler = runScript({
        q(cli) + " -i " + q(inClass) + " -o " + q(outClass) + " --emit-c " +
            q(cFile) + " --compile --c-compiler nope",
    });
    EXPECT_NE(badCompiler.exitCode, 0);
    EXPECT_NE(badCompiler.out.find("Unknown --c-compiler value"), std::string::npos)
        << badCompiler.out;

    auto missingInput = runScript({
        q(cli) + " -i " + q(test::getTempOutPath("mj2x_cli_missing_input.class")) +
            " -o " + q(outClass) + " --emit-c " + q(cFile),
    });
    EXPECT_NE(missingInput.exitCode, 0);
    EXPECT_NE(missingInput.out.find("load failed"), std::string::npos) << missingInput.out;
}

TEST(CliTest, BadJarInputsFailClosed) {
    fs::path cli = findCli();
    if (cli.empty()) {
        GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path outJar = test::getTempOutPath("mj2x_cli_bad_jar_output.jar");
    fs::path cFile = test::getTempOutPath("mj2x_cli_bad_jar_output.c");

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

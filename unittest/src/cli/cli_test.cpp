#include <gmock/gmock.h>

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
#ifdef _WIN32
    fs::path path = test::getTopProjectPath() / "build" / "windows" / "x64" / "release" / "mj2x-cli.exe";
#else
    fs::path path = test::getTopProjectPath() / "build" / "linux" / "x86_64" / "release" / "mj2x-cli";
#endif
    return fileExists(path) ? path : fs::path{};
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

fs::path writeTinyClass() {
    auto bytes = tinyClassBytes("CliRegCTest");
    fs::path path = test::getTempOutPath("mj2x_cli_regc_input.class");

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
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

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

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

    auto classBytes = readJarEntry(outJar, "demo/CliJarA.class");
    ca::str::Utf8StringArena arena;
    auto cn = readClassFromBytes(classBytes, arena);
    ASSERT_EQ(cn->methods.size(), 1u);
    EXPECT_TRUE((cn->methods[0]->access & jbytecode::core::Opcodes::ACC_NATIVE) != 0);

    std::string source = readText(cFile);
    EXPECT_NE(source.find("Generated by mj2x CBackend"), std::string::npos) << source;
    EXPECT_NE(source.find("Java_demo_CliJarA_add"), std::string::npos) << source;
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

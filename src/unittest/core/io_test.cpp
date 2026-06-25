#include <gmock/gmock.h>

#include <fstream>

#include "libca/str/utf8_string.hpp"
#include "test_helper.hpp"
#include "mj2x/mj2x.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using namespace ca::str;

// Helper: create a minimal class in memory
static std::unique_ptr<ClassNode> makeSimpleClass() {
    auto cn = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = ZUtf8StringRef::from_static("TestClass");
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ZUtf8StringRef::from_static("add"), ZUtf8StringRef::from_static("(II)I"),
        ZUtf8StringRef::from_static(""), noExceptions);
    mn->maxStack = 2;
    mn->maxLocals = 2;

    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(Opcodes::IADD));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));

    cn->methods.push_back(mn.release());
    return cn;
}

// Helper: write class bytes to temp file
static std::string writeTempClass(const std::string& suffix) {
    auto path = test::getTempOutPath("mj2x_" + suffix).string();

    auto cn = makeSimpleClass();
    ca::str::Utf8StringArena arena;
    ClassWriter cw(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

TEST(IOTest, ClassWriteReadRoundtrip) {
    auto cn = makeSimpleClass();
    ca::str::Utf8StringArena arena;
    ClassWriter cw(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();
    ASSERT_GT(bytes.size(), 50);

    ClassReader reader(arena, bytes.data(), bytes.size());
    ClassNode cn2;
    reader.accept(&cn2, 0);

    EXPECT_EQ(cn2.name, ZUtf8StringRef::from_static("TestClass"));
    EXPECT_EQ(cn2.methods.size(), 1);
    EXPECT_EQ(cn2.methods[0]->name, ZUtf8StringRef::from_static("add"));
    EXPECT_EQ(cn2.methods[0]->desc, ZUtf8StringRef::from_static("(II)I"));
}

TEST(IOTest, InputLoaderRoundtrip) {
    auto path = writeTempClass("loader_test.class");

    InputLoader loader;
    auto index = loader.load(path);
    ASSERT_NE(index, nullptr);

    auto* cls = index->getClass("TestClass");
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->classNode->methods.size(), 1);

    std::remove(path.c_str());
}

TEST(PipelineTest, BadInputPathReturnsError) {
    PipelineConfig cfg;
    cfg.inputPath = "nonexistent.class";
    cfg.outputPath = "out.class";
    Pipeline pipeline(std::move(cfg));
    EXPECT_NE(pipeline.run(), 0);
}

TEST(PipelineTest, FullPipelineRoundtrip) {
    auto inPath = writeTempClass("pipeline_in.class");
    auto outPath = test::getTempOutPath("mj2x_pipeline_out.class").string();

    PipelineConfig cfg;
    cfg.inputPath = inPath;
    cfg.outputPath = outPath;
    Pipeline pipeline(std::move(cfg));
    EXPECT_EQ(pipeline.run(), 0);

    std::ifstream ifs(outPath, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(ifs);
    EXPECT_GT(ifs.tellg(), 50);
    ifs.close();

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
}


#include <gmock/gmock.h>

#include <cstdlib>
#include <fstream>

#include "mj2x/mj2x.hpp"

using namespace mj2x;
using namespace jbytecode;

// Helper: create a minimal class in memory
static std::unique_ptr<ClassNode> makeSimpleClass() {
    auto cn = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = "TestClass";
    cn->superName = "java/lang/Object";

    std::vector<std::string> noExceptions;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
        "", noExceptions);
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
    auto tmpDir = std::getenv("TEMP");
    auto path = std::string(tmpDir ? tmpDir : ".") + "/mj2x_" + suffix;

    auto cn = makeSimpleClass();
    ClassWriter cw(ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

TEST(IOTest, ClassWriteReadRoundtrip) {
    auto cn = makeSimpleClass();
    ClassWriter cw(ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();
    ASSERT_GT(bytes.size(), 50);

    ClassReader reader(bytes.data(), bytes.size());
    ClassNode cn2;
    reader.accept(&cn2, 0);

    EXPECT_EQ(cn2.name, "TestClass");
    EXPECT_EQ(cn2.methods.size(), 1);
    EXPECT_EQ(cn2.methods[0]->name, "add");
    EXPECT_EQ(cn2.methods[0]->desc, "(II)I");
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
    auto outPath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
                   "/mj2x_pipeline_out.class";

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

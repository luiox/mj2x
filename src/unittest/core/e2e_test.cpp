#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "mj2x/mj2x.hpp"
#include "mj2x/backend/c_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"

using namespace mj2x;
using namespace jbytecode;

// Helper: construct a class with one method and write to disk
static std::string writeTestClass(const std::string& name, const std::string& suffix) {
    auto tmp = std::getenv("TEMP");
    auto path = std::string(tmp ? tmp : ".") + "/mj2x_e2e_" + suffix;

    auto cn = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = name;
    cn->superName = "java/lang/Object";

    std::vector<std::string> noExcepts;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I", "", noExcepts);
    mn->maxStack = 2;
    mn->maxLocals = 2;
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(Opcodes::IADD));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn->methods.push_back(mn.release());

    ClassWriter cw(ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

TEST(E2ETest, PipelineWithCBackend) {
    auto inPath = writeTestClass("E2ETest", "in.class");
    auto outPath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                   + "/mj2x_e2e_out.class";
    auto cPath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                 + "/mj2x_e2e_out.c";

    PipelineConfig cfg;
    cfg.inputPath = inPath;
    cfg.outputPath = outPath;

    Pipeline pipeline(std::move(cfg));
    auto cBackend = std::make_unique<CBackend>();
    cBackend->setOutputPath(cPath);
    pipeline.registerBackend(std::move(cBackend));
    pipeline.registerPreprocessor(std::make_unique<MarkAllMethodsPreprocessor>());

    EXPECT_EQ(pipeline.run(), 0);

    // Verify .class output
    {
        std::ifstream ifs(outPath, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(ifs);
        EXPECT_GT(ifs.tellg(), 50);
    }

    // Verify .c output
    {
        std::ifstream ifs(cPath);
        ASSERT_TRUE(ifs);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("JNIEXPORT"), std::string::npos);
        EXPECT_NE(content.find("return stack[sp--].i"), std::string::npos);
        EXPECT_NE(content.find("E2ETest_add"), std::string::npos);
        printf("=== Generated C ===\n%s\n", content.c_str());
    }

    // Verify the .class has ACC_NATIVE on the compiled method
    {
        InputLoader loader;
        auto index = loader.load(outPath);
        auto* cls = index->getClass("E2ETest");
        ASSERT_NE(cls, nullptr);
        ASSERT_GE(cls->methods.size(), 1);
        EXPECT_TRUE(cls->methods[0].methodNode->access & Opcodes::ACC_NATIVE);
        EXPECT_EQ(cls->methods[0].methodNode->instructions.size(), 0);
    }

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
    std::remove(cPath.c_str());
}

TEST(E2ETest, VoidMethodWithCBackend) {
    auto tmp = std::getenv("TEMP");
    auto inPath = std::string(tmp ? tmp : ".") + "/mj2x_e2e_void_in.class";
    auto outPath = std::string(tmp ? tmp : ".") + "/mj2x_e2e_void_out.class";
    auto cPath = std::string(tmp ? tmp : ".") + "/mj2x_e2e_void_out.c";

    // Manual class construction
    auto cn = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->name = "VoidTest";
    cn->superName = "java/lang/Object";

    std::vector<std::string> noExcepts;
    auto mn = std::make_unique<MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "noop", "()V", "", noExcepts);
    mn->maxStack = 1;
    mn->maxLocals = 0;
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    cn->methods.push_back(mn.release());

    ClassWriter cw(ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();
    {
        std::ofstream ofs(inPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    PipelineConfig cfg;
    cfg.inputPath = inPath;
    cfg.outputPath = outPath;

    Pipeline pipeline(std::move(cfg));
    auto cBackend = std::make_unique<CBackend>();
    cBackend->setOutputPath(cPath);
    pipeline.registerBackend(std::move(cBackend));
    pipeline.registerPreprocessor(std::make_unique<MarkAllMethodsPreprocessor>());

    EXPECT_EQ(pipeline.run(), 0);

    // Verify .c content
    {
        std::ifstream ifs(cPath);
        ASSERT_TRUE(ifs);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("return;"), std::string::npos);
        EXPECT_NE(content.find("void"), std::string::npos);
        printf("=== Generated C ===\n%s\n", content.c_str());
    }

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
    std::remove(cPath.c_str());
}

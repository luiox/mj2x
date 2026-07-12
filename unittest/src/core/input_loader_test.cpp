#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

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

TEST(InputLoaderTest, ReadRealClass)
{
    auto path = test::getTempOutPath("mj2x_input_loader_real.class");

    Utf8StringArena arena;
    auto            cn = std::make_unique<ClassNode>();
    cn->version        = Opcodes::V1_8;
    cn->access         = Opcodes::ACC_PUBLIC;
    cn->name           = arena.intern("Test");
    cn->superName      = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<Utf8StringRef> noExceptions;
    auto mn       = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static("id"),
                                           ZUtf8StringRef::from_static("(I)I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    mn->maxStack  = 1;
    mn->maxLocals = 1;
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn->methods.push_back(mn.release());

    ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&writer);
    auto          bytes = writer.toByteArray();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();

    InputLoader loader;
    auto        index = loader.load(path.string());
    ASSERT_NE(index, nullptr);

    auto* clazz = index->getClass("Test");
    ASSERT_NE(clazz, nullptr);
    EXPECT_GE(clazz->classNode->methods.size(), 1);
    EXPECT_EQ(clazz->classNode->name, ZUtf8StringRef::from_static("Test"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

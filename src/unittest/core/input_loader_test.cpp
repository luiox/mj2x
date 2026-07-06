#include <gmock/gmock.h>

#include <cstdio>
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

namespace {

std::string writeTestClass()
{
    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC;
    cn.name      = ZUtf8StringRef::from_static("Test");
    cn.superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static("value"),
                                           ZUtf8StringRef::from_static("()I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    mn->maxStack  = 1;
    mn->maxLocals = 0;
    mn->instructions.add(new InsnNode(Opcodes::ICONST_1));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));
    cn.methods.push_back(mn.release());

    ca::str::Utf8StringArena arena;
    ClassWriter              writer(arena, ClassWriter::COMPUTE_MAXS);
    cn.accept(&writer);
    const auto bytes = writer.toByteArray();

    const auto path = test::getTempOutPath("mj2x_input_loader_Test.class").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

}   // namespace

TEST(InputLoaderTest, ReadRealClass) {
    const auto path = writeTestClass();

    InputLoader loader;
    auto index = loader.load(path);
    ASSERT_NE(index, nullptr);

    auto* clazz = index->getClass("Test");
    ASSERT_NE(clazz, nullptr);
    EXPECT_GE(clazz->classNode->methods.size(), 1);
    EXPECT_EQ(clazz->classNode->name, ZUtf8StringRef::from_static("Test"));

    std::remove(path.c_str());
}


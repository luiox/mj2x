#include <gmock/gmock.h>

#include "mj2x/core/compiler_index.hpp"

using namespace mj2x;

TEST(CompilerIndexTest, AddAndRetrieveClass) {
    CompilerIndex index;
    auto cn = std::make_unique<jbytecode::tree::ClassNode>();
    cn->name = "Test";
    cn->version = jbytecode::core::Opcodes::V1_8;
    cn->access = jbytecode::core::Opcodes::ACC_PUBLIC;
    cn->superName = "java/lang/Object";

    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass("Test", std::move(cls));

    auto* retrieved = index.getClass("Test");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->classNode->name, "Test");
    EXPECT_FALSE(retrieved->modified);
}

TEST(CompilerIndexTest, GetNonExistentClassReturnsNull) {
    CompilerIndex index;
    EXPECT_EQ(index.getClass("NonExistent"), nullptr);
}

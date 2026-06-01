#include <gmock/gmock.h>

#include "mj2x/core/compiler_index.hpp"
#include "libca/str/utf8_string.hpp"

using namespace mj2x;

TEST(CompilerIndexTest, AddAndRetrieveClass) {
    CompilerIndex index;
    auto cn = std::make_unique<jbytecode::tree::ClassNode>();
    ca::str::Utf8String name("Test");
    ca::str::Utf8String superName("java/lang/Object");
    cn->name = name.ref();
    cn->version = jbytecode::core::Opcodes::V1_8;
    cn->access = jbytecode::core::Opcodes::ACC_PUBLIC;
    cn->superName = superName.ref();

    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass("Test", std::move(cls));

    auto* retrieved = index.getClass("Test");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->classNode->name, name.ref());
    EXPECT_FALSE(retrieved->modified);
}

TEST(CompilerIndexTest, GetNonExistentClassReturnsNull) {
    CompilerIndex index;
    EXPECT_EQ(index.getClass("NonExistent"), nullptr);
}

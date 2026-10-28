#include <gmock/gmock.h>

#include "mj2x/core/compiler_index.hpp"
#include "libca/str/utf8_string.hpp"
#include "test_helper.hpp"

using namespace mj2x;

TEST(CompilerIndexTest, AddAndRetrieveClass)
{
    CompilerIndex       index;
    auto                cn = std::make_unique<jbytecode::tree::ClassNode>();
    ca::str::Utf8String name("Test");
    ca::str::Utf8String superName("java/lang/Object");
    cn->name      = name.ref();
    cn->version   = jbytecode::core::Opcodes::V1_8;
    cn->access    = jbytecode::core::Opcodes::ACC_PUBLIC;
    cn->superName = superName.ref();

    index.addClass("Test", test::classFromTree("Test", *cn, index.arena()));

    auto* retrieved = index.getClass("Test");
    ASSERT_NE(retrieved, nullptr);
    ASSERT_NE(retrieved->dom, nullptr);
    EXPECT_EQ(retrieved->dom->thisName(), "Test");
    EXPECT_FALSE(retrieved->modified);
}

TEST(CompilerIndexTest, GetNonExistentClassReturnsNull)
{
    CompilerIndex index;
    EXPECT_EQ(index.getClass("NonExistent"), nullptr);
}

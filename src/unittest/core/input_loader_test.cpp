#include <gmock/gmock.h>

#include "mj2x/mj2x.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

TEST(InputLoaderTest, ReadRealClass) {
    auto* cls = std::getenv("TEMP");
    ASSERT_NE(cls, nullptr);
    auto path = std::string(cls) + "/Test.class";

    InputLoader loader;
    auto index = loader.load(path);
    ASSERT_NE(index, nullptr);

    auto* clazz = index->getClass("Test");
    ASSERT_NE(clazz, nullptr);
    EXPECT_GE(clazz->classNode->methods.size(), 1);
    EXPECT_EQ(clazz->classNode->name, "Test");
}

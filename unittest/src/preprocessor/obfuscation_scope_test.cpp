// obfuscation_scope_test.cpp
//
// 混淆作用域（issue #8）测试：类级 glob 判定"哪些类受 RegIR 变换影响"。
//
// 判定结果落在 CompilerClass::inObfuscationScope（方案 (b)），与 RegIR 完全
// 解耦；RegC / LLVM 两个后端查同一标记，故这里只测标记本身 + 语义边界。

#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "garena/arena.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/preprocessor/native_class_filter_preprocessor.hpp"
#include "mj2x/preprocessor/obfuscation_scope_preprocessor.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

garena::Arena& scratch()
{
    static garena::Arena arena;
    return arena;
}

void addClass(CompilerIndex& index, const char* internalName, bool isLibrary = false)
{
    auto node = std::make_unique<ClassNode>();
    node->name = ca::str::Utf8StringRef::from_cstr(internalName);
    node->superName = ca::str::Utf8StringRef::from_cstr("java/lang/Object");
    CompilerClass clazz;
    clazz.classNode = std::move(node);
    clazz.isLibrary = isLibrary;
    index.addClass(internalName, std::move(clazz));
}

}   // namespace

TEST(ObfuscationScopeTest, EmptyScopeLeavesEveryClassInScope)
{
    // 零行为变化是硬要求：不给任何模式时，产物必须与改动前逐字节一致。
    CompilerIndex index;
    addClass(index, "demo/A");
    addClass(index, "demo/B");

    ObfuscationScopePreprocessor preprocessor({}, {});
    preprocessor.process(index);

    EXPECT_EQ(preprocessor.outOfScopeCount(), 0u);
    EXPECT_TRUE(index.getClass("demo/A")->inObfuscationScope);
    EXPECT_TRUE(index.getClass("demo/B")->inObfuscationScope);
}

TEST(ObfuscationScopeTest, IncludeNarrowsScopeToMatchingClasses)
{
    CompilerIndex index;
    addClass(index, "demo/Core");
    addClass(index, "demo/Util");

    ObfuscationScopePreprocessor preprocessor({"demo/Core"}, {});
    preprocessor.process(index);

    EXPECT_TRUE(index.getClass("demo/Core")->inObfuscationScope);
    EXPECT_FALSE(index.getClass("demo/Util")->inObfuscationScope);
    EXPECT_EQ(preprocessor.outOfScopeCount(), 1u);
}

TEST(ObfuscationScopeTest, ExcludeWinsOverInclude)
{
    // 与 --native-include/exclude 同语义：exclude 优先。
    CompilerIndex index;
    addClass(index, "demo/Core");

    ObfuscationScopePreprocessor preprocessor({"demo/**"}, {"demo/Core"});
    preprocessor.process(index);

    EXPECT_FALSE(index.getClass("demo/Core")->inObfuscationScope);
}

TEST(ObfuscationScopeTest, GlobSemanticsMatchClassFilter)
{
    // 模式语义复用 classPassesFilter（`*` 不跨段、`**` 跨段），不新造一套。
    CompilerIndex index;
    addClass(index, "com/example/Secret");
    addClass(index, "com/example/deep/Hidden");

    // `*` 不跨段：只命中 Secret。
    ObfuscationScopePreprocessor star({"com/example/*"}, {});
    star.process(index);
    EXPECT_TRUE(index.getClass("com/example/Secret")->inObfuscationScope);
    EXPECT_FALSE(index.getClass("com/example/deep/Hidden")->inObfuscationScope);

    // `**` 跨段：两个都命中。
    ObfuscationScopePreprocessor starStar({"com/example/**"}, {});
    starStar.process(index);
    EXPECT_TRUE(index.getClass("com/example/Secret")->inObfuscationScope);
    EXPECT_TRUE(index.getClass("com/example/deep/Hidden")->inObfuscationScope);
}

TEST(ObfuscationScopeTest, LibraryClassesAreNeverScoped)
{
    // 库类不参与混淆作用域：它们本来也不进入编译集。
    CompilerIndex index;
    addClass(index, "java/lang/String", /*isLibrary=*/true);

    ObfuscationScopePreprocessor preprocessor({"**"}, {});
    preprocessor.process(index);

    // 库类被跳过（不计数、不改标记）。
    EXPECT_EQ(preprocessor.outOfScopeCount(), 0u);
}

TEST(ObfuscationScopeTest, SlashInPatternIsNormalized)
{
    // 模式侧容忍未归一化的形态（`/` → `.`），与 native filter 一致。
    CompilerIndex index;
    addClass(index, "dev/aide/jvmobf/I18n");

    ObfuscationScopePreprocessor preprocessor({"dev/aide/jvmobf/I18n"}, {});
    preprocessor.process(index);

    EXPECT_TRUE(index.getClass("dev/aide/jvmobf/I18n")->inObfuscationScope);
    EXPECT_EQ(preprocessor.outOfScopeCount(), 0u);
}

// NativeClassFilterPreprocessor 单元测试（对标 jvmtp filter.*，dev_plan §K P1-3）。
//
// 验证：
// - glob 语义：`*` 不跨段 / `**` 跨段 / `?` 单字符不跨段 / 字面量精确匹配；
// - normalizeClassGlobs：逗号拆分、空白修剪、`/` → `.`、空片段拒绝；
// - classPassesFilter：include 为空全放行、exclude 优先、多模式任一命中；
// - preprocessor 集成：filteredOut 置位 + MarkAll 联动（被剔除类不被标记）。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "test_helper.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/native_class_filter_preprocessor.hpp"

using namespace mj2x;

namespace {

// 构造一个空方法体类（名 + 一个普通静态方法），供 preprocessor 集成测试。
void buildSimpleClass(CompilerIndex& index, const std::string& className)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<jbytecode::tree::ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = jbytecode::core::Opcodes::V1_8;
    cn->access    = jbytecode::core::Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExc;
    auto* mn = new jbytecode::tree::MethodNode(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                                               arena.intern("run"), arena.intern("()V"),
                                               arena.intern(""), noExc);
    mn->instructions.add(new jbytecode::tree::InsnNode(jbytecode::core::Opcodes::RETURN));
    cn->methods.push_back(mn);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    for (auto* m : cls.classNode->methods) {
        CompilerMethod cm;
        cm.methodNode = m;
        cls.methods.push_back(std::move(cm));
    }
    index.addClass(className, std::move(cls));
}

bool anyMarked(const CompilerClass& cls)
{
    for (const auto& m : cls.methods) {
        if (m.markedForCompile) return true;
    }
    return false;
}

}   // namespace

// ---- glob 语义 ----

TEST(NativeClassFilterGlobTest, ExactAndSegmentSemantics)
{
    // 字面量精确匹配。
    EXPECT_TRUE(globMatchClassName("demo.CliJarA", "demo.CliJarA"));
    EXPECT_FALSE(globMatchClassName("demo.CliJarA", "demo.CliJarB"));
    EXPECT_FALSE(globMatchClassName("demo.CliJarA", "demo.CliJar"));

    // `*` 段内通配：不跨 `.`。
    EXPECT_TRUE(globMatchClassName("demo.CliJarA", "demo.*"));
    EXPECT_TRUE(globMatchClassName("demo.CliJarA", "*.CliJarA"));
    EXPECT_TRUE(globMatchClassName("demo.CliJarA", "de*o.Cli*rA"));
    EXPECT_FALSE(globMatchClassName("demo.sub.CliJarA", "demo.*"));
    EXPECT_TRUE(globMatchClassName("demo.sub.CliJarA", "demo.*.*"));

    // `*` 可匹配空串。
    EXPECT_TRUE(globMatchClassName("demo.", "demo.*"));
    EXPECT_TRUE(globMatchClassName("demo", "demo*"));

    // `**` 跨段（含匹配空串）。注意 `demo.**` 里 `**` 前有字面点：要求文本
    // 含尾点，故 "demo" 本身不命中（与 jvmtp 纯 glob / Java PathMatcher 一致）。
    EXPECT_TRUE(globMatchClassName("demo.sub.CliJarA", "demo.**"));
    EXPECT_FALSE(globMatchClassName("demo", "demo.**"));
    EXPECT_TRUE(globMatchClassName("demo.", "demo.**"));
    EXPECT_TRUE(globMatchClassName("a.b.c.d", "**.d"));
    EXPECT_FALSE(globMatchClassName("demo", "other.**"));

    // `?` 单字符且不跨段。
    EXPECT_TRUE(globMatchClassName("demo.A", "dem?.A"));
    EXPECT_FALSE(globMatchClassName("demo.AB", "demo.?"));
    EXPECT_FALSE(globMatchClassName("demo.A", "demo?A"));   // `?` 不吃 `.`

    // 前导/中间/尾随星号组合。
    EXPECT_TRUE(globMatchClassName("com.acme.core.Engine", "com.acme.*.Engine"));
    EXPECT_TRUE(globMatchClassName("com.acme.core.deep.Engine", "com.acme.**.Engine"));
    EXPECT_FALSE(globMatchClassName("com.acme.core.Engine", "com.acme.*.Engines"));

    // 内部名 `/` 形态由 classPassesFilter 归一化；裸匹配器只认归一化文本。
    EXPECT_FALSE(globMatchClassName("demo/CliJarA", "demo.*"));
}

TEST(NativeClassFilterGlobTest, EmptyAndDegenerateShapes)
{
    EXPECT_TRUE(globMatchClassName("", ""));
    EXPECT_TRUE(globMatchClassName("", "*"));
    EXPECT_TRUE(globMatchClassName("", "**"));
    EXPECT_FALSE(globMatchClassName("", "a"));
    EXPECT_FALSE(globMatchClassName("a", ""));
    EXPECT_TRUE(globMatchClassName("a", "*"));
    EXPECT_TRUE(globMatchClassName("a", "?"));
    EXPECT_FALSE(globMatchClassName("a.b", "?"));
    // `***` 解析为 `**` + `*`，行为同 `**`。
    EXPECT_TRUE(globMatchClassName("a.b.c", "***"));
    EXPECT_TRUE(globMatchClassName("abc", "***"));
}

// ---- 归一化 ----

TEST(NativeClassFilterGlobTest, NormalizeSplitsTrimsAndConvertsSeparators)
{
    std::vector<std::string> out;
    ASSERT_TRUE(normalizeClassGlobs({"com/example/critical/**, com/example/Generated*"}, out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "com.example.critical.**");
    EXPECT_EQ(out[1], "com.example.Generated*");

    // 多值 + 每条内部逗号分隔 + 空白修剪。
    ASSERT_TRUE(normalizeClassGlobs({"  a.b  ", "c/d, e.f "}, out));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "a.b");
    EXPECT_EQ(out[1], "c.d");
    EXPECT_EQ(out[2], "e.f");

    // 单个值即可多次拆分。
    ASSERT_TRUE(normalizeClassGlobs({"a,b,c"}, out));
    ASSERT_EQ(out.size(), 3u);
}

TEST(NativeClassFilterGlobTest, NormalizeRejectsEmptyFragments)
{
    std::vector<std::string> out;
    EXPECT_FALSE(normalizeClassGlobs({""}, out));          // 空串
    EXPECT_FALSE(normalizeClassGlobs({"   "}, out));       // 纯空白
    EXPECT_FALSE(normalizeClassGlobs({"a,,b"}, out));      // 中间空片段
    EXPECT_FALSE(normalizeClassGlobs({"a,"}, out));        // 尾随逗号
    EXPECT_FALSE(normalizeClassGlobs({",a"}, out));        // 前导逗号
    EXPECT_FALSE(normalizeClassGlobs({"a", ""}, out));     // 多值中有一个空
}

// ---- 门语义 ----

TEST(NativeClassFilterGlobTest, ClassPassesFilterSemantics)
{
    // include 为空：全放行（再受 exclude 约束）。
    EXPECT_TRUE(classPassesFilter("com.acme.Engine", {}, {}));
    EXPECT_TRUE(classPassesFilter("anything", {}, {}));

    // include 非空：必须命中至少一个。
    const std::vector<std::string> include = {"com.acme.**", "com.beta.*"};
    EXPECT_TRUE(classPassesFilter("com.acme.Engine", include, {}));
    EXPECT_TRUE(classPassesFilter("com.beta.Tool", include, {}));
    EXPECT_FALSE(classPassesFilter("com.gamma.Other", include, {}));
    // 内部名 `/` 形态同样命中（文本侧归一化）。
    EXPECT_TRUE(classPassesFilter("com/acme/deep/Engine", include, {}));

    // exclude 命中即剔除，且优先于 include。
    const std::vector<std::string> exclude = {"com.acme.test.**", "**.Generated*"};
    EXPECT_TRUE(classPassesFilter("com.acme.Engine", include, exclude));
    EXPECT_FALSE(classPassesFilter("com.acme.test.Helper", include, exclude));
    EXPECT_FALSE(classPassesFilter("com.acme.GeneratedFoo", include, exclude));
    // exclude-only（include 为空）。
    EXPECT_FALSE(classPassesFilter("com.acme.test.Helper", {}, exclude));
    EXPECT_TRUE(classPassesFilter("com.acme.Engine", {}, exclude));

    // jvmtp 文档同款样例。
    EXPECT_TRUE(classPassesFilter("com/example/critical/Crypto",
                                  {"com/example/critical/**"}, {}));
    EXPECT_FALSE(classPassesFilter("com/example/test/Helper",
                                   {"com/example/critical/**"}, {}));
    EXPECT_TRUE(classPassesFilter("com/example/Critical", {},
                                  {"com/example/test/**", "com/example/Generated*"}));
    EXPECT_FALSE(classPassesFilter("com/example/test/Helper", {},
                                   {"com/example/test/**", "com/example/Generated*"}));
    EXPECT_FALSE(classPassesFilter("com/example/GeneratedFoo", {},
                                   {"com/example/test/**", "com/example/Generated*"}));
}

// ---- preprocessor 集成 ----

TEST(NativeClassFilterPreprocessorTest, SetsFilteredOutAndCounts)
{
    CompilerIndex index;
    buildSimpleClass(index, "com/acme/Engine");
    buildSimpleClass(index, "com/acme/test/Helper");
    buildSimpleClass(index, "com/third/Widget");

    NativeClassFilterPreprocessor filter({"com.acme.**"}, {"com.acme.test.**"});
    test::runWithDomSync(filter, index);

    EXPECT_FALSE(index.getClass("com/acme/Engine")->filteredOut);
    EXPECT_TRUE(index.getClass("com/acme/test/Helper")->filteredOut);
    EXPECT_TRUE(index.getClass("com/third/Widget")->filteredOut);
    EXPECT_EQ(filter.filteredCount(), 2u);
}

TEST(NativeClassFilterPreprocessorTest, LibraryClassesAreNeverFiltered)
{
    CompilerIndex index;
    buildSimpleClass(index, "com/third/Widget");
    index.getClass("com/third/Widget")->isLibrary = true;

    NativeClassFilterPreprocessor filter({}, {"com.third.**"});
    test::runWithDomSync(filter, index);

    EXPECT_FALSE(index.getClass("com/third/Widget")->filteredOut);
    EXPECT_EQ(filter.filteredCount(), 0u);
}

TEST(NativeClassFilterPreprocessorTest, MarkAllSkipsFilteredClasses)
{
    CompilerIndex index;
    buildSimpleClass(index, "com/acme/Engine");
    buildSimpleClass(index, "com/third/Widget");

    NativeClassFilterPreprocessor filter({"com.acme.**"}, {});
    test::runWithDomSync(filter, index);
    test::runWithDomSync(MarkAllMethodsPreprocessor(), index);

    EXPECT_TRUE(anyMarked(*index.getClass("com/acme/Engine")));
    EXPECT_FALSE(anyMarked(*index.getClass("com/third/Widget")));
}

TEST(NativeClassFilterPreprocessorTest, EmptyFilterMarksEverything)
{
    CompilerIndex index;
    buildSimpleClass(index, "com/acme/Engine");
    buildSimpleClass(index, "com/third/Widget");

    NativeClassFilterPreprocessor filter({}, {});
    test::runWithDomSync(filter, index);
    test::runWithDomSync(MarkAllMethodsPreprocessor(), index);

    EXPECT_TRUE(anyMarked(*index.getClass("com/acme/Engine")));
    EXPECT_TRUE(anyMarked(*index.getClass("com/third/Widget")));
    EXPECT_EQ(filter.filteredCount(), 0u);
}

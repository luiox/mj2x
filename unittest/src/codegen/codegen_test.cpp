// Unit tests for the codegen utilities shared by the C-emitting backends.

#include <gmock/gmock.h>

#include <stdexcept>
#include <string>

#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/code_snippet.hpp"
#include "mj2x/codegen/include_set.hpp"

using namespace mj2x::codegen;

namespace {

TEST(CSourceStreamTest, EmitLineAppliesCurrentIndent)
{
    CSourceStream s;
    s.emitLine("a");
    s.indent();
    s.emitLine("b");
    s.indent();
    s.emitLine("c");
    s.dedent();
    s.emitLine("d");
    EXPECT_EQ(s.str(),
              "a\n"
              "    b\n"
              "        c\n"
              "    d\n");
}

TEST(CSourceStreamTest, DedentClampsAtZero)
{
    CSourceStream s;
    s.dedent();   // no-op, never negative
    s.dedent();
    s.emitLine("x");
    EXPECT_EQ(s.str(), "x\n");
}

TEST(CSourceStreamTest, EmitRawPreservesContentVerbatim)
{
    CSourceStream s;
    s.emitRaw("extern \"C\" {\n");
    s.indent();
    s.emitLine("void f(void);");
    s.dedent();
    s.emitRaw("}\n");
    EXPECT_EQ(s.str(),
              "extern \"C\" {\n"
              "    void f(void);\n"
              "}\n");
}

TEST(CSourceStreamTest, EmitMultilineAppliesIndentToEachLine)
{
    // snippet 模板只保留块内相对缩进，首层缩进由 emitMultiline 按当前 indent 平移。
    CSourceStream s;
    s.emitLine("void f(void) {");
    s.indent();
    s.emitMultiline("if (cond) {\n    do_thing();\n}\n");
    s.dedent();
    s.emitLine("}");
    EXPECT_EQ(s.str(),
              "void f(void) {\n"
              "    if (cond) {\n"
              "        do_thing();\n"
              "    }\n"
              "}\n");
}

TEST(CSourceStreamTest, EmitMultilineKeepsBlankLineUnindented)
{
    // 空行保留为空行（不加缩进，避免行尾空格）。
    CSourceStream s;
    s.indent();
    s.emitMultiline("first\n\nthird\n");
    EXPECT_EQ(s.str(),
              "    first\n"
              "\n"
              "    third\n");
}

TEST(IncludeSetTest, DeduplicatesAndGroupsSystemBeforeProject)
{
    IncludeSet set;
    set.addSystem("jni.h");
    set.addProject("regc/abi.hpp");
    set.addSystem("stdint.h");
    set.addSystem("jni.h");           // duplicate
    set.addProject("regc/abi.hpp");   // duplicate
    EXPECT_FALSE(set.empty());
    EXPECT_EQ(set.render(),
              "#include <jni.h>\n"
              "#include <stdint.h>\n"
              "#include \"regc/abi.hpp\"\n");
}

TEST(IncludeSetTest, EmptySetRendersNothing)
{
    IncludeSet set;
    EXPECT_TRUE(set.empty());
    EXPECT_EQ(set.render(), "");
}

TEST(CHelpersTest, CStringLiteralEscapesSpecialBytes)
{
    EXPECT_EQ(cStringLiteral("plain"), "\"plain\"");
    EXPECT_EQ(cStringLiteral("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(cStringLiteral("line1\nline2"), "\"line1\\nline2\"");
    EXPECT_EQ(cStringLiteral("\t\r"), "\"\\t\\r\"");
    // Non-printable bytes must be octal-escaped (a C backslash escape reads as
    // octal); 0x01 -> \001, 0x9F -> \237.  Decimal would be wrong: \159 parses
    // as octal 159 = 0x6F, not 0x9F.
    std::string withCtrl = std::string("x") + char(0x01) + char(0x9F) + "y";
    EXPECT_EQ(cStringLiteral(withCtrl), "\"x\\001\\237y\"");
}

TEST(CHelpersTest, MangleNameEscapesUnderscoreAndSlashes)
{
    EXPECT_EQ(mangleName("java/lang/String", false), "java_lang_String");
    EXPECT_EQ(mangleName("a_b", false), "a_1b");
    EXPECT_EQ(mangleName("com.example.Pkg", false), "com_example_Pkg");
    // descriptor mode additionally escapes ; and [
    EXPECT_EQ(mangleName("Lpkg/Cls;", true), "Lpkg_Cls_2");
    EXPECT_EQ(mangleName("[I", true), "_3I");
    // '$'(内部类名 Outer$Inner) 按 JNI 规范转义为 _24（两种模式都生效）
    EXPECT_EQ(mangleName("com/example/Outer$Inner", false), "com_example_Outer_24Inner");
}

TEST(CHelpersTest, MangleMethodBuildsJavaPrefixSymbol)
{
    // Static add(II)I on com/example/Foo -> Java_com_example_Foo_add__II
    EXPECT_EQ(mangleMethod("com/example/Foo", "add", "(II)I"), "Java_com_example_Foo_add__II");
    // No-arg method has no __ suffix.
    EXPECT_EQ(mangleMethod("Foo", "bar", "()V"), "Java_Foo_bar");
    // <init> maps to init_ (legal C identifier).
    EXPECT_EQ(mangleMethod("Foo", "<init>", "()V"), "Java_Foo_init_");
}

enum class TestSnippet
{
    Assign,
};

TEST(CodeSnippetTest, RendersTypedSnippetVariables)
{
    CodeSnippetSet<TestSnippet> snippets({
        {TestSnippet::Assign, "${lhs} = (${type})${rhs};"},
    });
    EXPECT_EQ(snippets.render(TestSnippet::Assign,
                              {
                                  {"lhs", "stack[++sp].i"},
                                  {"type", "jint"},
                                  {"rhs", "7"},
                              }),
              "stack[++sp].i = (jint)7;");
}

TEST(CodeSnippetTest, ThrowsOnUnknownVariable)
{
    // 未定义变量属契约违背，尽早抛异常而非把 ${missing} 留在生成代码里。
    EXPECT_THROW(renderSnippet("${known} ${missing}", {{"known", "ok"}}), std::runtime_error);
}

}   // namespace

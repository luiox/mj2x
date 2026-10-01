// Unit tests for the template-engine core: CExpr builders, CodeOptions switches
// and CodeTemplate registry (doc_ai/regc_template_engine.md §3-4).
//
// Every composed expression/statement must pass the lexical balance scan
// (balanced_c.hpp) — this is the executable guard against hand-spliced brackets.

#include <gmock/gmock.h>

#include <string>

#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_options.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/c_template.hpp"
#include "codegen/balanced_c.hpp"

using namespace mj2x::codegen;
using namespace mj2x::codegen::test;

namespace {

// ---- CExpr: 表达式构建器 ---------------------------------------------------

TEST(CExprTest, CallBuildsCommaJoinedArgs)
{
    EXPECT_EQ(exprCall("f", {}), "f()");
    EXPECT_EQ(exprCall("f", {"a"}), "f(a)");
    EXPECT_EQ(exprCall("f", {"a", "b", "c"}), "f(a, b, c)");
}

TEST(CExprTest, CallArgsBuildsDynamicArity)
{
    EXPECT_EQ(exprCallArgs("f", {}), "f()");
    const std::vector<std::string> args = {"r0", "r1", "r2"};
    EXPECT_EQ(exprCallArgs("f", args), "f(r0, r1, r2)");
}

TEST(CExprTest, CastWrapsExprInParens)
{
    EXPECT_EQ(exprCast("int32_t", "x"), "(int32_t)x");
    EXPECT_EQ(exprCast("mj2x_regc_resolved_call_site_t*", "site"),
              "(mj2x_regc_resolved_call_site_t*)site");
}

TEST(CExprTest, FnPtrCallBuildsCastThenCall)
{
    EXPECT_EQ(exprFnPtrCall("void (*)(int32_t)", "fn", {"1"}),
              "((void (*)(int32_t))fn)(1)");
    EXPECT_EQ(exprFnPtrCall("int32_t (*)(int32_t, int32_t)", "fn", {"a", "b"}),
              "((int32_t (*)(int32_t, int32_t))fn)(a, b)");
}

TEST(CExprTest, MemberAndIndexAccess)
{
    EXPECT_EQ(exprMember("site", "fn"), "site.fn");
    EXPECT_EQ(exprMember("obj", "a.b"), "obj.a.b");
    EXPECT_EQ(exprIndex("arr", "0"), "arr[0]");
    EXPECT_EQ(exprIndex("table", "key"), "table[key]");
}

TEST(CExprTest, BinopKeepsCallerVisibleNoImplicitParens)
{
    EXPECT_EQ(exprBinop("+", "a", "b"), "a + b");
    EXPECT_EQ(exprBinop("==", "site.fn", "(void*)0"), "site.fn == (void*)0");
}

TEST(CExprTest, TernaryAndParenAndNull)
{
    EXPECT_EQ(exprTernary("c", "t", "e"), "c ? t : e");
    EXPECT_EQ(exprParen("a + b"), "(a + b)");
    EXPECT_EQ(exprNull(), "(void*)0");
}

TEST(CExprTest, NestedCompositionBalances)
{
    // ((sig)fn)(cast(a), site.member[0])
    const std::string composed =
        exprFnPtrCall("int32_t (*)(int32_t, int32_t)", "site.fn",
                      {exprCast("int32_t", "r0"),
                       exprIndex(exprMember("site", "args"), "0")});
    EXPECT_EQ(composed, "((int32_t (*)(int32_t, int32_t))site.fn)((int32_t)r0, site.args[0])");
    std::string reason;
    EXPECT_TRUE(isBalancedC(composed, &reason)) << reason;

    // (int32_t)(cond ? a : (b + c))
    const std::string deeper =
        exprCast("int32_t", exprTernary("cond", "a", exprParen(exprBinop("+", "b", "c"))));
    EXPECT_TRUE(isBalancedC(deeper, &reason)) << reason;
}

// ---- CodeOptions: 开关系统 -------------------------------------------------

TEST(CodeOptionsTest, DefaultsMatchRegCEmitterLegacy)
{
    CodeOptions opts;
    EXPECT_EQ(opts.commentLevel, RegCCommentLevel::Summary);
    EXPECT_FALSE(opts.traceCallLog);
    EXPECT_FALSE(opts.resolvedDispatch);
    EXPECT_FALSE(opts.directCalls);
}

TEST(CodeOptionsTest, FieldsAreIndependentlySettable)
{
    CodeOptions opts;
    opts.commentLevel   = RegCCommentLevel::Detailed;
    opts.traceCallLog   = true;
    opts.resolvedDispatch = true;
    EXPECT_EQ(opts.commentLevel, RegCCommentLevel::Detailed);
    EXPECT_TRUE(opts.traceCallLog);
    EXPECT_TRUE(opts.resolvedDispatch);
    EXPECT_FALSE(opts.directCalls);
}

TEST(CodeOptionsTest, ScopeOverrideRestoresOnScopeExit)
{
    CodeOptions opts;
    EXPECT_FALSE(opts.traceCallLog);
    {
        CodeOptions::ScopeOverride guard(opts.traceCallLog, true);
        EXPECT_TRUE(opts.traceCallLog);
        {
            CodeOptions::ScopeOverride nested(opts.directCalls, true);
            EXPECT_TRUE(opts.directCalls);
        }
        EXPECT_FALSE(opts.directCalls);          // 内层恢复
        EXPECT_TRUE(opts.traceCallLog);
    }
    EXPECT_FALSE(opts.traceCallLog);             // 外层恢复
    EXPECT_FALSE(opts.directCalls);
}

// ---- CodeTemplate: 命名模板 -------------------------------------------------

TEST(CodeTemplateTest, DefineAndEmit)
{
    const CodeTemplate tpl = CodeTemplate::define("edge_copy", [](CSourceStream& s) {
        s.comment("phi edge copy");
        s.goto_("block_b7");
    });
    EXPECT_EQ(std::string(tpl.name()), "edge_copy");
    EXPECT_TRUE(tpl);
    CSourceStream out;
    out.block([&](CSourceStream& u) { tpl.emit(u); });
    EXPECT_EQ(out.str(),
              "{\n"
              "    // phi edge copy\n"
              "    goto block_b7;\n"
              "}\n");
}

TEST(CodeTemplateTest, EmitCapturesBoundParametersByValue)
{
    const std::string label = "block_b9";
    const CodeTemplate tpl  = CodeTemplate::define("goto_label", [label](CSourceStream& s) {
        s.goto_(label);
    });
    CSourceStream out;
    tpl.emit(out);
    EXPECT_EQ(out.str(), "goto block_b9;\n");
}

TEST(CodeTemplateTest, DependsOnDeclaresOptions)
{
    const CodeTemplate tpl = CodeTemplate::define("call_log", [](CSourceStream&) {})
                                 .dependsOn("traceCallLog");
    ASSERT_EQ(tpl.dependencies().size(), 1u);
    EXPECT_EQ(tpl.dependencies()[0], "traceCallLog");
}

TEST(CodeTemplateRegistryTest, AddFindAndDedupe)
{
    CodeTemplateRegistry reg;
    EXPECT_TRUE(reg.add(CodeTemplate::define("a", [](CSourceStream&) {})));
    EXPECT_TRUE(reg.add(CodeTemplate::define("b", [](CSourceStream&) {})));
    EXPECT_FALSE(reg.add(CodeTemplate::define("a", [](CSourceStream&) {})));   // 去重
    EXPECT_EQ(reg.all().size(), 2u);
    EXPECT_NE(reg.find("a"), nullptr);
    EXPECT_NE(reg.find("b"), nullptr);
    EXPECT_EQ(reg.find("missing"), nullptr);
    EXPECT_FALSE(reg.empty());
}

// ---- CSourceStream 结构辅助补充（switch 族/标签/注释） ----------------------

TEST(CSourceStreamTest, SwitchCaseLabelsAndGoto)
{
    CSourceStream s;
    s.switch_("op", [&](CSourceStream& o) {
        o.case_("1", [](CSourceStream& u) {
            u.emitLine("result = a + b;");
            u.emitLine("break;");
        });
        o.case_("2", [](CSourceStream& u) {
            u.emitLine("result = a - b;");
            u.emitLine("break;");
        });
        o.emitLine("default: {");
        {
            o.indent();
            o.emitLine("result = 0;");
            o.dedent();
        }
        o.emitLine("}");
    });
    EXPECT_EQ(s.str(),
              "switch (op) {\n"
              "    case 1: {\n"
              "        result = a + b;\n"
              "        break;\n"
              "    }\n"
              "    case 2: {\n"
              "        result = a - b;\n"
              "        break;\n"
              "    }\n"
              "    default: {\n"
              "        result = 0;\n"
              "    }\n"
              "}\n");
    std::string reason;
    EXPECT_TRUE(isBalancedC(s.str(), &reason)) << reason;
}

TEST(CSourceStreamTest, LabelSitsAtColumnZero)
{
    CSourceStream s;
    s.emitLine("if (x) {");
    s.indent();
    s.label_("block_b3");
    s.goto_("block_b4");
    s.dedent();
    s.emitLine("}");
    EXPECT_EQ(s.str(),
              "if (x) {\n"
              "block_b3:\n"
              "    goto block_b4;\n"
              "}\n");
}

TEST(CSourceStreamTest, CommentAddsSlashSlashPrefixAtIndent)
{
    CSourceStream s;
    s.indent();
    s.comment("RegC method: com/x/Foo.compute(I)I");
    EXPECT_EQ(s.str(), "    // RegC method: com/x/Foo.compute(I)I\n");
}

TEST(CSourceStreamTest, BlockIffElseMixRemainsBalanced)
{
    CSourceStream s;
    s.iff("site.fn != NULL", [](CSourceStream& o) {
        o.block([&](CSourceStream& u) {
            u.emitLine("call();");
        });
    }, [](CSourceStream& o) {
        o.while_("p != NULL", [](CSourceStream& u) {
            u.emitLine("p = p->next;");
        });
    });
    std::string reason;
    EXPECT_TRUE(isBalancedC(s.str(), &reason)) << reason;
}

// ---- assertBalancedC 工具本身 ----------------------------------------------

TEST(BalancedCTest, SkipsStringsAndComments)
{
    EXPECT_TRUE(isBalancedC("x = \"a)b(c\";"));
    EXPECT_TRUE(isBalancedC("// (unbalanced } {"));
    EXPECT_TRUE(isBalancedC("/* ( { } ) */ f();"));
    EXPECT_TRUE(isBalancedC("char c = '}';"));
    EXPECT_TRUE(isBalancedC("str = \"\\\"escaped\\\\\" ;"));
}

TEST(BalancedCTest, DetectsMismatchAndUnclosed)
{
    std::string reason;
    EXPECT_FALSE(isBalancedC("if (x {", &reason));
    EXPECT_FALSE(isBalancedC("f(}", &reason));
    EXPECT_FALSE(isBalancedC("} else {", &reason));
    EXPECT_FALSE(isBalancedC("switch (x) {", &reason));
}

}   // namespace

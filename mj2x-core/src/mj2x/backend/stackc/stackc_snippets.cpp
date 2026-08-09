/// @file stackc_snippets.cpp
/// @brief StackC 生成模板片段表（snippet 渲染）。
///
/// @details
/// 从 stackc_backend.cpp 单体拆出：所有 `StackCSnippet` 模板统一收口在这里，
/// 与 codegen::CodeSnippetSet 渲染器配合。异常检查模板带 `_MJ2X_UNLIKELY`
/// 分支预测提示（等价 JNIC 3.7.0 的 `__builtin_expect`，MSVC 下退化为普通 if）。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

#include "mj2x/codegen/code_snippet.hpp"

namespace mj2x {

using codegen::CSourceStream;

static const codegen::CodeSnippetSet<StackCSnippet>& stackCSnippets()
{
    static const codegen::CodeSnippetSet<StackCSnippet> snippets({
        {StackCSnippet::PushStackValue, "    stack[++sp]${acc} = ${value};\n"},
        {StackCSnippet::LocalLoad, "    stack[++sp]${acc} = locals[${slot}]${acc};\n"},
        {StackCSnippet::LocalStore, "    locals[${slot}]${acc} = stack[sp--]${acc};\n"},
        {StackCSnippet::BinaryUnsignedWrap,
         "    ${lhs}${acc} = (${signed_type})((${unsigned_type})${lhs}${acc} ${op} "
         "(${unsigned_type})${rhs}${acc});\n"
         "    sp--;\n"},
        {StackCSnippet::ReturnVoid,
         "    return;\n"},
        {StackCSnippet::NullPointerTrap,
         "    if (_MJ2X_UNLIKELY(${ref} == NULL)) {\n"
         "        jclass ex_class = (jclass)${ex_class};\n"
         "        (*env)->ThrowNew(env, ex_class, \"null reference\");\n"
         "${dispatch}"
         "    }\n"},
        {StackCSnippet::PendingExceptionCheck,
         "${pad}if (_MJ2X_UNLIKELY((*env)->ExceptionCheck(env))) {\n"
         "${dispatch}"
         "${pad}}\n"},
        {StackCSnippet::ArrayLength,
         "    stack[sp].i = (*env)->GetArrayLength(env, stack[sp].l);\n"},
        {StackCSnippet::ObjectArrayLoad,
         "    stack[sp - 1].l = ${value};\n"
         "    sp--;\n"},
        {StackCSnippet::ObjectArrayStore,
         "    (*env)->SetObjectArrayElement(env, (jobjectArray)stack[sp - 2].l, "
         "stack[sp - 1].i, stack[sp].l);\n"
         "    sp -= 3;\n"},
        {StackCSnippet::PrimitiveArrayLoad,
         "    {\n"
         "        ${type} value;\n"
         "        (*env)->Get${suffix}ArrayRegion(env, (${array_type})stack[sp - 1].l, "
         "stack[sp].i, 1, &value);\n"
         "        stack[sp - 1]${acc} = (${target_type})value;\n"
         "    }\n"
         "    sp--;\n"},
        {StackCSnippet::PrimitiveArrayStore,
         "    {\n"
         "        ${type} value = (${type})stack[sp]${acc};\n"
         "        (*env)->Set${suffix}ArrayRegion(env, (${array_type})stack[sp - 2].l, "
         "stack[sp - 1].i, 1, &value);\n"
         "    }\n"
         "    sp -= 3;\n"},
    });
    return snippets;
}

std::string renderStackCSnippet(StackCSnippet key, const codegen::SnippetVars& vars)
{
    return stackCSnippets().render(key, vars);
}

void emitCLine(codegen::CSourceStream& out, int indentLevel, const std::string& line)
{
    CSourceStream src;
    for (int i = 0; i < indentLevel; ++i) src.indent();
    src.emitLine(line);
    out.append(src.str());
}

}   // namespace mj2x

/// @file c_expr.hpp
/// @brief C 表达式构建器：纯函数，括号内聚。
///
/// @details
/// 见 doc_ai/regc_template_engine.md §3.1。每个构建器是纯函数，返回 `std::string`
/// （短生命周期值对象，非拼接容器，不违反 codegen_style.md §3.3；产物文本仍只经
/// CSourceStream 入流）。括号内聚规则：调用方永远不需要手写 `"(" + ... + ")"`，
/// 嵌套调用结果已含必要括号。`exprBinop` 不隐式加括号，优先级由调用方用
/// `exprParen` 显式表达（生成代码可读性优先）。
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace mj2x::codegen {

/// @brief 函数调用：`fn(a, b, ...)`（静态数量参数）。
std::string exprCall(std::string_view fn, std::initializer_list<std::string_view> args);

/// @brief 函数调用：`fn(a, b, ...)`（动态数量参数，如寄存器列表）。
std::string exprCallArgs(std::string_view fn, const std::vector<std::string>& args);

/// @brief 类型强转：`(type)expr`。
std::string exprCast(std::string_view type, std::string_view expr);

/// @brief 函数指针调用：`((fnPtrSig)fnExpr)(a, b, ...)`。
std::string exprFnPtrCall(std::string_view fnPtrSig, std::string_view fnExpr,
                          std::initializer_list<std::string_view> args);

/// @brief 成员访问：`obj.member`。
std::string exprMember(std::string_view obj, std::string_view member);

/// @brief 下标访问：`arr[idx]`。
std::string exprIndex(std::string_view arr, std::string_view idx);

/// @brief 二元运算：`lhs op rhs`（不隐式加括号，优先级由调用方显式表达）。
std::string exprBinop(std::string_view op, std::string_view lhs, std::string_view rhs);

/// @brief 三元条件：`cond ? thenExpr : elseExpr`。
std::string exprTernary(std::string_view cond, std::string_view thenExpr,
                        std::string_view elseExpr);

/// @brief 括号包裹：`(expr)`（优先级歧义时显式表达）。
std::string exprParen(std::string_view expr);

/// @brief 统一空指针写法：`(void*)0`。
std::string exprNull();

// ---- 实现（header-only，纯函数） -------------------------------------------

inline std::string exprCall(std::string_view fn, std::initializer_list<std::string_view> args)
{
    std::string text(fn);
    text += "(";
    size_t index = 0;
    for (const auto& arg : args) {
        if (index++ != 0) text += ", ";
        text += arg;
    }
    text += ")";
    return text;
}

inline std::string exprCallArgs(std::string_view fn, const std::vector<std::string>& args)
{
    std::string text(fn);
    text += "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) text += ", ";
        text += args[i];
    }
    text += ")";
    return text;
}

inline std::string exprCast(std::string_view type, std::string_view expr)
{
    std::string text("(");
    text += type;
    text += ")";
    text += expr;
    return text;
}

inline std::string exprFnPtrCall(std::string_view fnPtrSig, std::string_view fnExpr,
                                 std::initializer_list<std::string_view> args)
{
    std::string text("((");
    text += fnPtrSig;
    text += ")";
    text += fnExpr;
    text += ")(";
    size_t index = 0;
    for (const auto& arg : args) {
        if (index++ != 0) text += ", ";
        text += arg;
    }
    text += ")";
    return text;
}

inline std::string exprMember(std::string_view obj, std::string_view member)
{
    std::string text(obj);
    text += ".";
    text += member;
    return text;
}

inline std::string exprIndex(std::string_view arr, std::string_view idx)
{
    std::string text(arr);
    text += "[";
    text += idx;
    text += "]";
    return text;
}

inline std::string exprBinop(std::string_view op, std::string_view lhs, std::string_view rhs)
{
    std::string text(lhs);
    text += " ";
    text += op;
    text += " ";
    text += rhs;
    return text;
}

inline std::string exprTernary(std::string_view cond, std::string_view thenExpr,
                               std::string_view elseExpr)
{
    std::string text(cond);
    text += " ? ";
    text += thenExpr;
    text += " : ";
    text += elseExpr;
    return text;
}

inline std::string exprParen(std::string_view expr)
{
    std::string text("(");
    text += expr;
    text += ")";
    return text;
}

inline std::string exprNull()
{
    return "(void*)0";
}

}   // namespace mj2x::codegen

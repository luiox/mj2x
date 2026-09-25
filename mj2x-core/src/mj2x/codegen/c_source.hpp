/// @file c_source.hpp
/// @brief Indent-aware C source stream for backends that emit C/C++.
///
/// @details
/// Generated C translation units used to be assembled by writing into a raw
/// `std::ostringstream` with hand-counted leading spaces (`out << "    ..."`).
/// That made indentation easy to drift, blocks hard to follow, and diffs noisy.
///
/// `CSourceStream` owns the current indent level so emitters write logical
/// content (`emitLine`) instead of literal whitespace.  Raw fragments that must
/// not be re-indented (e.g. an externally formatted block) still have an escape
/// hatch via `emitRaw`.  The stream is a drop-in sink: callers flush it to a
/// string when the translation unit is complete.
///
/// 三层模型（mj2x/doc_ai/codegen_style.md §3）的排版层。底层存储是
/// `ca::str::Utf8StringBuilder`，与拼接层（Utf8StringBuilder）/驻留层
/// （Utf8StringArena）同属 Utf8 家族：emitter 的多片段 `append` 拼一条 C 语句的
/// 模式可无缝迁移到这里，无需跨容器转换。
#pragma once

#include "libca/str/format.hpp"
#include "libca/str/utf8_string.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mj2x::codegen {

class CSourceStream;

/// @brief C 语句体 lambda：接收流引用，块内缩进由 CSourceStream 自动管理。
/// @details 用 `iff`/`block`/`add_func` 的 lambda 重载生成嵌套块时，花括号、
///          缩进与换行全部由流负责，调用方只写语句内容，杜绝手拼括号漂移。
using CBody = std::function<void(CSourceStream&)>;

/// @brief 前向声明注册表：add_func 定义的函数在此登记，render 输出到文件头部。
/// @details 生成器用 add_func 定义函数时自动登记声明；调用点出现在定义之前
///          （如帮手函数被更早的方法体引用）时，把注册表渲染到翻译单元头部
///          即可，无需手工维护前向声明列表。重复登记幂等。
class CForwardDeclarations
{
public:
    /// @brief 登记一条前向声明文本（如 `static int32_t foo(int32_t x);`）。
    void declare(const std::string& declaration)
    {
        if (seen_.insert(declaration).second) {
            decls_.push_back(declaration);
        }
    }

    /// @brief 渲染全部已登记声明（每条一行，末尾换行）。
    std::string render() const
    {
        std::string text;
        for (const auto& decl : decls_) {
            text += decl;
            text += "\n";
        }
        return text;
    }

    bool empty() const { return decls_.empty(); }

private:
    std::vector<std::string> decls_;
    std::set<std::string>    seen_;
};

/// @brief add_func 的结果：函数名 + 完整签名行（不含 `{`），供调用点引用。
struct CFuncRef
{
    std::string name;
    std::string signature;
};

/// @brief Indent-aware sink for generated C/C++ source text.
class CSourceStream
{
public:
    CSourceStream() = default;

    // ---- 行级发射（带缩进管理） ------------------------------------------

    /// @brief Append one logical line at the current indent, terminated by `\n`.
    /// @details 等价于 `appendLineStart(line); newLine()`。适合「一调用一行」的
    ///          简单语句；多片段拼一条语句时改用 `appendLineStart` + `append`。
    void emitLine(std::string_view line)
    {
        writeIndent();
        appendStr(line);
        out_.append("\n");
    }

    /// @brief Append text verbatim with no indent and no trailing newline.
    /// @details Use for pre-formatted blocks or fragments where the caller
    ///          controls all whitespace (e.g. raw-string runtime templates).
    void emitRaw(std::string_view text) { appendStr(text); }

    /// @brief 按当前缩进逐行发射多行文本。
    /// @details 将 text 按 '\n' 拆分：每个非空行写入「当前缩进 + 行内容 + 换行」，
    ///          空行只写换行（保留空行但不加缩进，避免行尾空格）。用于把 snippet
    ///          渲染结果接入 CSourceStream 的缩进管理——snippet 模板只需保留块内
    ///          相对缩进，首层缩进由本方法按当前 indent 自动平移，调用点处于任意
    ///          缩进层都能正确排版（替代旧式「模板自带 4 空格 + out.append」）。
    void emitMultiline(std::string_view text)
    {
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t end     = text.find('\n', pos);
            const size_t lineLen = (end == std::string_view::npos) ? (text.size() - pos) : (end - pos);
            std::string_view line(text.data() + pos, lineLen);
            if (!line.empty()) {
                writeIndent();
                appendStr(line);
            }
            out_.append("\n");
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
    }

    /// @brief 在当前缩进处开始一条逻辑行，不追加换行。
    /// @details 写入当前缩进后返回，调用方继续用 `append` 拼该行剩余片段，
    ///          最后用 `newLine()` 收尾。与 Utf8StringBuilder 多片段拼接模式对接：
    ///          `src.appendLineStart("    ").append(...)` 的迁移目标。
    ///          返回 `*this` 以支持链式拼接（`appendLineStart(...).append(...)`）。
    CSourceStream& appendLineStart(std::string_view first = {})
    {
        writeIndent();
        if (!first.empty()) appendStr(first);
        return *this;
    }

    /// @brief 写入一个换行，结束当前逻辑行。
    void newLine() { out_.append("\n"); }

    /// @brief 写入一个带缩进和换行的空行。
    void emitBlankLine() { out_.append("\n"); }

    /// @brief Increase the indent level by one step (four spaces per step).
    void indent() { ++indentLevel_; }

    /// @brief Decrease the indent level, clamped at zero.
    void dedent()
    {
        if (indentLevel_ > 0) --indentLevel_;
    }

    /// @brief RAII 缩进守卫：构造 indent，析构 dedent，保证嵌套块成对。
    struct IndentGuard
    {
        explicit IndentGuard(CSourceStream& s) : stream(s) { stream.indent(); }
        ~IndentGuard() { stream.dedent(); }
        CSourceStream& stream;
    };

    // ---- 块级结构辅助（lambda 驱动，自动缩进/花括号/换行） ----------------
    //
    // 嵌套控制流用 `iff`/`block` 表达，花括号与缩进由流统一管理，避免手工
    // 拼接 `{`/`}` 与缩进漂移。语句内容仍可用 `emitLine` 或 `appendLineStart`
    // 多片段拼接（两者混用安全）。

    /// @brief 发射 `{ ... }` 语句块。
    void block(const CBody& body)
    {
        emitLine("{");
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief 发射 `if (cond) { then }`，可选 `else { else }`。
    /// @param condition C 条件表达式文本（不含外层括号）。
    /// @param elseBody  为空（默认）时省略 else 分支。
    void iff(std::string_view condition, const CBody& thenBody, const CBody& elseBody = CBody())
    {
        emitLine(ca::str::format_std("if ({}) {{", condition));
        {
            IndentGuard g(*this);
            thenBody(*this);
        }
        if (elseBody) {
            emitLine("} else {");
            {
                IndentGuard g(*this);
                elseBody(*this);
            }
        }
        emitLine("}");
    }

    /// @brief 发射一个函数/语句块定义；signature 为完整签名行（不含 `{`）。
    /// @example add_func("static int32_t foo(int32_t x)", [](CSourceStream& s){ ... });
    void add_func(std::string_view signature, const CBody& body)
    {
        emitLine(signature);
        emitLine("{");
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief 发射 `returnType name(argType argName, ...)` 风格的函数定义。
    /// @details 参数为 (类型, 名) 列表，逐个拼进签名行；body 写函数体语句。
    void add_func(std::string_view returnType, std::string_view name,
                  std::initializer_list<std::pair<std::string_view, std::string_view>> args,
                  const CBody& body)
    {
        std::string signature(returnType);
        signature += " ";
        signature += name;
        signature += "(";
        size_t index = 0;
        for (const auto& [type, arg] : args) {
            if (index++ != 0) signature += ", ";
            signature += type;
            signature += " ";
            signature += arg;
        }
        signature += ")";
        add_func(signature, body);
    }

    /// @brief 发射函数定义并登记前向声明（挂注册表时）。
    /// @details 返回的 CFuncRef 携带函数名与签名行；调用点出现在定义之前时，
    ///          把注册表 render 到翻译单元头部即得前向声明。
    CFuncRef add_func_declared(std::string_view signature, const CBody& body)
    {
        if (fwd_ != nullptr) fwd_->declare(std::string(signature) + ";");
        add_func(signature, body);
        CFuncRef ref;
        ref.signature = std::string(signature);
        ref.name      = functionNameFromSignature(ref.signature);
        return ref;
    }

    /// @brief 发射函数定义（returnType name(args) 形式）并登记前向声明。
    CFuncRef add_func_declared(
        std::string_view returnType, std::string_view name,
        std::initializer_list<std::pair<std::string_view, std::string_view>> args,
        const CBody& body)
    {
        std::string signature(returnType);
        signature += " ";
        signature += name;
        signature += "(";
        size_t index = 0;
        for (const auto& [type, arg] : args) {
            if (index++ != 0) signature += ", ";
            signature += type;
            signature += " ";
            signature += arg;
        }
        signature += ")";
        return add_func_declared(signature, body);
    }

    /// @brief 挂接前向声明注册表（通常由翻译单元构建方设置）。
    void setForwardDeclarations(CForwardDeclarations* fwd) { fwd_ = fwd; }

    // ---- 常用语句辅助（单行语句，自动缩进） --------------------------------

    /// @brief 赋值语句：`dest = expr;`。
    void assign(std::string_view dest, std::string_view expr)
    {
        emitLine(ca::str::format_std("{} = {};", dest, expr));
    }

    /// @brief 变量声明：`type name;` 或 `type name = init;`。
    void declare_var(std::string_view type, std::string_view name,
                     std::string_view init = {})
    {
        if (init.empty()) {
            emitLine(ca::str::format_std("{} {};", type, name));
        }
        else {
            emitLine(ca::str::format_std("{} {} = {};", type, name, init));
        }
    }

    /// @brief return 语句：`return;` 或 `return expr;`。
    void ret(std::string_view expr = {})
    {
        if (expr.empty()) {
            emitLine("return;");
        }
        else {
            emitLine(ca::str::format_std("return {};", expr));
        }
    }

    /// @brief while 循环：`while (cond) { body }`。
    void while_(std::string_view cond, const CBody& body)
    {
        emitLine(ca::str::format_std("while ({}) {{", cond));
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief for 循环：`for (header) { body }`，header 不含括号。
    void for_(std::string_view header, const CBody& body)
    {
        emitLine(ca::str::format_std("for ({}) {{", header));
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief switch 语句：`switch (expr) { ... }`，expr 不含外层括号。
    /// @details case 分支体用 `case_` 或 `block` 发出；`break`/`goto` 由分支体内显式发出。
    void switch_(std::string_view expr, const CBody& body)
    {
        emitLine(ca::str::format_std("switch ({}) {{", expr));
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief case 分支：`case <label>: { ... }`；label 为 `"default"` 时输出 `default: { ... }`。
    /// @details 分支体自动加花括号并缩进；`break` 语句由 body 内 `emitLine("break;")`
    ///          显式发出（需要 fallthrough 时天然省略）。
    void case_(std::string_view label, const CBody& body)
    {
        if (label == "default") {
            emitLine("default: {");
        }
        else {
            emitLine(ca::str::format_std("case {}: {{", label));
        }
        {
            IndentGuard g(*this);
            body(*this);
        }
        emitLine("}");
    }

    /// @brief goto 语句：`goto <label>;`。
    void goto_(std::string_view label)
    {
        emitLine(ca::str::format_std("goto {};", label));
    }

    /// @brief 标签：`<name>:`。
    /// @details 标签不缩进（C 惯例贴左）；emitFunction 的 block_b<n> 标签用本方法发出。
    void label_(std::string_view name)
    {
        const int saved = indentLevel_;
        indentLevel_    = 0;
        emitLine(ca::str::format_std("{}:", name));
        indentLevel_ = saved;
    }

    /// @brief 注释行：`// <text>`（缩进随上下文）。
    /// @details 内容不经转义，调用方负责 escapeCommentIdent（混淆符号安全）。
    void comment(std::string_view text)
    {
        emitLine(ca::str::format_std("// {}", text));
    }

    /// @brief 调用语句：`name(args);`。
    void call(std::string_view name, std::initializer_list<std::string_view> args)
    {
        std::string text(name);
        text += "(";
        size_t index = 0;
        for (const auto& arg : args) {
            if (index++ != 0) text += ", ";
            text += arg;
        }
        text += ");";
        emitLine(text);
    }

    /// @brief 函数指针调用语句：`((sig)expr)(args);`。
    void call_fn_ptr(std::string_view fnPointerSig, std::string_view expr,
                     std::initializer_list<std::string_view> args)
    {
        std::string text("((");
        text += fnPointerSig;
        text += ")";
        text += expr;
        text += ")(";
        size_t index = 0;
        for (const auto& arg : args) {
            if (index++ != 0) text += ", ";
            text += arg;
        }
        text += ");";
        emitLine(text);
    }

    /// @brief 发射一条普通语句（`emitLine` 的 std::string 便捷重载）。
    void emitLine(const std::string& line)
    {
        writeIndent();
        appendStr(line);
        out_.append("\n");
    }
    /// @brief 发射一条普通语句（`emitLine` 的 const char* 便捷重载）。
    void emitLine(const char* line)
    {
        writeIndent();
        appendStr(line);
        out_.append("\n");
    }

    // ---- 行内续接（不加缩进、不加换行） ----------------------------------
    //
    // 签名与 ca::str::Utf8StringBuilder 的 append 重载对齐：emitter 把
    // out.append(operandExpr(...)) 这类调用从 Utf8StringBuilder 平移到
    // CSourceStream 时，仅需把 out 换类型，调用点不变。

    CSourceStream& append(const ca::str::Utf8StringRef& str)
    {
        out_.append(str);
        return *this;
    }
    CSourceStream& append(const ca::str::Utf8String& str)
    {
        out_.append(str);
        return *this;
    }
    CSourceStream& append(const char* cstr)
    {
        out_.append(cstr);
        return *this;
    }
    CSourceStream& append(const char* data, ca::usize byte_length)
    {
        out_.append(data, byte_length);
        return *this;
    }
    CSourceStream& append(const ca::u8* data, ca::usize byte_length)
    {
        out_.append(data, byte_length);
        return *this;
    }
    /// @brief 追加 std::string_view（兼容 std::string / 字面量片段）。
    CSourceStream& append(std::string_view sv)
    {
        out_.append(sv.data(), sv.size());
        return *this;
    }
    /// @brief 追加一个整数的十进制文本（替代 `append(std::to_string(x))`）。
    CSourceStream& appendInt(ca::i64 value)
    {
        const std::string digits = std::to_string(value);
        out_.append(digits.data(), digits.size());
        return *this;
    }

    /// @brief 底层 builder 直连（迁移期渐进迁移用）。
    /// @details 当一个 emit 函数尚未整体迁移时，调用方可经此拿到 Utf8StringBuilder
    ///          继续用旧片段拼接；全量迁移完成后此访问器可移除。
    ca::str::Utf8StringBuilder& sink() { return out_; }

    /// @brief Render the accumulated source as a single string（交付边界）。
    std::string str() const { return out_.build().to_std_string(); }

    /// @brief 取出底层积累的 Utf8String（用于把片段内联回表达式）。
    /// @details 局部构建器场景（构造一个表达式/标识符片段后内联到更大的流）用本方法，
    ///          避免经 std::string 往返。与 Utf8StringBuilder::build() 语义一致。
    ca::str::Utf8String build() const { return out_.build(); }

    /// @brief Current indent level (number of four-space steps).
    int indentLevel() const { return indentLevel_; }

private:
    void writeIndent()
    {
        for (int i = 0; i < indentLevel_; ++i) out_.append("    ");
    }
    void appendStr(std::string_view sv) { out_.append(sv.data(), sv.size()); }

    /// 从签名行提取函数名：取 '(' 前最后一段标识符。
    static std::string functionNameFromSignature(const std::string& signature)
    {
        const size_t paren   = signature.find('(');
        const std::string prefix =
            paren == std::string::npos ? signature : signature.substr(0, paren);
        const size_t space = prefix.find_last_of(" *");
        return space == std::string::npos ? prefix : prefix.substr(space + 1);
    }

    ca::str::Utf8StringBuilder out_;
    int                        indentLevel_ = 0;
    CForwardDeclarations*      fwd_         = nullptr;
};

}   // namespace mj2x::codegen

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
/// 三层模型（doc_ai/reg_codegen_syntax.md §3）的排版层。底层存储是
/// `ca::str::Utf8StringBuilder`，与拼接层（Utf8StringBuilder）/驻留层
/// （Utf8StringArena）同属 Utf8 家族：emitter 的多片段 `append` 拼一条 C 语句的
/// 模式可无缝迁移到这里，无需跨容器转换。
#pragma once

#include "libca/str/utf8_string.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace mj2x::codegen {

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

    ca::str::Utf8StringBuilder out_;
    int                        indentLevel_ = 0;
};

}   // namespace mj2x::codegen

// Test helper: lexical bracket-balance scan for generated C/C++ text.
//
// Guards the "structure as data" invariant of the template engine: any text the
// emitters produce (and any text composed with the CExpr builders) must pass
// this scan. Strings, char literals, and both comment styles are skipped so
// brackets inside them never count.

#pragma once

#include <string>
#include <string_view>

namespace mj2x::codegen::test {

/// @brief Lexical scan asserting `() {} []` are balanced and well-nested.
///
/// @details Skips `"..."` string literals (with escapes), `'x'` char literals,
///          `//` line comments and `/* */` block comments. Returns false and
///          fills `reason` (when non-null) with the offending bracket and byte
///          offset on the first mismatch.
inline bool isBalancedC(const std::string& text, std::string* reason = nullptr)
{
    std::string stack;
    const size_t n = text.size();

    auto fail = [&](const char* what, size_t offset) {
        if (reason != nullptr) {
            *reason = std::string(what) + " at byte " + std::to_string(offset);
        }
        return false;
    };

    for (size_t i = 0; i < n; ++i) {
        const char c = text[i];
        switch (c) {
            case '/':
                if (i + 1 < n && text[i + 1] == '/') {
                    while (i < n && text[i] != '\n') ++i;
                    if (i < n) --i;   // let the loop consume the newline
                }
                else if (i + 1 < n && text[i + 1] == '*') {
                    i += 2;
                    bool closed = false;
                    while (i + 1 < n) {
                        if (text[i] == '*' && text[i + 1] == '/') {
                            closed = true;
                            ++i;
                            break;
                        }
                        ++i;
                    }
                    if (!closed) return fail("unterminated /* comment", i);
                }
                break;
            case '"':
                ++i;
                while (i < n) {
                    if (text[i] == '\\') {
                        i += 2;
                        continue;
                    }
                    if (text[i] == '"') break;
                    if (text[i] == '\n') return fail("unterminated string literal", i);
                    ++i;
                }
                if (i >= n) return fail("unterminated string literal", n);
                break;
            case '\'':
                ++i;
                while (i < n) {
                    if (text[i] == '\\') {
                        i += 2;
                        continue;
                    }
                    if (text[i] == '\'') break;
                    if (text[i] == '\n') return fail("unterminated char literal", i);
                    ++i;
                }
                if (i >= n) return fail("unterminated char literal", n);
                break;
            case '(':
            case '[':
            case '{':
                stack.push_back(c);
                break;
            case ')':
            case ']':
            case '}':
                if (stack.empty()) return fail("unmatched closing bracket", i);
                {
                    const char open = stack.back();
                    const bool ok   = (c == ')' && open == '(') || (c == ']' && open == '[') ||
                                    (c == '}' && open == '{');
                    if (!ok) return fail("mismatched closing bracket", i);
                    stack.pop_back();
                }
                break;
            default: break;
        }
    }

    if (!stack.empty()) return fail("unclosed bracket", n);
    return true;
}

}   // namespace mj2x::codegen::test

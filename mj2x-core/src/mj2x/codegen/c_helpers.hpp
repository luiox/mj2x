/// @file c_helpers.hpp
/// @brief Stateless helpers shared by the C/C++ emitting backends.
///
/// @details
/// StackC and RegC each used to carry their own copy of the same string
/// helpers: C string escaping, JNI symbol mangling, type-display maps.  That
/// meant bug fixes had to land in two places and the two backends slowly
/// drifted.  This header is the single home for those stateless helpers so any
/// new C-emitting backend can reuse them.
///
/// Everything here is header-only `inline` because the helpers are tiny and
/// stateless; pulling them into a .cpp would only add a build unit for no
/// isolation benefit.
#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>

namespace mj2x::codegen {

/// @brief Render a byte string as a quoted C string literal.
/// @details Escapes backslash, quote, newline, carriage return, tab; emits
///          non-printable bytes as 3-digit octal (C string-literal grammar
///          reads a backslash escape as octal, so the digits must already be
///          octal, not decimal).  Matches the exact escaping StackC has
///          always used; RegC now shares it.
inline std::string cStringLiteral(const std::string& text)
{
    std::ostringstream os;
    os << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '\\': os << "\\\\"; break;
        case '"': os << "\\\""; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            if (std::isprint(c)) {
                os << static_cast<char>(c);
            }
            else {
                os << '\\' << static_cast<char>('0' + ((c >> 6) & 7))
                   << static_cast<char>('0' + ((c >> 3) & 7)) << static_cast<char>('0' + (c & 7));
            }
            break;
        }
    }
    os << '"';
    return os.str();
}

/// @brief Apply JNI `Java_<class>_<method>` name mangling to one segment.
/// @param name The class or method name component.
/// @param descriptorMode When true, also escape `;` and `[` (used for the
///                       overloaded-argument suffix appended to method symbols).
inline std::string mangleName(std::string_view name, bool descriptorMode)
{
    std::string result;
    result.reserve(name.size());
    for (unsigned char uc : name) {
        char c = static_cast<char>(uc);
        if (c == '/' || c == '.') {
            result += '_';
        }
        else if (c == '_') {
            result += "_1";
        }
        else if (descriptorMode && c == ';') {
            result += "_2";
        }
        else if (descriptorMode && c == '[') {
            result += "_3";
        }
        else {
            result += c;
        }
    }
    return result;
}

/// @brief Build the full `Java_<class>_<method>__<args>` JNI export symbol.
/// @details `<init>` / `<clinit>` are mapped to `init_` / `clinit_` so the
///          exported name stays a legal C identifier even though the JVM will
///          not auto-bind constructors through it.
inline std::string mangleMethod(std::string_view className, std::string_view methodName,
                                std::string_view methodDesc)
{
    std::string result = "Java_";
    result += mangleName(className, false);
    result += "_";
    if (methodName == "<init>") {
        result += "init_";
    }
    else if (methodName == "<clinit>") {
        result += "clinit_";
    }
    else {
        result += mangleName(methodName, false);
    }

    auto paren = methodDesc.find(')');
    if (paren != std::string_view::npos && paren > 1) {
        result += "__";
        result += mangleName(methodDesc.substr(1, paren - 1), true);
    }
    return result;
}

}   // namespace mj2x::codegen

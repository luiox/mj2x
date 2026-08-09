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

#include "libca/str/utf8_string.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>

namespace mj2x::codegen {

using ca::u32;
using ca::u8;
using ca::usize;

/// @brief Escape arbitrary JVM symbol bytes into ASCII-safe text for comments/dumps.
/// @details Obfuscated or re-native samples can carry any byte in class/method/field
///          names and descriptors.  Generated comments must stay textually intact, so
///          printable ASCII passes through, everything else is UTF-8-decoded and
///          emitted as `\uXXXX` (BMP) / `\UXXXXXXXX` (supplementary); malformed bytes
///          degrade to a single-byte `\uXX`.  Output is always valid ASCII UTF-8.
inline ca::str::Utf8String escapeCommentIdent(const ca::str::Utf8StringRef& raw)
{
    ca::str::Utf8StringBuilder b;
    const u8*                  data = raw.data();
    const usize                end  = raw.byte_length();
    usize                      i    = 0;
    while (i < end) {
        const u8 lead = data[i];
        if (lead < 0x80) {
            if (lead >= 0x20 && lead != 0x7F) {
                b.append(reinterpret_cast<const char*>(&lead), 1);
            }
            else {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned>(lead));
                b.append(esc);
            }
            ++i;
            continue;
        }
        u32   cp  = 0;
        usize len = 0;
        if ((lead & 0xE0) == 0xC0 && i + 1 < end && (data[i + 1] & 0xC0) == 0x80) {
            cp  = static_cast<u32>(lead & 0x1F) << 6 | (data[i + 1] & 0x3F);
            len = 2;
        }
        else if ((lead & 0xF0) == 0xE0 && i + 2 < end && (data[i + 1] & 0xC0) == 0x80 &&
                 (data[i + 2] & 0xC0) == 0x80) {
            cp  = static_cast<u32>(lead & 0x0F) << 12 |
                  static_cast<u32>(data[i + 1] & 0x3F) << 6 | (data[i + 2] & 0x3F);
            len = 3;
        }
        else if ((lead & 0xF8) == 0xF0 && i + 3 < end && (data[i + 1] & 0xC0) == 0x80 &&
                 (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
            cp  = static_cast<u32>(lead & 0x07) << 18 |
                  static_cast<u32>(data[i + 1] & 0x3F) << 12 |
                  static_cast<u32>(data[i + 2] & 0x3F) << 6 | (data[i + 3] & 0x3F);
            len = 4;
        }
        else {
            // Malformed byte: escape the raw byte itself.
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned>(lead));
            b.append(esc);
            ++i;
            continue;
        }
        char esc[16];
        if (cp <= 0xFFFF) {
            snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned>(cp));
        }
        else {
            snprintf(esc, sizeof(esc), "\\U%08X", static_cast<unsigned>(cp));
        }
        b.append(esc);
        i += len;
    }
    return b.build();
}

/// @brief Render a byte string as a quoted C string literal.
/// @details Escapes backslash, quote, newline, carriage return, tab; emits
///          non-printable bytes as 3-digit octal (C string-literal grammar
///          reads a backslash escape as octal, so the digits must already be
///          octal, not decimal).  Matches the exact escaping StackC has
///          always used; RegC now shares it.
inline std::string cStringLiteral(std::string_view text)
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
        else if (c == '$') {
            // JNI 规范：'$'(U+0024) 转义为 "_24"。内部类名（Outer$Inner）必须转义，
            // 否则按 Java_* 符号查找时会失败。此转义对基于符号绑定的消费者是规范正确修复。
            result += "_24";
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

/// @brief FNV-1a 64-bit hash（StackC/RegC 共用的不透明函数名/缓存标识符基础）。
inline std::uint64_t fnv1a64(std::string_view value, std::uint64_t seed = 1469598103934665603ULL)
{
    std::uint64_t hash = seed;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// @brief 64-bit 值 → 定宽 16 位十六进制文本。
inline std::string hex64(std::uint64_t value)
{
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

/// @brief Native 方法的不透明 C 函数名：`__mj2x_c_<fnv1a64>`。
/// @details 与 JNI `Java_*` 符号查找脱钩，靠 RegisterNatives 表动态绑定；
///          StackC 与 RegC 共用同一命名，避免两类生成库交叉引用时命名分裂。
inline std::string nativeFunctionName(std::string_view className, std::string_view methodName,
                                      std::string_view methodDesc)
{
    auto hash = fnv1a64(className);
    hash      = fnv1a64(methodName, hash);
    hash      = fnv1a64(methodDesc, hash);
    return "__mj2x_c_" + hex64(hash);
}

/// @brief 每类的 register 函数名：类名非字母数字转义为 `_XX`（末两位 hex）。
inline std::string registerFunctionName(std::string_view className)
{
    std::string ident = "_mj2x_register_";
    for (unsigned char c : className) {
        if (std::isalnum(c)) {
            ident += static_cast<char>(c);
        }
        else {
            ident += "_";
            ident += hex64(c).substr(14);
        }
    }
    return ident;
}

/// @brief 浮点字面量文本（`%.*g`，与 `std::setprecision(P) << double` 的默认
///        格式规则一致：P-1 位有效数字，指数落在 [-4, P) 内用定点否则科学计数）。
/// @details 发射器统一用它，不依赖 ostringstream/iomanip。NaN/Inf 由调用方
///          走宏分支（NAN/INFINITY），这里不处理。
inline std::string formatFloatLiteral(double value, int precision)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    return std::string(buffer);
}

}   // namespace mj2x::codegen

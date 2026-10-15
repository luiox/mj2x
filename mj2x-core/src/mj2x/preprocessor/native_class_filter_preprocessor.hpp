#pragma once

// native_class_filter_preprocessor.hpp
//
// 类级 glob include/exclude 过滤器（对标 jvmtp `filter.include` /
// `filter.exclude`，见 doc_ai/jvmtp_gap_analysis.md §K P1-3）。
//
// 语义（与 jvmtp 对齐）：
//   - 按内部类名匹配（CompilerIndex 键为 JVM 内部形态 `demo/CliJarA`；匹配前
//     文本与模式两侧都把 `/` 归一化为 `.`，因此 `com/example/**` 可直接粘贴）。
//   - `*`  匹配任意字符但不跨段（不吞 `.`）；
//     `**` 匹配任意字符含跨段；
//     `?`  匹配单个非 `.` 字符。
//   - include 非空时，类必须命中至少一个 include 模式才进入编译集；
//     include 为空时视为全部放行（再受 exclude 约束）。
//   - exclude 优先于 include：命中任一 exclude 模式的类一律剔除。
//
// 过滤器只打 `CompilerClass::filteredOut` 标志，不删类——JAR 往返仍需保留
// 原始条目，且其他类可能引用被剔除的类。方法级选择（annotation / method
// prefix / mark-all）与 <init>/<clinit> 搬运在各自入口跳过 filteredOut 类。

#include "mj2x/preprocessor/preprocessor.hpp"
#include "libca/log/log_macros.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mj2x {

/// @brief 段感知 glob 匹配（`*` 不跨 `.`，`**` 跨段，`?` 单字符不跨 `.`）。
///
/// 模式需已归一化（`/` → `.`）；文本为内部类名（`.` 分隔）。实现为
/// O(n*m) DP——类名与模式均为短串，确定性且无正则依赖。
bool globMatchClassName(const std::string& text, const std::string& pattern);

/// @brief 归一化一串 glob：按 `,` 拆分、去空白、`/` → `.`。
/// 空片段（如 `"a,,b"` / 纯空白 / 空串）视为非法，返回 false。
bool normalizeClassGlobs(const std::vector<std::string>& raw, std::vector<std::string>& out);

/// @brief 类是否通过 include/exclude 门（exclude 优先；模式容忍 `/` 未归一化形态）。
bool classPassesFilter(const std::string&        className,
                       const std::vector<std::string>& include,
                       const std::vector<std::string>& exclude);

/// @brief 类级过滤器 preprocessor：对不通过门的类置 `filteredOut`。
///
/// 必须在方法级选择 preprocessor（annotation / method prefix / mark-all）
/// 与 InitClinitMoverPreprocessor **之前**注册——后者按该标志跳过。
class NativeClassFilterPreprocessor : public Preprocessor
{
public:
    NativeClassFilterPreprocessor(std::vector<std::string> include, std::vector<std::string> exclude)
        : include_(std::move(include)), exclude_(std::move(exclude))
    {}

    void process(CompilerIndex& index) override
    {
        std::size_t total = 0;
        std::size_t kept  = 0;
        for (auto& [name, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            ++total;
            if (classPassesFilter(name, include_, exclude_)) {
                ++kept;
                continue;
            }
            cls.filteredOut = true;
        }
        filtered_ = total - kept;
        CA_LOGT_INFO("preprocess", "Class filter: kept {}, filtered {} class(es)\n", kept, filtered_);
        if (!include_.empty() && kept == 0 && total > 0) {
            // include 非空却一个都没中：模式拼错是最常见原因，值得一条警告。
            CA_LOGT_WARN("preprocess",
                         "Class filter: --native-include matched 0 of {} class(es); "
                         "no method will be compiled\n",
                         total);
        }
    }

    /// @brief 本轮被剔除的类数（日志/测试用）。
    std::size_t filteredCount() const { return filtered_; }

private:
    std::vector<std::string> include_;
    std::vector<std::string> exclude_;
    std::size_t              filtered_ = 0;
};

// ---------------------------------------------------------------------------
// 实现（header-only，供 mj2x-core 与 unittest 直接包含）
// ---------------------------------------------------------------------------

inline bool globMatchClassName(const std::string& text, const std::string& pattern)
{
    // 模式切 token：连续两个 '*' 合成一个跨段 token（"***" = "**" + "*"）。
    enum Kind { Lit, Any, Star, DoubleStar };
    std::vector<std::pair<Kind, char>> tokens;
    tokens.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                tokens.emplace_back(DoubleStar, '\0');
                i += 2;
                continue;
            }
            tokens.emplace_back(Star, '\0');
            ++i;
            continue;
        }
        if (pattern[i] == '?') {
            tokens.emplace_back(Any, '\0');
            ++i;
            continue;
        }
        tokens.emplace_back(Lit, pattern[i]);
        ++i;
    }

    // dp[i][j]：text[0..i) 是否匹配 tokens[0..j)。
    const std::size_t n = text.size();
    const std::size_t m = tokens.size();
    std::vector<std::vector<char>> dp(n + 1, std::vector<char>(m + 1, 0));
    dp[0][0] = 1;
    for (std::size_t j = 0; j < m; ++j) {
        const Kind kind = tokens[j].first;
        for (std::size_t i = 0; i <= n; ++i) {
            if (kind == Star || kind == DoubleStar) {
                // 星号匹配空串，或多吃一个 text[i-1]（单星不吃 '.'）。
                if (dp[i][j]) {
                    dp[i][j + 1] = 1;
                }
                if (i > 0 && dp[i - 1][j + 1] && (kind == DoubleStar || text[i - 1] != '.')) {
                    dp[i][j + 1] = 1;
                }
                continue;
            }
            if (i == 0 || !dp[i - 1][j]) continue;
            const char c = text[i - 1];
            if (kind == Any) {
                if (c != '.') dp[i][j + 1] = 1;
            }
            else if (tokens[j].second == c) {
                dp[i][j + 1] = 1;
            }
        }
    }
    return dp[n][m] != 0;
}

inline bool normalizeClassGlobs(const std::vector<std::string>& raw, std::vector<std::string>& out)
{
    out.clear();
    for (const auto& value : raw) {
        std::size_t start = 0;
        while (start <= value.size()) {
            std::size_t comma = value.find(',', start);
            const bool  last  = (comma == std::string::npos);
            std::string piece = value.substr(start, last ? std::string::npos : comma - start);
            // 去首尾 ASCII 空白。
            const std::size_t b = piece.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                return false;   // 空片段：整串空白或 ",,"
            }
            const std::size_t e = piece.find_last_not_of(" \t\r\n");
            piece               = piece.substr(b, e - b + 1);
            for (auto& ch : piece) {
                if (ch == '/') ch = '.';
            }
            out.push_back(std::move(piece));
            if (last) break;
            start = comma + 1;
        }
    }
    return true;
}

inline bool classPassesFilter(const std::string&        className,
                              const std::vector<std::string>& include,
                              const std::vector<std::string>& exclude)
{
    // CompilerIndex 的键是 JVM 内部名（`/` 分隔，如 demo/CliJarA）；文本与模式
    // 两侧一并归一化，保证同构（模式侧容忍调用方传未归一化的 jvmtp 形态）。
    auto normalize = [](std::string s) {
        for (auto& ch : s) {
            if (ch == '/') ch = '.';
        }
        return s;
    };
    const std::string name = normalize(className);
    for (const auto& pattern : exclude) {
        if (globMatchClassName(name, normalize(pattern))) return false;
    }
    if (include.empty()) return true;
    for (const auto& pattern : include) {
        if (globMatchClassName(name, normalize(pattern))) return true;
    }
    return false;
}

}   // namespace mj2x

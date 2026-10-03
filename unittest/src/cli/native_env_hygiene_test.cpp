// native_env_hygiene_test.cpp
//
// 子进程环境形状卫生函数簇的合成 env 断言（issue #1023 item 1，消解 #1022
// 验证残留：去重硬证据、16KB WARN 观测、isAbnormalChildExit 阈值边界）。
// 被测函数在 mj2x-cli/src/native_env_hygiene.hpp（自匿名 namespace 纯搬移，
// 行为零改动）；本测试经跨模块 include 直接实例化 inline 定义，不链接
// mj2x-cli（unittest 与 CLI 无符号关系，见 unittest/xmake.lua 的
// mj2x-cli-companion 注记）。
//
// mergeProcessEnvironment 的两类断言走行为面：
//   - 含 '=' 键/空值剔除与 case-insensitive 同名胜出：spawn 真子进程打印
//     环境回读（Command::env_ 私有无读取器）；
//   - >16KB WARN 分支：注册捕获日志后端到 libca LoggerRegistry 的 "compile"
//     target（CLI 侧同 target 名，见 mj2x-core/src/mj2x/logging.cpp）。

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/log/logger.hpp"
#include "libca/log/logger_registry.hpp"

#include "mj2x-cli/src/native_env_hygiene.hpp"

namespace {

using mj2x::cli::dedupePathValue;
using mj2x::cli::environmentKeyEquals;
using mj2x::cli::isPathFamilyKey;
using mj2x::cli::kMergedEnvironmentBytesLimit;
using mj2x::cli::mergeProcessEnvironment;
using mj2x::cli::ProcessEnvironment;

// ---- dedupePathValue：路径族值去重（splitenv→unique→joinenv）----

TEST(NativeEnvHygieneDedupePathValue, KeepsFirstOccurrenceInOrder)
{
    EXPECT_EQ(dedupePathValue("C:\\a;C:\\b;C:\\a;C:\\b;C:\\a"), "C:\\a;C:\\b");
    EXPECT_EQ(dedupePathValue("C:\\b;C:\\a"), "C:\\b;C:\\a");
}

TEST(NativeEnvHygieneDedupePathValue, MergesCaseVariantsOnWindows)
{
#ifdef _WIN32
    // Windows 比较大小写不敏感（与文件系统口径一致），保序取首个。
    EXPECT_EQ(dedupePathValue("C:\\Tools;c:\\tools;C:\\TOOLS"), "C:\\Tools");
#else
    GTEST_SKIP() << "case-insensitive dedup is Windows-only";
#endif
}

TEST(NativeEnvHygieneDedupePathValue, DropsEmptySegments)
{
    // 内部空段与尾分号均剔除（去重只收缩形状，不改查找语义）。
    EXPECT_EQ(dedupePathValue("C:\\a;;C:\\b;"), "C:\\a;C:\\b");
    EXPECT_EQ(dedupePathValue(""), "");
    EXPECT_EQ(dedupePathValue(";;"), "");
}

TEST(NativeEnvHygieneDedupePathValue, KeepsQuotedPathDistinctFromBare)
{
    // 引号路径按字面量参与比较：带引号与不带引号不合并（引号剥离只发生在
    // loadMsvcEnvironment 的 cl.exe 探测处，去重不做语义解释）。
    EXPECT_EQ(dedupePathValue("\"C:\\a\";C:\\a"), "\"C:\\a\";C:\\a");
}

#ifndef _WIN32
TEST(NativeEnvHygieneDedupePathValue, NonWindowsUsesColonAndExactCompare)
{
    EXPECT_EQ(dedupePathValue("/a:/A:/a"), "/a:/A");
}
#endif

// ---- isAbnormalChildExit：NTSTATUS 失败码阈值口径（<= -1000 触发重试）----

#ifdef _WIN32
TEST(NativeEnvHygieneIsAbnormalChildExit, ThresholdBoundaries)
{
    using mj2x::cli::isAbnormalChildExit;
    EXPECT_FALSE(isAbnormalChildExit(0));
    EXPECT_FALSE(isAbnormalChildExit(1));
    EXPECT_FALSE(isAbnormalChildExit(-1));           // 常规错误退出不触发重试
    EXPECT_FALSE(isAbnormalChildExit(-2));
    EXPECT_FALSE(isAbnormalChildExit(-999));         // 阈值下侧
    EXPECT_TRUE(isAbnormalChildExit(-1000));         // 阈值边界（<= 判定）
    EXPECT_TRUE(isAbnormalChildExit(-1073740940));   // 0xC0000374 堆损坏（带符号化 NTSTATUS）
    EXPECT_TRUE(isAbnormalChildExit(-1073741819));   // 0xC0000005 访问违例
    EXPECT_TRUE(isAbnormalChildExit(-2147483647 - 1)); // INT_MIN
}
#endif

// ---- environmentKeyEquals / isPathFamilyKey：键口径 ----

TEST(NativeEnvHygieneKeys, CaseInsensitiveMatchAndPathFamilySet)
{
    EXPECT_TRUE(environmentKeyEquals("Path", "PATH"));
    EXPECT_FALSE(environmentKeyEquals("PATHH", "PATH"));
    EXPECT_TRUE(isPathFamilyKey("include"));
    EXPECT_TRUE(isPathFamilyKey("Lib"));
    EXPECT_FALSE(isPathFamilyKey("LIBPATH"));   // 重试覆盖集有它，路径族没有
    EXPECT_FALSE(isPathFamilyKey("TEMP"));
}

// ---- mergeProcessEnvironment：剔除/胜出（spawn 行为级）----

void setProcessEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv((std::string(name) + "=" + value).c_str());
#else
    setenv(name, value, 1);
#endif
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string              current;
    for (const char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        }
        else {
            current += c;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

TEST(NativeEnvHygieneMerge, CullsInvalidEntriesAndCapturedOverridesParent)
{
#ifdef _WIN32
    // 父进程预置：独有变量、与 captured 大小写变体同名变量、空值覆盖目标变量。
    setProcessEnv("MJ2X_HYG_PARENT_ONLY", "parent");
    setProcessEnv("MJ2X_HYG_BOTH", "parent");
    setProcessEnv("MJ2X_HYG_EMPTY", "parent");
    ca::process::Command command("cmd");
    command.arg("/c").arg("set");
#else
    ca::process::Command command("env");
#endif

    const ProcessEnvironment captured = {
        {"MJ2X_HYG_GOOD", "good"},
        {"MJ2X_HYG_BOTH", "captured"},   // case-insensitive 同名以 captured 胜出
        {"MJ2X_HYG_EMPTY", ""},          // 空值覆盖剔除——该键不注入
        {"MJ2X_HYG=BAD", "x"},           // 含 '=' 键剔除（libca 会整体拒绝注入）
    };
    mergeProcessEnvironment(command, captured);

    auto completed = command.output();
    ASSERT_FALSE(completed.is_err())
        << "spawn must not fail: culled entries (empty value / '=' key) must not "
           "reach Command::env";
    const auto output = std::move(completed).unwrap();
    const auto lines  = splitLines(output.stdout_data);

    const auto lineOf = [&lines](const std::string& entry) {
        for (const auto& line : lines) {
            if (line == entry) return true;
        }
        return false;
    };
    const auto hasPrefix = [&lines](const std::string& prefix) {
        for (const auto& line : lines) {
            if (line.rfind(prefix, 0) == 0) return true;
        }
        return false;
    };

    EXPECT_TRUE(lineOf("MJ2X_HYG_GOOD=good"));
    // 同名胜出且父进程底版被跳过：值恰为 captured、parent 版不出现。
    EXPECT_TRUE(lineOf("MJ2X_HYG_BOTH=captured"));
    EXPECT_FALSE(lineOf("MJ2X_HYG_BOTH=parent"));
    // 含 '=' 键整条剔除：子进程环境不得出现该条目形状。
    EXPECT_FALSE(hasPrefix("MJ2X_HYG=BAD="));
    // 空值覆盖剔除：无 "VAR=" 空值条目；Windows 下子进程保留父进程原值。
    EXPECT_FALSE(lineOf("MJ2X_HYG_EMPTY="));
#ifdef _WIN32
    EXPECT_TRUE(lineOf("MJ2X_HYG_EMPTY=parent"));
    EXPECT_TRUE(lineOf("MJ2X_HYG_PARENT_ONLY=parent"));
#endif
}

// ---- mergeProcessEnvironment：>16KB WARN 分支（捕获日志后端）----

class CapturingLogBackend : public ca::log::ILogBackend
{
public:
    void log(ca::log::Level, std::string_view, std::string_view, int,
             const ca::log::OpaqueFormat& message) override
    {
        std::string rendered;
        message.render_to(rendered);
        entries_.push_back(std::move(rendered));
    }

    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

/// RAII 把捕获后端挂到 LoggerRegistry 的 "compile" target，析构注销还原。
class CompileLogCapture
{
public:
    CompileLogCapture()
    {
        backend_ = std::make_shared<CapturingLogBackend>();
        ca::log::LoggerRegistry::register_logger(
            "compile", std::make_shared<ca::log::Logger>(backend_));
    }
    ~CompileLogCapture() { ca::log::LoggerRegistry::register_logger("compile", nullptr); }

    CompileLogCapture(const CompileLogCapture&)            = delete;
    CompileLogCapture& operator=(const CompileLogCapture&) = delete;

    const std::vector<std::string>& entries() const { return backend_->entries(); }

private:
    std::shared_ptr<CapturingLogBackend> backend_;
};

std::size_t countContains(const std::vector<std::string>& entries, const std::string& needle)
{
    std::size_t count = 0;
    for (const auto& entry : entries) {
        if (entry.find(needle) != std::string::npos) ++count;
    }
    return count;
}

TEST(NativeEnvHygieneMerge, WarnsWhenMergedBlockExceedsBytesLimit)
{
    CompileLogCapture capture;
    // 单条超长 PATH 值（无 ';' 可去重，去重后仍超限），键 4 字节 + 值 + 2。
    const std::string hugeValue(20 * 1024, 'x');
    ca::process::Command command("xmake");
    mergeProcessEnvironment(command, {{"PATH", hugeValue}});

    const auto& entries = capture.entries();
    EXPECT_EQ(countContains(entries, "merged child environment block is"), 1U);

    // WARN 消息自证字节数：提取 "block is <N> bytes" 的 N，断言 > 限值且与
    // 注入集一致（父进程底版增量之外，captured 单条贡献 4 + 20KB + 2）。
    std::size_t reported = 0;
    for (const auto& entry : entries) {
        const auto marker = entry.find("merged child environment block is ");
        if (marker == std::string::npos) continue;
        const auto begin = marker + std::strlen("merged child environment block is ");
        const auto end   = entry.find(" bytes", begin);
        ASSERT_NE(end, std::string::npos) << entry;
        reported = std::stoull(entry.substr(begin, end - begin));
    }
    EXPECT_GT(reported, kMergedEnvironmentBytesLimit);
    EXPECT_GE(reported, std::strlen("PATH") + hugeValue.size() + 2);
}

TEST(NativeEnvHygieneMerge, NoWarnWhenMergedBlockUnderBytesLimit)
{
    // 自感知宿主环境病态巨型 PATH 的干扰：按产品同款口径预估总字节，超出
    // 限值则本断言无法稳定成立（环境问题而非产品缺陷），跳过。
    std::size_t expected = 0;
    for (const auto& [key, value] : mj2x::cli::currentProcessEnvironment()) {
        if (key.empty() || value.empty() || key.find('=') != std::string::npos) continue;
        expected += key.size() + (isPathFamilyKey(key) ? dedupePathValue(value) : value).size() + 2;
    }
    expected += std::strlen("PATH") + dedupePathValue("C:\\small").size() + 2;
    if (expected > kMergedEnvironmentBytesLimit) {
        GTEST_SKIP() << "host environment alone exceeds " << kMergedEnvironmentBytesLimit
                     << " bytes";
    }

    CompileLogCapture capture;
    ca::process::Command command("xmake");
    mergeProcessEnvironment(command, {{"PATH", "C:\\small"}});
    EXPECT_EQ(countContains(capture.entries(), "merged child environment block is"), 0U);
}

}   // namespace

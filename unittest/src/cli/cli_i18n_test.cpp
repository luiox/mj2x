// cli_i18n_test.cpp
//
// CLI 消息国际化(issue #888)端到端:--lang/MJ2X_LANG 语言解析、未知语言回退
// zh_CN、help/schema/错误文案随语言切换。经 CLI 进程验证(与 cli_test.cpp
// 同风格:bat/sh 脚本包装 + chcp 65001)。

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

struct ExecResult
{
    int         exitCode = -1;
    std::string out;
};

std::string q(const fs::path& path)
{
    std::string s = path.string();
#ifdef _WIN32
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return "\"" + s + "\"";
}

bool fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

fs::path findCli()
{
    auto path = test::getBuildTargetPath("mj2x-cli");
    if (fileExists(path)) return path;
    return {};
}

/// Sanitize an inherited PATH for batch scripts(与 cli_test.cpp 同因:cmd 继承的
/// PATH 可能含未展开的 %VAR% 与双反斜杠,会击穿 xmake 子进程)。
std::string sanitizePathForBatch(const char* pathEnv)
{
    std::string out;
    std::string seg;
    if (!pathEnv) return out;
    for (const char* p = pathEnv;; ++p) {
        if (*p == ';' || *p == '\0') {
            if (!seg.empty() && seg.find('%') == std::string::npos) {
                if (seg[0] != '\\' || seg.size() < 2 || seg[1] != '\\') {
                    std::string folded;
                    for (size_t i = 0; i < seg.size();) {
                        if (seg[i] == '\\' && i + 1 < seg.size() && seg[i + 1] == '\\') {
                            folded += '\\';
                            i += 2;
                        }
                        else {
                            folded += seg[i];
                            ++i;
                        }
                    }
                    seg = folded;
                }
                if (!out.empty()) out += ';';
                out += seg;
            }
            seg.clear();
            if (*p == '\0') break;
        }
        else {
            seg += *p;
        }
    }
    return out;
}

/// 运行 mj2x-cli(可选预设环境变量),捕获 stdout+stderr 与退出码。
ExecResult runCliEnv(const std::vector<std::string>& args, const char* langEnv = nullptr)
{
    ExecResult result;
    fs::path   tmpDir = test::getTempOutPath("mj2x_i18n_test.keep").parent_path();
#ifdef _WIN32
    fs::path script =
        tmpDir / ("mj2x_i18n_test_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  ".bat");
    {
        std::ofstream out(script);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        const std::string cleanPath = sanitizePathForBatch(std::getenv("PATH"));
        if (!cleanPath.empty()) out << "set \"PATH=" << cleanPath << "\"\n";
        if (langEnv) out << "set \"MJ2X_LANG=" << langEnv << "\"\n";
        std::string line = q(findCli());
        for (const auto& a : args) line += " " + a;
        out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }
    std::string command = q(script) + " 2>&1";
#else
    fs::path script =
        tmpDir / ("mj2x_i18n_test_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  ".sh");
    {
        std::ofstream out(script);
        out << "#!/usr/bin/env sh\n";
        std::string line = q(findCli());
        for (const auto& a : args) line += " " + a;
        if (langEnv) line = "MJ2X_LANG=" + std::string(langEnv) + " " + line;
        out << line << "\n";
    }
    std::string command = "sh " + q(script) + " 2>&1";
#endif
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return result;
    char   buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        result.out.append(buffer, n);
    }
#ifdef _WIN32
    result.exitCode = _pclose(pipe);
#else
    result.exitCode = WEXITSTATUS(pclose(pipe));
#endif
    return result;
}

/// zh_CN 断言子串(UTF-8 字面量以 u8 前缀 + (const char*) 适配窄执行字符集)。
const char* kZhUsage       = "\xE7\x94\xA8\xE6\xB3\x95:";                 // 用法:
const char* kZhUnknownKey  = "\xE6\x9C\xAA\xE7\x9F\xA5\xE9\x85\x8D\xE7\xBD\xAE\xE9\x94\xAE";  // 未知配置键
const char* kZhBackendDesc = "\xE9\x80\x89\xE6\x8B\xA9 native \xE5\x8F\x91\xE5\xB0\x84\xE5\x90\x8E\xE7\xAB\xAF";  // 选择 native 发射后端

class CliI18nTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        cli_ = findCli();
        if (cli_.empty()) GTEST_SKIP() << "mj2x-cli is not built";
    }

    fs::path cli_;
};

TEST_F(CliI18nTest, DefaultLanguageIsZhCn)
{
    const auto result = runCliEnv({"--help"});
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find(kZhUsage), std::string::npos) << result.out;
    EXPECT_EQ(result.out.find("Usage:"), std::string::npos) << result.out;
    // 参数描述与分组标题同表本地化。
    EXPECT_NE(result.out.find(kZhBackendDesc), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, LangFlagSwitchesToEnglish)
{
    const auto result = runCliEnv({"--lang", "en_US", "--help"});
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("Usage:"), std::string::npos) << result.out;
    EXPECT_NE(result.out.find("Input file (.class / .jar)"), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, InlineLangSyntaxSwitches)
{
    const auto result = runCliEnv({"--lang=en_US", "--help"});
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("Usage:"), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, LangEnvVarSwitchesLanguage)
{
    const auto result = runCliEnv({"--help"}, "en_US");
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("Usage:"), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, LangFlagOverridesEnvVar)
{
    const auto result = runCliEnv({"--lang", "zh_CN", "--help"}, "en_US");
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find(kZhUsage), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, UnknownLangFallsBackToZhCn)
{
    const auto result = runCliEnv({"--lang", "fr_FR", "--help"});
    EXPECT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find(kZhUsage), std::string::npos) << result.out;
}

// ── help flag 段 golden（issue #1075 验收项 4）──
// flag 段两列布局迁入 HelpTable 渲染（值内零对齐空格），描述列逐行锁死历史
// 列位。以下逐行断言整行字节（前导缩进 + 名字列 + 历史描述列位）；换行符
// 不入断言（Windows 捕获为 CRLF、POSIX 为 LF）。历史列位本就不统一：
// config/help 行在全行 31 列、dump_schema/lang 行在 33 列、未迁移的
// dump_template 行在 34 列（自带对齐空格）。zh 描述为 UTF-8 字面量，
// \x 转义后紧跟十六进制字符处用 "" 分段防转义串位。

TEST_F(CliI18nTest, HelpFlagSectionGoldenEn)
{
    const auto result = runCliEnv({"--lang", "en_US", "--help"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("  --config <file>              Load JSON options; command line values override it"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --dump-config-schema [<file>]  Write config JSON Schema (draft-07) to file or stdout"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --dump-config-template [<file>] Write commented config template to file or stdout"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --lang <lang>                  UI language (zh_CN / en_US; MJ2X_LANG env is also honored)"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --help                       Print help"), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, HelpFlagSectionGoldenZh)
{
    const auto result = runCliEnv({"--help"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("  --config <file>              \xE5\x8A\xA0\xE8\xBD\xBD JSON \xE9\x85\x8D\xE7\xBD\xAE;\xE5\x91\xBD\xE4\xBB\xA4\xE8\xA1\x8C\xE5\x80\xBC\xE8\xA6\x86\xE7\x9B\x96\xE9\x85\x8D\xE7\xBD\xAE"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --dump-config-schema [<file>]  \xE8\xBE\x93\xE5\x87\xBA\xE9\x85\x8D\xE7\xBD\xAE JSON Schema(draft-07)\xE5\x88\xB0\xE6\x96\x87\xE4\xBB\xB6\xE6\x88\x96 stdout"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --dump-config-template [<file>] \xE8\xBE\x93\xE5\x87\xBA\xE5\xB8\xA6\xE6\xB3\xA8\xE9\x87\x8A\xE7\x9A\x84\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA8\xA1\xE6\x9D\xBF\xE5\x88\xB0\xE6\x96\x87\xE4\xBB\xB6\xE6\x88\x96 stdout"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --lang <lang>                  \xE7\x95\x8C\xE9\x9D\xA2\xE8\xAF\xAD\xE8\xA8\x80(zh_CN / en_US;\xE4\xBA\xA6\xE5\x8F\xAF\xE7\x94\xA8 MJ2X_LANG \xE7\x8E\xAF\xE5\xA2\x83\xE5\x8F\x98\xE9\x87\x8F)"),
              std::string::npos) << result.out;
    EXPECT_NE(result.out.find("  --help                       \xE6\x89\x93\xE5\x8D\xB0\xE5\xB8\xAE\xE5\x8A\xA9"),
              std::string::npos) << result.out;
}

// 迁移后 --lang 由 opt 按 String 选项解析（迁移前为容忍式手写扫描）：
// 悬空无值报 MissingValue，fail-closed。锁定该行为变更。
TEST_F(CliI18nTest, DanglingLangFlagFailsClosed)
{
    const auto result = runCliEnv({"--lang"});
    EXPECT_NE(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("需要提供值"), std::string::npos) << result.out;
}

// 空格形态的 --lang 会消费后继 token 作值（即使以 '-' 开头），剩余裸 token
// 报 UnexpectedArgument——与迁移前"静默忽略 lang、其余正常解析"不同，
// fail-closed。合法用法为 --lang=en_US 内联形态或 --lang <lang> <args...>。
TEST_F(CliI18nTest, LangFlagFollowedByFlagFailsClosed)
{
    const auto result = runCliEnv({"--lang", "-o", "out.class"});
    EXPECT_NE(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("未知选项"), std::string::npos) << result.out;
}

TEST_F(CliI18nTest, ErrorMessageLocalizesPerLanguage)
{
    const fs::path cfg = test::getTempOutPath("mj2x_i18n_unknown_key.json");
    {
        std::ofstream out(cfg);
        out << "{\"foo_bar\": 1}";
    }
    const auto zh = runCliEnv({"--config", cfg.string(), "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(zh.exitCode, 0);
    EXPECT_NE(zh.out.find(kZhUnknownKey), std::string::npos) << zh.out;

    const auto en =
        runCliEnv({"--lang", "en_US", "--config", cfg.string(), "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(en.exitCode, 0);
    EXPECT_NE(en.out.find("unknown config key"), std::string::npos) << en.out;
}

TEST_F(CliI18nTest, SchemaDescriptionLocalizes)
{
    const auto en = runCliEnv({"--lang", "en_US", "--dump-config-schema"});
    EXPECT_EQ(en.exitCode, 0) << en.out;
    EXPECT_NE(en.out.find("CFF constant-false dead-block density"), std::string::npos) << en.out;

    const auto zh = runCliEnv({"--dump-config-schema"});
    EXPECT_EQ(zh.exitCode, 0) << zh.out;
    // zh:CFF 恒假死块密度。
    const char* kZhCffDead = "CFF \xE6\x81\x92\xE5\x81\x87\xE6\xAD\xBB\xE5\x9D\x97\xE5\xAF\x86\xE5\xBA\xA6";
    EXPECT_NE(zh.out.find(kZhCffDead), std::string::npos) << zh.out;
}

}   // namespace

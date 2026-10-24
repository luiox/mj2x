// config_registry_test.cpp
//
// 配置系统（主线十二）测试：参数注册表单源 → --dump-config-schema /
// --dump-config-template 输出、配置文件全量键解析、未知键/类型/枚举错误定位、
// CLI↔config 合并语义。全部通过 CLI 进程验证（与 cli_test.cpp 同风格）。

#include <gmock/gmock.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

/// Sanitize an inherited PATH for batch scripts（与 cli_test.cpp 同因：cmd 继承的
/// PATH 可能含未展开的 %VAR% 与双反斜杠，会击穿 xmake 子进程）。
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

/// 运行 mj2x-cli 并捕获 stdout+stderr 与退出码。
///
/// 经临时 bat/sh 脚本执行（cli_test.cpp 同方案）：_popen 直传带引号命令行会触发
/// cmd /c 的引号剥离规则破坏参数，bat 方式已验证稳定。
ExecResult runCli(const std::vector<std::string>& args)
{
    ExecResult result;
    fs::path   tmpDir = test::getTempOutPath("mj2x_cfg_test.keep").parent_path();
#ifdef _WIN32
    fs::path script =
        tmpDir / ("mj2x_cfg_test_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  ".bat");
    {
        std::ofstream out(script);
        out << "@echo off\n";
        out << "chcp 65001 >nul 2>&1\n";
        const std::string cleanPath = sanitizePathForBatch(std::getenv("PATH"));
        if (!cleanPath.empty()) out << "set \"PATH=" << cleanPath << "\"\n";
        std::string line = q(findCli());
        for (const auto& a : args) line += " " + a;
        out << line << "\n";
        out << "exit /b %errorlevel%\n";
    }
    std::string command = q(script) + " 2>&1";
#else
    fs::path script =
        tmpDir / ("mj2x_cfg_test_" +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  ".sh");
    {
        std::ofstream out(script);
        out << "#!/usr/bin/env sh\n";
        std::string line = q(findCli());
        for (const auto& a : args) line += " " + a;
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

std::string writeConfig(const std::string& content, const char* name)
{
    const fs::path path = test::getTempOutPath(name);
    {
        std::ofstream out(path);
        out << content;
    }
    return q(path);
}

/// 所有合法配置键（与 param_registry.cpp 的 kParamTable 对齐；LLVM 键按构建裁剪）。
std::vector<std::string> expectedKeys()
{
    std::vector<std::string> keys = {
        "input",
        "output",
        "backend",
        "emit_c",
        "emit_cpp",
        "emit_native_dir",
        "dump_mir",
        "native_annotation",
        "native_method_prefixes",
        "native_include",
        "native_exclude",
        "compile",
        "compile_mode",
        "c_compiler",
        "native_timeout",
        "native_opt_level",
        "generate_cmake",
        "generate_xmake",
        "targets",
        "target_cc",
        "stackc_exception_check",
        "regc_trace_calls",
        "regc_direct_calls",
        "regc_comment_level",
        "regc_safe_env",
        "regc_safe_env_debug",
        "regc_no_inline",
        "regc_obfuscate",
        "regc_cff_dead",
        "regc_cff_state_encrypt",
        "regc_mba_deep",
        "regc_opaque_zero",
        "regc_hide_constants",
        "regc_indirect_calls",
        "regc_pod_struct",
        "regc_computed_jumps",
        "regc_transform_seed",
        "vmc4_dispatcher",
        "vmc4_seed",
        "vmc4_granularity",
        "vmc4_alias",
        "vmc4_bogus",
        "vmc4_drift",
        "vmc4_drift_period",
        "vmc4_trace_surface",
        "vmc4_encrypt_program",
        "vmc4_lazy_decrypt",
        "vmc4_block_lazy_decrypt",
        "vmc4_shuffle_blocks",
        "vmc4_super_ops",
        "vmc4_fragment",
        "vmc4_regfile",
        "strings_encrypt",
        "runtime_protection",
        "runtime_protection_step",
        "runtime_protection_delay_ms",
        "native_lib_key",
        "native_lib_pack_hook",
        "client_id",
        "intrinsic_config",
        "obf_scope_exclude",
        "obf_scope_include",
        "intrinsic_src",
        "log_quiet",
        "log_verbose",
    };
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    keys.insert(keys.begin() + 5, "emit_llvm");
    keys.push_back("llvm_tool");
#endif
    return keys;
}

}   // namespace

// ---------- schema 导出 ----------

TEST(ConfigRegistryTest, SchemaRoundTripsAllRegisteredKeys)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-config-schema"});
    ASSERT_EQ(result.exitCode, 0) << result.out;

    // 固定头部（dump(2) 输出）。
    EXPECT_NE(result.out.find("\"$schema\": \"http://json-schema.org/draft-07/schema#\""),
              std::string::npos)
        << result.out;
    EXPECT_NE(result.out.find("\"type\": \"object\""), std::string::npos) << result.out;
    EXPECT_NE(result.out.find("\"additionalProperties\": false"), std::string::npos)
        << result.out;

    // 每个注册键都出现在 properties 中。
    const std::string propsSection = result.out.substr(result.out.find("\"properties\""));
    for (const auto& key : expectedKeys()) {
        EXPECT_NE(propsSection.find("\"" + key + "\""), std::string::npos)
            << "schema 缺键: " << key;
    }
}

TEST(ConfigRegistryTest, SchemaAnnotationsMatchRegistrySemantics)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-config-schema"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    const std::string& out = result.out;

    // 枚举 + 默认值（backend / runtime_protection）。dump(2) 键按字母序输出，
    // default 在 description 之前。llvm 后端按构建配置条件存在。
    EXPECT_NE(out.find("\"enum\": [\n        \"stackc\",\n        \"regc\""), std::string::npos)
        << out;
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    EXPECT_NE(out.find("\"enum\": [\n        \"stackc\",\n        \"regc\",\n        \"llvm\""),
              std::string::npos)
        << out;
#endif
    EXPECT_NE(out.find("\"backend\": {\n      \"default\": \"stackc\","), std::string::npos)
        << out;
    EXPECT_NE(out.find("\"runtime_protection\": {\n      \"default\": \"none\","),
              std::string::npos)
        << out;
    EXPECT_NE(out.find("\"enum\": [\n        \"none\",\n        \"log\",\n        \"delay\""),
              std::string::npos)
        << out;

    // 数值范围（dump 键按字母序；Int 恒非负，minimum 恒输出——无追加下界即为 0）。
    EXPECT_NE(out.find("\"regc_cff_dead\": {"), std::string::npos) << out;
    EXPECT_NE(out.find("\"minimum\": 0,\n      \"type\": \"integer\""), std::string::npos)
        << out;
    EXPECT_NE(out.find("\"maximum\": 3,\n      \"minimum\": 0,"), std::string::npos) << out;
    EXPECT_NE(out.find("\"minimum\": 1,"), std::string::npos) << out;
    EXPECT_NE(out.find("\"maximum\": 4,"), std::string::npos) << out;

    // hex pattern / 列表 / 布尔。
    EXPECT_NE(out.find("\"pattern\": \"^[0-9a-fA-F]+$\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"items\": {\n        \"type\": \"string\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"log_quiet\": {"), std::string::npos) << out;
    EXPECT_NE(out.find("\"default\": false"), std::string::npos) << out;

    // 描述非空（help/schema 同源）：每个注册键 + schema 顶层各一条 description。
    std::size_t descCount = 0;
    std::size_t pos       = 0;
    while ((pos = out.find("\"description\": \"", pos)) != std::string::npos) {
        ++descCount;
        pos += std::string("\"description\": \"").size();
    }
    EXPECT_EQ(descCount, expectedKeys().size() + 1);   // +1 = schema 顶层 description
}

TEST(ConfigRegistryTest, SchemaWrittenToFileMatchesStdout)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const fs::path schemaFile = test::getTempOutPath("mj2x_config_schema.json");
    // OptionalString 形态:文件目标仅支持内联 =（裸出现=stdout;空格形态不再
    // 消费后继,杜绝与位置参数的歧义）。
    const auto     toFile     = runCli({"--dump-config-schema=" + q(schemaFile)});
    ASSERT_EQ(toFile.exitCode, 0) << toFile.out;
    ASSERT_TRUE(fileExists(schemaFile));

    const auto toStdout = runCli({"--dump-config-schema"});
    ASSERT_EQ(toStdout.exitCode, 0);
    std::ifstream in(schemaFile);
    std::string   fromFile((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(fromFile, toStdout.out);
}

TEST(ConfigRegistryTest, TemplateContainsAllKeys)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-config-template"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    for (const auto& key : expectedKeys()) {
        EXPECT_NE(result.out.find("\"" + key + "\""), std::string::npos)
            << "模板缺键: " << key;
    }
}

// 双 dump 互斥：MutexConflict 经 ParseError.group（MutexGroup::label =
// "dump_once"）分派到专用文案，锁定组标识分派链路。
TEST(ConfigRegistryTest, BothDumpFlagsRejectedViaGroupLabel)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-config-schema", "--dump-config-template"});
    EXPECT_NE(result.exitCode, 0) << result.out;
    EXPECT_NE(result.out.find("只能指定一次"), std::string::npos) << result.out;
}

// ---------- 配置文件解析 ----------

TEST(ConfigRegistryTest, UnknownKeyRejectedWithKeyName)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const std::string cfg = writeConfig("{\"foo_bar\": 1}", "mj2x_cfg_unknown_key.json");
    const auto        result = runCli({"--config", cfg, "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(result.exitCode, 0);
    // 消息文案随语言本地化(见 cli_i18n_test);此处只断言键名定位。
    EXPECT_NE(result.out.find("foo_bar"), std::string::npos) << result.out;
}

TEST(ConfigRegistryTest, TypeMismatchReportedWithKeyName)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const std::string cfg = writeConfig("{\"backend\": 123}", "mj2x_cfg_type_backend.json");
    const auto        result = runCli({"--config", cfg, "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.out.find("backend"), std::string::npos) << result.out;

    const std::string cfg2 =
        writeConfig("{\"regc_cff_dead\": \"abc\"}", "mj2x_cfg_type_int.json");
    const auto result2 = runCli({"--config", cfg2, "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(result2.exitCode, 0);
    EXPECT_NE(result2.out.find("regc_cff_dead"), std::string::npos) << result2.out;
}

TEST(ConfigRegistryTest, EnumValueValidated)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const std::string cfg =
        writeConfig("{\"vmc4_dispatcher\": \"bogus_kind\"}", "mj2x_cfg_enum.json");
    const auto result = runCli({"--config", cfg, "-i", "in.class", "-o", "out.class"});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.out.find("vmc4_dispatcher"), std::string::npos) << result.out;
}

TEST(ConfigRegistryTest, BackendFromConfigDrivesValidation)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    // config backend=vmc4 + --emit-cpp：交叉校验应命中 "--emit-cpp requires --backend regc"。
    const std::string cfg = writeConfig("{\"backend\": \"vmc4\"}", "mj2x_cfg_backend_vmc4.json");
    const auto        result =
        runCli({"--config", cfg, "-i", "in.class", "-o", "out.class", "--emit-cpp", "x.cpp"});
    EXPECT_NE(result.exitCode, 0);
    // CLI 默认 zh_CN 输出（i18n 后文案经 MJ2X_LANG/--lang 选择，此处默认语言）。
    EXPECT_NE(result.out.find("--emit-cpp 当前需要 --backend regc"), std::string::npos)
        << result.out;
}

TEST(ConfigRegistryTest, CliOverridesConfigBackend)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    // config backend=vmc4 被 CLI --backend regc 覆盖：--emit-cpp 交叉校验通过，
    // 随后因缺 -i/-o 报 usage（输出不应再含 emit-cpp/backend 校验错误）。
    const std::string cfg = writeConfig("{\"backend\": \"vmc4\"}", "mj2x_cfg_backend_override.json");
    const auto result =
        runCli({"--config", cfg, "--backend", "regc", "--emit-cpp", "x.cpp"});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_EQ(result.out.find("--emit-cpp currently requires --backend regc"),
              std::string::npos)
        << result.out;
}

TEST(ConfigRegistryTest, ConfigSupportsAllBackendFamilies)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    // regc/vmc4/runtime/nativeLib/client 族键一次配置 → 应通过解析（无 -i/-o 时报
    // usage 而非配置错误），证明全量参数已进配置文件。
    const std::string cfg = writeConfig(
        "{\"backend\": \"regc\", \"regc_cff_dead\": 3, \"regc_mba_deep\": true, "
        "\"vmc4_drift\": 2, \"runtime_protection\": \"fake\", \"client_id\": 7, "
        "\"native_lib_key\": \"1a2b\"}",
        "mj2x_cfg_all_families.json");
    const auto result = runCli({"--config", cfg});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_EQ(result.out.find("unknown config key"), std::string::npos) << result.out;
    EXPECT_EQ(result.out.find("invalid value"), std::string::npos) << result.out;
    EXPECT_EQ(result.out.find("config key"), std::string::npos) << result.out;
    // 解析成功 → 失败原因是缺 -i/-o 的 usage(默认 zh_CN,文案随语言本地化)。
    EXPECT_TRUE(result.out.find("Usage: mj2x-cli") != std::string::npos ||
                result.out.find("\xE7\x94\xA8\xE6\xB3\x95: mj2x-cli") != std::string::npos)
        << result.out;
}

// ---- preset（默认配置档）+ 生效配置回显 ----
//
// 四层合并顺序：默认值 < preset < 配置文件 < CLI。preset 不是第四套语法——
// 它是内置的 JSON 对象，键名与配置文件完全相同，走同一套 parseJson 校验。

/// --dump-effective-config 的输出 → key -> {value 原文, source 层名}。
///
/// @details
/// 回显是排查"到底开了啥"的唯一手段，故测试按它断言而不是另造观测点。
/// 逐行扫描而非正则：dump(2) 是缩进多行输出，跨行正则在不同标准库实现上行为
/// 不一致（MSVC 实测匹配不到），逐行扫描没有这层风险。
std::map<std::string, std::pair<std::string, std::string>> parseEffective(const std::string& out)
{
    std::map<std::string, std::pair<std::string, std::string>> result;
    std::istringstream                                         in(out);
    std::string                                                line;
    std::string                                                key;
    std::string                                                value;
    std::string                                                source;
    bool                                                       hasValue  = false;
    bool                                                       hasSource = false;

    // nlohmann::json 按字母序输出对象键，条目内是 "source" 先于 "value"。
    // 故不能假定读取顺序：只有两个字段都到齐才算一条完整条目。
    auto reset = [&]() {
        key.clear();
        value.clear();
        source.clear();
        hasValue = hasSource = false;
    };
    auto flush = [&]() {
        // 只在实际提交后重置——字段到齐前重置会把已读到的那一半丢掉。
        if (!key.empty() && hasValue && hasSource) {
            result[key] = {value, source};
            reset();
        }
    };

    auto trimmed = [](const std::string& text) {
        const size_t b = text.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        const size_t e = text.find_last_not_of(" \t\r\n");
        return text.substr(b, e - b + 1);
    };

    while (std::getline(in, line)) {
        const std::string text = trimmed(line);
        if (text.rfind("\"", 0) != 0) continue;
        const size_t nameEnd = text.find('"', 1);
        if (nameEnd == std::string::npos) continue;
        const std::string name = text.substr(1, nameEnd - 1);
        const std::string rest = trimmed(text.substr(nameEnd + 1));
        if (rest.empty()) continue;
        if (rest[0] != ':') continue;
        const std::string body = trimmed(rest.substr(1));
        if (body == "{") {
            // 条目开头："<key>": {（跳过 effective/layers/preset/config 这几个
            // 顶层键，它们不是参数条目）。
            flush();
            reset();
            if (name != "effective" && name != "layers" && name != "preset" && name != "config") {
                key = name;
            }
            continue;
        }
        if (key.empty()) continue;
        if (name == "value") {
            value = body;
            while (!value.empty() && value.back() == ',') value.pop_back();
            hasValue = true;
            flush();
        }
        else if (name == "source") {
            source = body;
            while (!source.empty() && (source.back() == ',' || source.back() == '"')) {
                source.pop_back();
            }
            if (!source.empty() && source.front() == '"') source.erase(0, 1);
            hasSource = true;
            flush();
        }
    }
    flush();
    return result;
}

TEST(ConfigRegistryTest, PresetUnknownNameIsRejectedWithValidList)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--preset", "bogus"});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_NE(result.out.find("bogus"), std::string::npos) << result.out;
    // 报错要列出合法档位，否则用户只能去翻文档。
    for (const char* name : {"none", "perf", "balanced", "max"}) {
        EXPECT_NE(result.out.find(name), std::string::npos) << result.out;
    }
}

TEST(ConfigRegistryTest, PresetNamesAreAcceptedCaseInsensitively)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    for (const char* name : {"none", "perf", "balanced", "max", "BALANCED", "Max"}) {
        SCOPED_TRACE(name);
        const auto result = runCli({"--preset", name, "--dump-effective-config"});
        EXPECT_EQ(result.exitCode, 0) << result.out;
        EXPECT_NE(result.out.find("\"preset\""), std::string::npos) << result.out;
    }
}

// perf 档：JNI 边界压到最少（直调开、内联保留、safe-env 关），零混淆。
TEST(ConfigRegistryTest, PerfPresetTurnsOnDirectCalls)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--preset", "perf", "--dump-effective-config"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    const auto effective = parseEffective(result.out);
    ASSERT_FALSE(effective.empty()) << result.out;

    // 字符串值在回显里是 JSON 原始 token，带引号。
    EXPECT_EQ(effective.at("regc_direct_calls").first, "\"all\"") << result.out;
    EXPECT_EQ(effective.at("regc_direct_calls").second, "preset") << result.out;
    EXPECT_EQ(effective.at("regc_no_inline").first, "false");
    EXPECT_EQ(effective.at("regc_no_inline").second, "preset");
    // 零混淆：重型两项保持默认。
    EXPECT_EQ(effective.at("regc_hide_constants").second, "default");
    EXPECT_EQ(effective.at("regc_indirect_calls").second, "default");
}

// max 档：两个重型项必须被打开——它们不被 --regc-obfuscate 联动，靠手记开关
// 最容易漏，正是"要保护才付代价"的那两项。
TEST(ConfigRegistryTest, MaxPresetTurnsOnHeavyweightOptions)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--preset", "max", "--dump-effective-config"});
    ASSERT_EQ(result.exitCode, 0) << result.out;
    const auto effective = parseEffective(result.out);
    ASSERT_FALSE(effective.empty()) << result.out;

    for (const char* key : {"regc_hide_constants", "regc_indirect_calls", "regc_obfuscate",
                            "regc_mba_deep", "regc_opaque_zero", "regc_cff_state_encrypt"}) {
        ASSERT_NE(effective.find(key), effective.end()) << key << " missing: " << result.out;
        EXPECT_EQ(effective.at(key).first, "true") << key;
        EXPECT_EQ(effective.at(key).second, "preset") << key;
    }
}

// 四层优先级的可观测证据：同一键在 preset / config / cli 三层各给一次，
// 生效值与来源层必须逐层推进。
TEST(ConfigRegistryTest, FourLayerPriorityIsObservable)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    // 未给 preset/config：默认层。
    const auto plain = parseEffective(runCli({"--dump-effective-config"}).out);
    ASSERT_FALSE(plain.empty());
    EXPECT_EQ(plain.at("regc_obfuscate").first, "false");
    EXPECT_EQ(plain.at("regc_obfuscate").second, "default");

    // preset max 打开它。
    const auto fromPreset =
        parseEffective(runCli({"--preset", "max", "--dump-effective-config"}).out);
    EXPECT_EQ(fromPreset.at("regc_obfuscate").first, "true");
    EXPECT_EQ(fromPreset.at("regc_obfuscate").second, "preset");

    // 配置文件关掉它 → config 层赢过 preset。
    const std::string cfg = writeConfig("{\"regc_obfuscate\": false}", "mj2x_cfg_over_preset.json");
    const auto fromConfig =
        parseEffective(runCli({"--preset", "max", "--config", cfg, "--dump-effective-config"}).out);
    EXPECT_EQ(fromConfig.at("regc_obfuscate").first, "false");

    // CLI 显式打开 → cli 层赢过 config。
    const auto fromCli = parseEffective(
        runCli({"--preset", "max", "--config", cfg, "--regc-obfuscate", "--dump-effective-config"})
            .out);
    EXPECT_EQ(fromCli.at("regc_obfuscate").first, "true");
    EXPECT_EQ(fromCli.at("regc_obfuscate").second, "cli");
}

// --dump-effective-config 不要求 -i/-o（与 schema/template 同口径）：它的用途
// 就是排查"到底开了啥"，产不产物无关。
TEST(ConfigRegistryTest, EffectiveConfigDumpDoesNotRequireInputOutput)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-effective-config"});
    EXPECT_EQ(result.exitCode, 0) << result.out;
    // 走到 -i/-o 校验会打用法（"用法"的 UTF-8），出现即说明位置放错了。
    EXPECT_EQ(result.out.find("\xE7\x94\xA8\xE6\xB3\x95"), std::string::npos) << result.out;
    // 四个层都要在输出里出现。
    for (const char* layer : {"default", "preset", "config", "cli"}) {
        EXPECT_NE(result.out.find(layer), std::string::npos) << result.out;
    }
}

// 三个 dump 互斥（与 schema/template 同组）。
TEST(ConfigRegistryTest, DumpFlagsAreMutuallyExclusiveWithEffectiveConfig)
{
    const fs::path cli = findCli();
    if (cli.empty()) GTEST_SKIP() << "mj2x-cli is not built";

    const auto result = runCli({"--dump-config-schema", "--dump-effective-config"});
    EXPECT_NE(result.exitCode, 0) << result.out;
}

// param_registry.cpp
//
// 参数注册表单源实现（配置系统主线十二）：全量参数声明、CLI/JSON 解析回调、
// JSON Schema 构建、--help 分组输出、配置模板生成。
//
// 设计文档：mj2x/doc_ai/config_system_design.md

#include "param_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mj2x::cli {
namespace {

using json = nlohmann::json;

// ---------- 简单类型参数回调宏 ----------
// 每个宏生成 cli_<member> / json_<member> 两个函数，供 kParams 表引用。

#define DEF_BOOL(member, flag, jp, desc, grp, req)                                            \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        (void)v;                                                                              \
        (void)e;                                                                              \
        o.member = true;                                                                      \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_boolean()) {                                                                \
            e = std::string("config key must be a boolean: ") + jp;                           \
            return false;                                                                     \
        }                                                                                     \
        o.member = j.get<bool>();                                                             \
        return true;                                                                          \
    }

#define DEF_STR(member, flag, jp, desc, grp, req)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value";                                      \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_string()) {                                                                 \
            e = std::string("config key must be a string: ") + jp;                            \
            return false;                                                                     \
        }                                                                                     \
        o.member = j.get<std::string>();                                                      \
        return true;                                                                          \
    }

#define DEF_INT(member, flag, jp, lo, hi, desc, grp, req)                                     \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value";                                      \
            return false;                                                                     \
        }                                                                                     \
        char*  end  = nullptr;                                                                \
        long long val = std::strtoll(v, &end, 0);                                             \
        if (end == v || *end != '\0' || (lo != 0 && val < lo) || (hi != 0 && val > hi)) {     \
            e = std::string("invalid value for ") + flag + ": " + v + " (expected integer";   \
            if (lo != 0 || hi != 0) {                                                         \
                e += " in [";                                                                 \
                if (lo != 0) e += std::to_string(lo);                                         \
                e += ",";                                                                     \
                if (hi != 0) e += std::to_string(hi);                                         \
                e += "]";                                                                     \
            }                                                                                 \
            e += ")";                                                                         \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<int>(val);                                                     \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_number_integer()) {                                                         \
            e = std::string("config key must be an integer: ") + jp;                          \
            return false;                                                                     \
        }                                                                                     \
        const long long val = j.get<long long>();                                             \
        if ((lo != 0 && val < lo) || (hi != 0 && val > hi)) {                                 \
            e = std::string("value out of range for ") + jp + " (expected";                   \
            if (lo != 0 || hi != 0) {                                                         \
                e += " in [";                                                                 \
                if (lo != 0) e += std::to_string(lo);                                         \
                e += ",";                                                                     \
                if (hi != 0) e += std::to_string(hi);                                         \
                e += "]";                                                                     \
            }                                                                                 \
            e += ")";                                                                         \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<int>(val);                                                     \
        return true;                                                                          \
    }

#define DEF_U64(member, flag, jp, desc, grp, req)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value";                                      \
            return false;                                                                     \
        }                                                                                     \
        char*      end = nullptr;                                                             \
        const unsigned long long val = std::strtoull(v, &end, 0);                             \
        if (end == v || *end != '\0') {                                                       \
            e = std::string("invalid value for ") + flag + ": " + v;                          \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<uint64_t>(val);                                                \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (j.is_number_unsigned()) {                                                         \
            o.member = j.get<uint64_t>();                                                     \
            return true;                                                                      \
        }                                                                                     \
        if (j.is_number_integer() && j.get<long long>() >= 0) {                               \
            o.member = static_cast<uint64_t>(j.get<long long>());                             \
            return true;                                                                      \
        }                                                                                     \
        e = std::string("config key must be a non-negative integer: ") + jp;                  \
        return false;                                                                         \
    }

#define DEF_HEX(member, flag, jp, desc, grp, req)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value";                                      \
            return false;                                                                     \
        }                                                                                     \
        char*  end = nullptr;                                                                 \
        const unsigned long val = std::strtoul(v, &end, 16);                                  \
        if (end == v || *end != '\0') {                                                       \
            e = std::string("invalid hex value for ") + flag + ": " + v;                      \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<std::uint32_t>(val);                                           \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (j.is_string()) {                                                                  \
            const std::string s = j.get<std::string>();                                       \
            char*            end = nullptr;                                                   \
            const unsigned long val = std::strtoul(s.c_str(), &end, 16);                     \
            if (end == s.c_str() || *end != '\0') {                                           \
                e = std::string("invalid hex value for config key ") + jp + ": " + s;         \
                return false;                                                                 \
            }                                                                                 \
            o.member = static_cast<std::uint32_t>(val);                                       \
            return true;                                                                      \
        }                                                                                     \
        if (j.is_number_unsigned()) {                                                         \
            o.member = j.get<std::uint32_t>();                                                \
            return true;                                                                      \
        }                                                                                     \
        e = std::string("config key must be a hex string or number: ") + jp;                  \
        return false;                                                                         \
    }

// 逗号分隔枚举值集合查询。
bool enumContains(const char* values, const std::string& v)
{
    std::istringstream in(values);
    std::string        item;
    while (std::getline(in, item, ',')) {
        if (item == v) return true;
    }
    return false;
}

#define DEF_ENUM_STR(member, flag, jp, values, def, desc, grp, req)                           \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value: " + values;                           \
            return false;                                                                     \
        }                                                                                     \
        if (!enumContains(values, v)) {                                                       \
            e = std::string("Unknown value for ") + flag + ": " + v + " (expected: " +        \
                values + ")";                                                                 \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_string()) {                                                                 \
            e = std::string("config key must be a string: ") + jp;                            \
            return false;                                                                     \
        }                                                                                     \
        const std::string v = j.get<std::string>();                                           \
        if (!enumContains(values, v)) {                                                       \
            e = std::string("invalid value for config key ") + jp + ": " + v +                \
                " (expected: " + values + ")";                                                \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }

#define DEF_STR_LIST(member, flag, jp, desc, grp, req)                                        \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = std::string(flag) + " requires a value";                                      \
            return false;                                                                     \
        }                                                                                     \
        o.member.push_back(v);                                                                \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_array()) {                                                                  \
            e = std::string("config key must be a string array: ") + jp;                      \
            return false;                                                                     \
        }                                                                                     \
        o.member.clear();                                                                     \
        for (const auto& item : j) {                                                          \
            if (!item.is_string()) {                                                          \
                e = std::string("config array contains a non-string item: ") + jp;            \
                return false;                                                                 \
            }                                                                                 \
            o.member.push_back(item.get<std::string>());                                      \
        }                                                                                     \
        return true;                                                                          \
    }

// ---------- 特殊参数回调 ----------

bool cli_input(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "-i requires a value"; return false; }
    o.inputPath = v;
    return true;
}
bool json_input(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: input"; return false; }
    o.inputPath = j.get<std::string>();
    return true;
}

bool cli_output(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "-o requires a value"; return false; }
    o.outputPath = v;
    return true;
}
bool json_output(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: output"; return false; }
    o.outputPath = j.get<std::string>();
    return true;
}

bool parseBackendNameImpl(const std::string& name, CEmitterBackend& backend)
{
    if (name == "stackc" || name == "cbackend" || name == "c") {
        backend = CEmitterBackend::StackC;
        return true;
    }
    if (name == "regc") {
        backend = CEmitterBackend::RegC;
        return true;
    }
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    if (name == "llvm") {
        backend = CEmitterBackend::LlvmIr;
        return true;
    }
#endif
    if (name == "vmc4") {
        backend = CEmitterBackend::Vmc4;
        return true;
    }
    return false;
}

bool cli_backend(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--backend requires a value"; return false; }
    if (!parseBackendNameImpl(v, o.cBackendKind)) {
#ifdef MJ2X_DISABLE_LLVM_BACKEND
        e = "Unknown --backend value. Expected stackc, regc, or vmc4. "
            "Legacy alias cbackend is also accepted.";
#else
        e = "Unknown --backend value. Expected stackc, regc, llvm, or vmc4. "
            "Legacy alias cbackend is also accepted.";
#endif
        return false;
    }
    o.backendSelected = true;
    return true;
}
bool json_backend(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: backend"; return false; }
    if (!parseBackendNameImpl(j.get<std::string>(), o.cBackendKind)) {
#ifdef MJ2X_DISABLE_LLVM_BACKEND
        e = "invalid value for config key backend: " + j.get<std::string>() +
            " (expected: stackc, regc, vmc4)";
#else
        e = "invalid value for config key backend: " + j.get<std::string>() +
            " (expected: stackc, regc, llvm, vmc4)";
#endif
        return false;
    }
    o.backendSelected = true;
    return true;
}

// emit 输出参数：CLI 侧首个显式输出参数清空其他输出（历史行为），
// config 侧不做清空（互斥由校验兜底，与旧 applyConfigOptions 一致）。
bool cli_emit_c(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--emit-c requires a value"; return false; }
    if (!o.cliEmitOutputSeen) {
        o.cOutputPath.clear();
        o.cppOutputPath.clear();
        o.llvmOutputPath.clear();
        o.nativeDirOutput.clear();
        o.cliEmitOutputSeen = true;
    }
    o.cOutputPath = v;
    return true;
}
bool json_emit_c(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: emit_c"; return false; }
    o.cOutputPath = j.get<std::string>();
    return true;
}

bool cli_emit_cpp(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--emit-cpp requires a value"; return false; }
    if (!o.cliEmitOutputSeen) {
        o.cOutputPath.clear();
        o.cppOutputPath.clear();
        o.llvmOutputPath.clear();
        o.nativeDirOutput.clear();
        o.cliEmitOutputSeen = true;
    }
    o.cppOutputPath = v;
    return true;
}
bool json_emit_cpp(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: emit_cpp"; return false; }
    o.cppOutputPath = j.get<std::string>();
    return true;
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
bool cli_emit_llvm(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--emit-llvm requires a value"; return false; }
    if (!o.cliEmitOutputSeen) {
        o.cOutputPath.clear();
        o.cppOutputPath.clear();
        o.llvmOutputPath.clear();
        o.nativeDirOutput.clear();
        o.cliEmitOutputSeen = true;
    }
    o.llvmOutputPath = v;
    return true;
}
bool json_emit_llvm(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: emit_llvm"; return false; }
    o.llvmOutputPath = j.get<std::string>();
    return true;
}
#endif

bool cli_emit_native_dir(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--emit-native-dir requires a value"; return false; }
    if (!o.cliEmitOutputSeen) {
        o.cOutputPath.clear();
        o.cppOutputPath.clear();
        o.llvmOutputPath.clear();
        o.nativeDirOutput.clear();
        o.cliEmitOutputSeen = true;
    }
    o.nativeDirOutput = v;
    return true;
}
bool json_emit_native_dir(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: emit_native_dir"; return false; }
    o.nativeDirOutput = j.get<std::string>();
    return true;
}

bool cli_native_method_prefix(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--native-method-prefix requires a value"; return false; }
    if (!o.cliNativePrefixSeen) {
        o.nativeMethodPrefixes.clear();
        o.cliNativePrefixSeen = true;
    }
    o.nativeMethodPrefixes.push_back(v);
    return true;
}
bool json_native_method_prefix(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_array()) { e = "config key must be a string array: native_method_prefixes"; return false; }
    o.nativeMethodPrefixes.clear();
    for (const auto& item : j) {
        if (!item.is_string()) {
            e = "config array contains a non-string item: native_method_prefixes";
            return false;
        }
        o.nativeMethodPrefixes.push_back(item.get<std::string>());
    }
    return true;
}

bool cli_compile_mode(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--compile-mode requires a value"; return false; }
    if (!parseCompileModeName(v, o.compileMode)) {
        e = "Unknown --compile-mode value. Expected file or pipe.";
        return false;
    }
    o.compileModeSelected = true;
    return true;
}
bool json_compile_mode(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: compile_mode"; return false; }
    if (!parseCompileModeName(j.get<std::string>(), o.compileMode)) {
        e = "invalid value for config key compile_mode: " + j.get<std::string>() +
            " (expected: file, pipe)";
        return false;
    }
    o.compileModeSelected = true;
    return true;
}

bool cli_c_compiler(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--c-compiler requires a value"; return false; }
    o.compilerSelected = true;
    if (v == std::string("xmake")) {
        o.buildDriver = NativeBuildDriver::Xmake;
        return true;
    }
    if (!parseCompilerName(v, o.cCompilerKind)) {
        e = "Unknown --c-compiler value. Expected msvc, gcc, clang, zig, or xmake.";
        return false;
    }
    return true;
}
bool json_c_compiler(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: c_compiler"; return false; }
    const std::string v = j.get<std::string>();
    o.compilerSelected = true;
    if (v == "xmake") {
        o.buildDriver = NativeBuildDriver::Xmake;
        return true;
    }
    if (!parseCompilerName(v, o.cCompilerKind)) {
        e = "invalid value for config key c_compiler: " + v +
            " (expected: msvc, gcc, clang, zig, xmake)";
        return false;
    }
    return true;
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
bool cli_llvm_tool(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--llvm-tool requires a value"; return false; }
    if (!parseLlvmToolName(v, o.llvmTool)) {
        e = "Unknown --llvm-tool value. Expected clang or llc.";
        return false;
    }
    o.llvmToolSelected = true;
    return true;
}
bool json_llvm_tool(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: llvm_tool"; return false; }
    if (!parseLlvmToolName(j.get<std::string>(), o.llvmTool)) {
        e = "invalid value for config key llvm_tool: " + j.get<std::string>() +
            " (expected: clang, llc)";
        return false;
    }
    o.llvmToolSelected = true;
    return true;
}
#endif

bool cli_regc_comment_level(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--regc-comment-level requires a value"; return false; }
    if (v == std::string("none")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::None;
    else if (v == std::string("summary")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Summary;
    else if (v == std::string("detailed")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Detailed;
    else {
        e = "Unknown --regc-comment-level value. Expected none, summary, or detailed.";
        return false;
    }
    return true;
}
bool json_regc_comment_level(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = "config key must be a string: regc_comment_level"; return false; }
    const std::string v = j.get<std::string>();
    if (v == "none") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::None;
    else if (v == "summary") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Summary;
    else if (v == "detailed") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Detailed;
    else {
        e = "invalid value for config key regc_comment_level: " + v +
            " (expected: none, summary, detailed)";
        return false;
    }
    return true;
}

bool cli_regc_safe_env_debug(CliOptions& o, const char* v, std::string& e)
{
    (void)v;
    (void)e;
    // 调试观察模式隐含开启 safe-env（历史行为）。
    o.regcSafeEnv      = true;
    o.regcSafeEnvDebug = true;
    return true;
}
bool json_regc_safe_env_debug(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_boolean()) { e = "config key must be a boolean: regc_safe_env_debug"; return false; }
    o.regcSafeEnvDebug = j.get<bool>();
    if (o.regcSafeEnvDebug) o.regcSafeEnv = true;
    return true;
}

bool cli_vmc4_seed(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--vmc4-seed requires a value"; return false; }
    // 16 进制（0x...）或十进制均可，strtoull 自动识别（历史行为）。
    char*      end = nullptr;
    const unsigned long long val = std::strtoull(v, &end, 0);
    if (end == v || *end != '\0') {
        e = std::string("invalid value for --vmc4-seed: ") + v;
        return false;
    }
    o.vmc4Seed         = static_cast<uint64_t>(val);
    o.vmc4SeedSelected = true;
    return true;
}
bool json_vmc4_seed(CliOptions& o, const json& j, std::string& e)
{
    if (j.is_number_unsigned()) {
        o.vmc4Seed         = j.get<uint64_t>();
        o.vmc4SeedSelected = true;
        return true;
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        char*             end = nullptr;
        const unsigned long long val = std::strtoull(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0') {
            e = "invalid value for config key vmc4_seed: " + s;
            return false;
        }
        o.vmc4Seed         = static_cast<uint64_t>(val);
        o.vmc4SeedSelected = true;
        return true;
    }
    e = "config key must be a number or hex string: vmc4_seed";
    return false;
}

bool cli_native_lib_key(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = "--native-lib-key requires a value"; return false; }
    char*  end = nullptr;
    const unsigned long val = std::strtoul(v, &end, 16);
    if (end == v || *end != '\0') {
        e = std::string("invalid hex value for --native-lib-key: ") + v;
        return false;
    }
    o.nativeLibKey         = static_cast<std::uint32_t>(val);
    o.nativeLibKeySelected = true;
    return true;
}
bool json_native_lib_key(CliOptions& o, const json& j, std::string& e)
{
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        char*             end = nullptr;
        const unsigned long val = std::strtoul(s.c_str(), &end, 16);
        if (end == s.c_str() || *end != '\0') {
            e = "invalid hex value for config key native_lib_key: " + s;
            return false;
        }
        o.nativeLibKey         = static_cast<std::uint32_t>(val);
        o.nativeLibKeySelected = true;
        return true;
    }
    if (j.is_number_unsigned()) {
        o.nativeLibKey         = j.get<std::uint32_t>();
        o.nativeLibKeySelected = true;
        return true;
    }
    e = "config key must be a hex string or number: native_lib_key";
    return false;
}

// ---------- 参数表 ----------
// 顺序即 --help 分组展示顺序。group 变化时打印分组标题。

// global
DEF_STR(nativeAnnotation, "--native-annotation", "native_annotation",
         "只翻译带目标方法注解的方法（JVM descriptor，如 Lmj2x/Native;）", "方法选择", "")
DEF_BOOL(dumpMIR, "--dump-mir", "dump_mir", "打印中间表示（调试用）", "global", "")
DEF_BOOL(compileNative, "--compile", "compile", "把生成的 C/C++ 源码编译为 native library", "编译", "")
DEF_STR(cmakeProjectDir, "--generate-cmake", "generate_cmake",
         "生成独立 CMake native 工程目录", "编译", "")
DEF_STR(xmakeProjectDir, "--generate-xmake", "generate_xmake",
         "生成独立 xmake native 工程目录（推荐；配合 --compile 一键出 DLL/SO）", "编译", "")
DEF_BOOL(regcTraceCalls, "--regc-trace-calls", "regc_trace_calls",
         "RegC: 生成运行时调用日志（[CALL]/[DEEP]，stderr，排查用）", "RegC", "regc")
DEF_BOOL(regcDirectCalls, "--regc-direct-calls", "regc_direct_calls",
         "RegC: 启用 native-to-native 直接 C 调用（默认关闭：直调不产生 JVM 栈帧，getStackTrace 等语义失真）",
         "RegC", "regc")
DEF_BOOL(regcSafeEnv, "--regc-safe-env", "regc_safe_env",
         "RegC: 开启 safe-env（JNI 函数表快照 + 方法入口恢复；捕获时表已污染则 JNI_OnLoad fail closed）",
         "RegC", "regc")
DEF_BOOL(regcNoInline, "--regc-no-inline", "regc_no_inline",
         "RegC: 关闭 SSA 方法内联（默认开启：静态调用 + 无异常区 + 大小/递归门控的语义等价内联）",
         "RegC", "regc")
DEF_BOOL(regcObfuscate, "--regc-obfuscate", "regc_obfuscate",
         "RegC: 开启基础混淆变换（MBA + opaque 谓词 + CFF 平坦化；默认全部关闭，RegC 保持语义直译最快路径）",
         "RegC", "regc")
DEF_INT(regcCffDead, "--regc-cff-dead", "regc_cff_dead", 0, 0,
        "RegC: CFF 恒假死块密度（默认 0 关闭；N = 每 N 个跳转插 1 死块；死块条件依赖运行时 state，编译器无法证明恒假不可 DCE）",
        "RegC", "regc")
DEF_BOOL(regcCffStateEncrypt, "--regc-cff-state-encrypt", "regc_cff_state_encrypt",
         "RegC: CFF 派生随机 state 码 + dispatcher 状态异或（默认关闭；同 seed 同产物）", "RegC", "regc")
DEF_BOOL(regcMbaDeep, "--regc-mba-deep", "regc_mba_deep",
         "RegC: 深 MBA（De Morgan/进位链 + barrier 防折叠，per-site 变体；默认关闭）", "RegC", "regc")
DEF_BOOL(regcOpaqueZero, "--regc-opaque-zero", "regc_opaque_zero",
         "RegC: opaque-零恒等变体（demorgan/shift-pair/or-dec，按种子选变体；默认关闭）", "RegC", "regc")
DEF_U64(regcTransformSeed, "--regc-transform-seed", "regc_transform_seed",
        "RegC: 变换种子（默认 0；决定 state 码/变体选择，同 seed 同产物）", "RegC", "regc")
DEF_ENUM_STR(stackcExceptionCheck, "--stackc-exception-check", "stackc_exception_check",
             "full,lite", "full",
             "StackC pending 异常检查密度（默认 full；lite 仅保留 handler 边界检查）", "StackC", "")
DEF_ENUM_STR(vmc4Dispatcher, "--vmc4-dispatcher", "vmc4_dispatcher",
             "switch_table,funcptr_table,computed_goto,if_nest,state_dispatcher", "switch_table",
             "VMC4(测试性): dispatcher 形态（默认 switch_table；computed_goto 需 GCC/clang 编译）",
             "VMC4", "vmc4")
DEF_ENUM_STR(vmc4Granularity, "--vmc4-granularity", "vmc4_granularity", "class,method,program",
             "class",
             "VMC4: 实例粒度（默认 class；method 每方法独立 VM 实例，最强可变性、体积膨胀最大；program 全程序共享一个 VM 实例，opcode 空间全局统一、体积最优）",
             "VMC4", "vmc4")
DEF_INT(vmc4Alias, "--vmc4-alias", "vmc4_alias", 0, 3,
        "VMC4: 每语义 ALT 别名字节数 0..3（默认 1；0 关闭别名）", "VMC4", "vmc4")
DEF_INT(vmc4Bogus, "--vmc4-bogus", "vmc4_bogus", 0, 4,
        "VMC4: 注入的不可达假 handler 数 0..4（默认 1；0 关闭，只污染静态分析不参与执行）",
        "VMC4", "vmc4")
DEF_INT(vmc4Drift, "--vmc4-drift", "vmc4_drift", 0, 4,
        "VMC4: 执行期 dispatch 变体数 0..4（默认 0 关闭；开启后语义标签按字节偏移周期轮转，unmap 表解码）",
        "VMC4", "vmc4")
DEF_INT(vmc4DriftPeriod, "--vmc4-drift-period", "vmc4_drift_period", 1, 0,
        "VMC4: drift 变体切换周期（字节偏移单位，默认 16）", "VMC4", "vmc4")
DEF_STR(vmc4TraceSurface, "--vmc4-trace-surface", "vmc4_trace_surface",
        "VMC4: 写 expected trace surface 文件（mjdbg 红队契约，同 seed+同输入 → 同内容）", "VMC4", "vmc4")
DEF_ENUM_STR(runtimeProtection, "--runtime-protection", "runtime_protection",
             "none,log,delay,fake,terminate", "none",
             "RegC/VMC4: 方法入口反侦测检查点（默认 none 不注入；依赖 mj2x-crt rt_protect 模块）",
             "运行时保护", "regc|vmc4")
DEF_INT(runtimeProtectionStep, "--runtime-protection-step", "runtime_protection_step", 1, 0,
        "检查点概率触发步长（默认 16）", "运行时保护", "regc|vmc4")
DEF_INT(runtimeProtectionDelayMs, "--runtime-protection-delay", "runtime_protection_delay_ms", 0, 0,
        "Delay 动作睡眠毫秒数（默认 500）", "运行时保护", "regc|vmc4")
DEF_STR(nativeLibPackHook, "--native-lib-pack-hook", "native_lib_pack_hook",
        "嵌入前外部处理钩子（sepacker DLL 加壳等）：{in} 占位符替换为编译产物路径，hook 应就地改写该文件；非零退出码视为失败；顺序：hook → 可选加密 → 嵌入（默认关闭）",
        "native lib", "")
DEF_STR(intrinsicConfigPath, "--intrinsic-config", "intrinsic_config",
        "用户自定义 intrinsic JSON（issue #818）：{\"intrinsics\":[{owner,name,desc,symbol,...}],\"sources\":[...]}",
        "intrinsic", "")
DEF_STR_LIST(intrinsicSources, "--intrinsic-src", "intrinsic_src",
             "追加用户 intrinsic 实现源文件（可重复；并入生成工程编译）", "intrinsic", "")
DEF_BOOL(quiet, "--quiet", "log_quiet", "日志只保留警告与错误（stderr）", "日志", "")
DEF_BOOL(verbose, "--verbose", "log_verbose", "日志提升到 debug 级别（stderr）", "日志", "")
DEF_U64(clientId, "--client-id", "client_id",
        "per-client 唯一派生（默认 0 = 不变：产物与历史逐字节一致；非 0 时所有后端 seed 经 splitmix64(baseSeed ^ clientId) 派生，每个客户端得到唯一 ISA/变体/state 码产物；同 clientId 同 seed 同产物）",
        "client", "")

}   // namespace

// ---------- 注册表数组（顺序即 --help 展示顺序） ----------

#define PARAM(id_, flag_, jp_, type_, enum_, lo_, hi_, def_, desc_, grp_, req_, cliFn, jsonFn) \
    { id_, flag_, jp_, type_, enum_, lo_, hi_, def_, desc_, grp_, req_, cliFn, jsonFn }

const ParamDef kParamTable[] = {
    // global
    PARAM("global.input", "-i", "input", ParamType::String, nullptr, 0, 0, "",
          "输入文件（.class / .jar）", "global", "", cli_input, json_input),
    PARAM("global.output", "-o", "output", ParamType::String, nullptr, 0, 0, "",
          "输出文件（.class / .jar，方法被标记为 native）", "global", "", cli_output, json_output),
#ifdef MJ2X_DISABLE_LLVM_BACKEND
    PARAM("global.backend", "--backend", "backend", ParamType::Enum, "stackc,regc,vmc4",
          0, 0, "stackc",
          "选择 native 发射后端（stackc 默认；cbackend/c 为兼容别名）", "global", "",
          cli_backend, json_backend),
#else
    PARAM("global.backend", "--backend", "backend", ParamType::Enum, "stackc,regc,llvm,vmc4",
          0, 0, "stackc",
          "选择 native 发射后端（stackc 默认；cbackend/c 为兼容别名）", "global", "",
          cli_backend, json_backend),
#endif
    PARAM("global.emit_c", "--emit-c", "emit_c", ParamType::String, nullptr, 0, 0, "",
          "输出的 C 源码路径", "global", "", cli_emit_c, json_emit_c),
    PARAM("global.emit_cpp", "--emit-cpp", "emit_cpp", ParamType::String, nullptr, 0, 0, "",
          "输出 RegC C++17 源码路径（需 .cpp 后缀 + --backend regc）", "global", "",
          cli_emit_cpp, json_emit_cpp),
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    PARAM("global.emit_llvm", "--emit-llvm", "emit_llvm", ParamType::String, nullptr, 0, 0, "",
          "输出 LLVM textual IR 路径（需 .ll 后缀 + --backend llvm）", "global", "",
          cli_emit_llvm, json_emit_llvm),
#endif
    PARAM("global.emit_native_dir", "--emit-native-dir", "emit_native_dir", ParamType::String,
          nullptr, 0, 0, "",
          "RegC: 多文件输出（mj2x_regc_api.h + runtime.c + 每类 m_<Class>.c + entry.c）",
          "global", "", cli_emit_native_dir, json_emit_native_dir),
    PARAM("global.dump_mir", "--dump-mir", "dump_mir", ParamType::Bool, nullptr, 0, 0, "false",
          "打印中间表示（调试用）", "global", "", cli_dumpMIR, json_dumpMIR),
    // selection
    PARAM("selection.native_annotation", "--native-annotation", "native_annotation",
          ParamType::String, nullptr, 0, 0, "",
          "只翻译带目标方法注解的方法（JVM descriptor，如 Lmj2x/Native;）", "方法选择", "",
          cli_nativeAnnotation, json_nativeAnnotation),
    PARAM("selection.native_method_prefixes", "--native-method-prefix",
          "native_method_prefixes", ParamType::StringList, nullptr, 0, 0, "",
          "只翻译 owner.name 以指定前缀开头的方法（可重复）", "方法选择", "",
          cli_native_method_prefix, json_native_method_prefix),
    // compile
    PARAM("compile.enabled", "--compile", "compile", ParamType::Bool, nullptr, 0, 0, "false",
          "把生成的 C/C++ 源码编译为 native library（位于 --output 旁）", "编译", "",
          cli_compileNative, json_compileNative),
    PARAM("compile.mode", "--compile-mode", "compile_mode", ParamType::Enum, "file,pipe", 0, 0,
          "file", "native 编译输入模式", "编译", "", cli_compile_mode, json_compile_mode),
    PARAM("compile.c_compiler", "--c-compiler", "c_compiler", ParamType::Enum,
          "msvc,gcc,clang,zig,xmake", 0, 0, "auto",
          "C 编译器或构建驱动（xmake 为构建驱动）", "编译", "", cli_c_compiler, json_c_compiler),
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    PARAM("compile.llvm_tool", "--llvm-tool", "llvm_tool", ParamType::Enum, "clang,llc", 0, 0,
          "clang", "LLVM IR 编译路径", "编译", "", cli_llvm_tool, json_llvm_tool),
#endif
    PARAM("compile.generate_cmake", "--generate-cmake", "generate_cmake", ParamType::String,
          nullptr, 0, 0, "", "生成独立 CMake native 工程目录", "编译", "",
          cli_cmakeProjectDir, json_cmakeProjectDir),
    PARAM("compile.generate_xmake", "--generate-xmake", "generate_xmake", ParamType::String,
          nullptr, 0, 0, "", "生成独立 xmake native 工程目录（推荐；配合 --compile 一键出 DLL/SO）",
          "编译", "", cli_xmakeProjectDir, json_xmakeProjectDir),
    // stackc
    PARAM("stackc.exception_check", "--stackc-exception-check", "stackc_exception_check",
          ParamType::Enum, "full,lite", 0, 0, "full",
          "StackC pending 异常检查密度（lite 仅保留 handler 边界检查）", "StackC", "stackc",
          cli_stackcExceptionCheck, json_stackcExceptionCheck),
    // regc
    PARAM("regc.trace_calls", "--regc-trace-calls", "regc_trace_calls", ParamType::Bool, nullptr,
          0, 0, "false", "RegC: 生成运行时调用日志（[CALL]/[DEEP]，stderr，排查用）", "RegC", "regc",
          cli_regcTraceCalls, json_regcTraceCalls),
    PARAM("regc.direct_calls", "--regc-direct-calls", "regc_direct_calls", ParamType::Bool,
          nullptr, 0, 0, "false",
          "RegC: 启用 native-to-native 直接 C 调用（默认关闭：直调不产生 JVM 栈帧，getStackTrace 等语义失真）",
          "RegC", "regc", cli_regcDirectCalls, json_regcDirectCalls),
    PARAM("regc.comment_level", "--regc-comment-level", "regc_comment_level", ParamType::Enum,
          "none,summary,detailed", 0, 0, "summary", "RegC: 生成注释级别", "RegC", "regc",
          cli_regc_comment_level, json_regc_comment_level),
    PARAM("regc.safe_env", "--regc-safe-env", "regc_safe_env", ParamType::Bool, nullptr, 0, 0,
          "false",
          "RegC: 开启 safe-env（JNI 函数表快照 + 方法入口恢复；捕获时表已污染则 JNI_OnLoad fail closed）",
          "RegC", "regc", cli_regcSafeEnv, json_regcSafeEnv),
    PARAM("regc.safe_env_debug", "--regc-safe-env-debug", "regc_safe_env_debug", ParamType::Bool,
          nullptr, 0, 0, "false",
          "RegC: safe-env 调试观察模式（检出 hook 只打告警不 fail，隐含开启 --regc-safe-env）",
          "RegC", "regc", cli_regc_safe_env_debug, json_regc_safe_env_debug),
    PARAM("regc.no_inline", "--regc-no-inline", "regc_no_inline", ParamType::Bool, nullptr, 0, 0,
          "false",
          "RegC: 关闭 SSA 方法内联（默认开启：静态调用 + 无异常区 + 大小/递归门控的语义等价内联，消除 native 方法间 JNI 往返）",
          "RegC", "regc", cli_regcNoInline, json_regcNoInline),
    PARAM("regc.obfuscate", "--regc-obfuscate", "regc_obfuscate", ParamType::Bool, nullptr, 0, 0,
          "false",
          "RegC/LLVM: 开启基础混淆变换（MBA + opaque 谓词 + CFF 平坦化；RegIR 级与 RegC 同源；默认全部关闭保持语义直译最快路径；LLVM 侧变换产物超子集时回退未变换）",
          "RegC/LLVM", "regc|llvm", cli_regcObfuscate, json_regcObfuscate),
    PARAM("regc.cff_dead", "--regc-cff-dead", "regc_cff_dead", ParamType::Int, nullptr, 0, 0,
          "0",
          "RegC/LLVM: CFF 恒假死块密度（默认 0 关闭；N = 每 N 个跳转插 1 死块；死块条件依赖运行时 state，编译器无法证明恒假不可 DCE；注入 barrier 的方法在 LLVM 侧回退未变换）",
          "RegC/LLVM", "regc|llvm", cli_regcCffDead, json_regcCffDead),
    PARAM("regc.cff_state_encrypt", "--regc-cff-state-encrypt", "regc_cff_state_encrypt",
          ParamType::Bool, nullptr, 0, 0, "false",
          "RegC/LLVM: CFF 派生随机 state 码 + dispatcher 状态异或（默认关闭；同 seed 同产物）",
          "RegC/LLVM", "regc|llvm", cli_regcCffStateEncrypt, json_regcCffStateEncrypt),
    PARAM("regc.mba_deep", "--regc-mba-deep", "regc_mba_deep", ParamType::Bool, nullptr, 0, 0,
          "false",
          "RegC: 深 MBA（De Morgan/进位链 + barrier 防折叠，per-site 变体；默认关闭；LLVM 侧 barrier 超出子集，变换回退未变换）",
          "RegC/LLVM", "regc|llvm", cli_regcMbaDeep, json_regcMbaDeep),
    PARAM("regc.opaque_zero", "--regc-opaque-zero", "regc_opaque_zero", ParamType::Bool, nullptr,
          0, 0, "false",
          "RegC/LLVM: opaque-零恒等变体（demorgan/shift-pair/or-dec，按种子选变体；默认关闭）",
          "RegC/LLVM", "regc|llvm", cli_regcOpaqueZero, json_regcOpaqueZero),
    PARAM("regc.transform_seed", "--regc-transform-seed", "regc_transform_seed",
          ParamType::UInt64, nullptr, 0, 0, "0",
          "RegC/LLVM: 变换种子（默认 0；决定 state 码/变体选择，同 seed 同产物）", "RegC/LLVM",
          "regc|llvm", cli_regcTransformSeed, json_regcTransformSeed),
    // vmc4
    PARAM("vmc4.dispatcher", "--vmc4-dispatcher", "vmc4_dispatcher", ParamType::Enum,
          "switch_table,funcptr_table,computed_goto,if_nest,state_dispatcher", 0, 0,
          "switch_table",
          "VMC4(测试性): dispatcher 形态（默认 switch_table；computed_goto 需 GCC/clang 编译）",
          "VMC4", "vmc4", cli_vmc4Dispatcher, json_vmc4Dispatcher),
    PARAM("vmc4.seed", "--vmc4-seed", "vmc4_seed", ParamType::Hex, nullptr, 0, 0, "0",
          "VMC4: 主 seed，驱动每实例 opcode 映射/handler 顺序（同 seed 同输入 → 同产物）", "VMC4",
          "vmc4", cli_vmc4_seed, json_vmc4_seed),
    PARAM("vmc4.granularity", "--vmc4-granularity", "vmc4_granularity", ParamType::Enum,
          "class,method,program", 0, 0, "class",
          "VMC4: 实例粒度（class 默认；method 每方法独立 VM 实例最强可变性；program 全程序共享一个 VM 实例体积最优）",
          "VMC4", "vmc4", cli_vmc4Granularity, json_vmc4Granularity),
    PARAM("vmc4.alias", "--vmc4-alias", "vmc4_alias", ParamType::Int, nullptr, 0, 3, "1",
          "VMC4: 每语义 ALT 别名字节数 0..3（0 关闭别名）", "VMC4", "vmc4", cli_vmc4Alias,
          json_vmc4Alias),
    PARAM("vmc4.bogus", "--vmc4-bogus", "vmc4_bogus", ParamType::Int, nullptr, 0, 4, "1",
          "VMC4: 注入的不可达假 handler 数 0..4（0 关闭；只污染静态分析不参与执行）", "VMC4",
          "vmc4", cli_vmc4Bogus, json_vmc4Bogus),
    PARAM("vmc4.drift", "--vmc4-drift", "vmc4_drift", ParamType::Int, nullptr, 0, 4, "0",
          "VMC4: 执行期 dispatch 变体数 0..4（0 关闭；开启后语义标签按字节偏移周期轮转，unmap 表解码）",
          "VMC4", "vmc4", cli_vmc4Drift, json_vmc4Drift),
    PARAM("vmc4.drift_period", "--vmc4-drift-period", "vmc4_drift_period", ParamType::Int,
          nullptr, 1, 0, "16", "VMC4: drift 变体切换周期（字节偏移单位）", "VMC4", "vmc4",
          cli_vmc4DriftPeriod, json_vmc4DriftPeriod),
    PARAM("vmc4.trace_surface", "--vmc4-trace-surface", "vmc4_trace_surface", ParamType::String,
          nullptr, 0, 0, "",
          "VMC4: 写 expected trace surface 文件（mjdbg 红队契约，同 seed+同输入 → 同内容）",
          "VMC4", "vmc4", cli_vmc4TraceSurface, json_vmc4TraceSurface),
    // runtime
    PARAM("runtime.protection", "--runtime-protection", "runtime_protection", ParamType::Enum,
          "none,log,delay,fake,terminate", 0, 0, "none",
          "RegC/VMC4/LLVM: 方法入口反侦测检查点（默认 none 不注入；依赖 mj2x-crt rt_protect 模块；LLVM 侧编译时附带 rt_protect 源）",
          "运行时保护", "regc|vmc4|llvm", cli_runtimeProtection, json_runtimeProtection),
    PARAM("runtime.protection_step", "--runtime-protection-step", "runtime_protection_step",
          ParamType::Int, nullptr, 1, 0, "16", "检查点概率触发步长", "运行时保护",
          "regc|vmc4|llvm", cli_runtimeProtectionStep, json_runtimeProtectionStep),
    PARAM("runtime.protection_delay_ms", "--runtime-protection-delay",
          "runtime_protection_delay_ms", ParamType::Int, nullptr, 0, 0, "500",
          "Delay 动作睡眠毫秒数", "运行时保护", "regc|vmc4|llvm", cli_runtimeProtectionDelayMs,
          json_runtimeProtectionDelayMs),
    // nativeLib
    PARAM("native_lib.key", "--native-lib-key", "native_lib_key", ParamType::Hex, nullptr, 0, 0,
          "",
          "native lib 加密存储：嵌入前 XOR 加密（xorshift32 密钥流），资源入口加 .enc 后缀，运行时 NativeLoader 解密加载；静态提取不可直接加载（默认关闭 = 明文嵌入）",
          "native lib", "", cli_native_lib_key, json_native_lib_key),
    PARAM("native_lib.pack_hook", "--native-lib-pack-hook", "native_lib_pack_hook",
          ParamType::String, nullptr, 0, 0, "",
          "嵌入前外部处理钩子（sepacker DLL 加壳等）：{in} 占位符替换为编译产物路径，hook 应就地改写该文件；非零退出码视为失败；顺序：hook → 可选加密 → 嵌入（默认关闭）",
          "native lib", "", cli_nativeLibPackHook, json_nativeLibPackHook),
    // client
    PARAM("client.id", "--client-id", "client_id", ParamType::UInt64, nullptr, 0, 0, "0",
          "per-client 唯一派生（默认 0 = 不变：产物与历史逐字节一致；非 0 时所有后端 seed 经 splitmix64(baseSeed ^ clientId) 派生，每个客户端得到唯一 ISA/变体/state 码产物；同 clientId 同 seed 同产物）",
          "client", "", cli_clientId, json_clientId),
    // intrinsic
    PARAM("intrinsic.config", "--intrinsic-config", "intrinsic_config", ParamType::String,
          nullptr, 0, 0, "",
          "用户自定义 intrinsic JSON（issue #818）：{\"intrinsics\":[{owner,name,desc,symbol,...}],\"sources\":[...]}",
          "intrinsic", "", cli_intrinsicConfigPath, json_intrinsicConfigPath),
    PARAM("intrinsic.sources", "--intrinsic-src", "intrinsic_src", ParamType::StringList,
          nullptr, 0, 0, "", "追加用户 intrinsic 实现源文件（可重复；并入生成工程编译）",
          "intrinsic", "", cli_intrinsicSources, json_intrinsicSources),
    // log
    PARAM("log.quiet", "--quiet", "log_quiet", ParamType::Bool, nullptr, 0, 0, "false",
          "日志只保留警告与错误（stderr）", "日志", "", cli_quiet, json_quiet),
    PARAM("log.verbose", "--verbose", "log_verbose", ParamType::Bool, nullptr, 0, 0, "false",
          "日志提升到 debug 级别（stderr）", "日志", "", cli_verbose, json_verbose),
};

#undef PARAM

const ParamDef* kParams     = kParamTable;
const std::size_t kParamCount = sizeof(kParamTable) / sizeof(kParamTable[0]);

const ParamDef* findParamByCli(const std::string& cliFlag)
{
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (cliFlag == kParams[i].cliFlag) return &kParams[i];
    }
    return nullptr;
}

const ParamDef* findParamByJsonPath(const std::string& jsonPath)
{
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (jsonPath == kParams[i].jsonPath) return &kParams[i];
    }
    return nullptr;
}

bool parseBackendName(const std::string& name, CEmitterBackend& backend)
{
    return parseBackendNameImpl(name, backend);
}

// ---------- JSON Schema 构建（--dump-config-schema） ----------

nlohmann::json buildConfigSchemaJson()
{
    json props = json::object();
    for (std::size_t i = 0; i < kParamCount; ++i) {
        const ParamDef& p = kParams[i];
        json            node;
        node["description"] = p.description;
        switch (p.type) {
        case ParamType::Bool:
            node["type"]    = "boolean";
            node["default"] = (std::string(p.defaultStr) == "true");
            break;
        case ParamType::Int:
            node["type"] = "integer";
            if (p.minValue != 0) node["minimum"] = p.minValue;
            if (p.maxValue != 0) node["maximum"] = p.maxValue;
            node["default"] = p.defaultStr[0] ? std::stoll(p.defaultStr) : 0;
            break;
        case ParamType::UInt64:
            node["type"]    = "integer";
            node["minimum"] = 0;
            node["default"] = p.defaultStr[0] ? std::stoull(p.defaultStr) : 0;
            break;
        case ParamType::Hex:
            node["type"]    = "string";
            node["pattern"] = "^[0-9a-fA-F]+$";
            node["default"] = p.defaultStr;
            break;
        case ParamType::Enum: {
            node["type"] = "string";
            json         arr = json::array();
            std::istringstream in(p.enumValues);
            std::string        item;
            while (std::getline(in, item, ',')) arr.push_back(item);
            node["enum"]    = arr;
            node["default"] = p.defaultStr;
            break;
        }
        case ParamType::String:
            node["type"]    = "string";
            node["default"] = p.defaultStr;
            break;
        case ParamType::StringList:
            node["type"]    = "array";
            node["items"]   = json{{"type", "string"}};
            node["default"] = json::array();
            break;
        }
        if (p.requires && p.requires[0]) node["x-requires"] = p.requires;
        props[p.jsonPath] = std::move(node);
    }
    return json{
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"$id", "https://morpher.dev/schema/mj2x-config.schema.json"},
        {"title", "mj2x config"},
        {"description", "mj2x native protection configuration"},
        {"type", "object"},
        {"properties", std::move(props)},
        {"additionalProperties", false},
    };
}

// ---------- --help 分组输出 ----------

void printHelp()
{
    printf("Usage: mj2x-cli -i <input.class|input.jar> -o <output.class|output.jar> [options]\n");
    printf("Options:\n");
    printf("  --config <file>              Load JSON options; command line values override it\n");
    printf("  --dump-config-schema [<file>]  Write config JSON Schema (draft-07) to file or stdout\n");
    printf("  --dump-config-template [<file>] Write commented config template to file or stdout\n");

    std::size_t width = 2;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        width = std::max(width, std::strlen(kParams[i].cliFlag));
    }
    const char* currentGroup = nullptr;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        const ParamDef& p = kParams[i];
        if (!currentGroup || std::strcmp(currentGroup, p.group) != 0) {
            currentGroup = p.group;
            printf("\n  [%s]\n", p.group);
        }
        printf("  %-*s  %s\n", static_cast<int>(width), p.cliFlag, p.description);
    }
    printf("\n  --help                       Print help\n");
}

// ---------- 配置模板生成（--dump-config-template，JSONC） ----------

std::string buildConfigTemplateText()
{
    std::ostringstream out;
    out << "// mj2x 配置模板（--dump-config-template 生成；键说明见 --dump-config-schema）\n"
           "// 删除注释即得合法配置；未列出的键保持默认值；未知键会被拒绝。\n"
           "{\n";
    for (std::size_t i = 0; i < kParamCount; ++i) {
        const ParamDef& p = kParams[i];
        std::string     value;
        switch (p.type) {
        case ParamType::Bool:
            value = (std::string(p.defaultStr) == "true") ? "true" : "false";
            break;
        case ParamType::Int:
        case ParamType::UInt64:
            value = p.defaultStr[0] ? p.defaultStr : "0";
            break;
        case ParamType::Hex:
        case ParamType::Enum:
        case ParamType::String:
            value = std::string("\"") + p.defaultStr + "\"";
            break;
        case ParamType::StringList:
            value = "[]";
            break;
        }
        out << "  \"" << p.jsonPath << "\": " << value;
        if (i + 1 < kParamCount) out << ",";
        out << "  // " << p.id;
        if (p.requires && p.requires[0]) out << "（requires: " << p.requires << "）";
        out << "\n";
    }
    out << "}\n";
    return out.str();
}

}   // namespace mj2x::cli


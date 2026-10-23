// param_registry.cpp
//
// 参数注册表单源实现（配置系统主线十二）：基表声明、CLI/JSON 解析回调、
// JSON Schema 构建、--help 分组输出、配置模板生成。
//
// 完整参数表 = 静态基表（global/selection/compile + strings/runtime/nativeLib/
// client/intrinsic/log）+ 各后端接线单元的参数段（backend_registry 注册序），
// 首次访问时拼装（main() 启动后，全部接线单元已完成静态注册）。行序即
// --help 分组/schema 属性/模板键序，单后端构建被裁后端段自然缺席。
//
// 设计文档：mj2x/doc_ai/config_system_design.md

#include "param_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "backend_registry.hpp"
#include "param_registry_internal.hpp"
#include "mj2x/preprocessor/native_class_filter_preprocessor.hpp"
#include "libca/i18n/i18n.hpp"

#ifdef _WIN32
// param_registry.hpp → native_compiler.hpp → native_process_timeout.hpp 传递
// 引入的 libca/process/subprocess.hpp 为保护 Command 同名成员 #undef 了全局
// stdout 宏；恢复 MSVC CRT 语义（printHelp 分组输出的 fputs 需要）。
#    include <cstdio>
#    define stdout (__acrt_iob_func(1))
#endif

namespace mj2x::cli {
namespace {

using json = nlohmann::json;

// ---------- 特殊参数回调 ----------

bool cli_input(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"-i"}); return false; }
    o.inputPath = v;
    return true;
}
bool json_input(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"input"}); return false; }
    o.inputPath = j.get<std::string>();
    return true;
}

bool cli_output(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"-o"}); return false; }
    o.outputPath = v;
    return true;
}
bool json_output(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"output"}); return false; }
    o.outputPath = j.get<std::string>();
    return true;
}

// backend 枚举串注册表单源（拼装期回填 global.backend 行）；CLI 名与兼容
// 别名（cbackend/c → stackc）由各后端模块自报，被裁后端不可解析（#886）。
bool parseBackendNameImpl(const std::string& name, CEmitterBackend& backend)
{
    const BackendModule* module = findBackendModuleByCliName(name);
    if (module == nullptr) return false;
    backend = module->kind;
    return true;
}

bool cli_backend(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--backend"}); return false; }
    if (!parseBackendNameImpl(v, o.cBackendKind)) {
        if (findBackendModule(CEmitterBackend::StackC) == nullptr)
            e = param_internal::trfs("mj2x.cli.err.unknown_flag_value", {"--backend", enabledBackendCliList()});
        else
            e = param_internal::trfs("mj2x.cli.err.unknown_flag_value",
                     {"--backend", enabledBackendCliList() + " (cbackend/c also accepted)"});
        return false;
    }
    o.backendSelected = true;
    return true;
}
bool json_backend(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"backend"}); return false; }
    if (!parseBackendNameImpl(j.get<std::string>(), o.cBackendKind)) {
        e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum",
                 {"backend", j.get<std::string>(), enabledBackendCliList()});
        return false;
    }
    o.backendSelected = true;
    return true;
}

// emit 输出参数：CLI 侧首个显式输出参数清空其他输出（历史行为），
// config 侧不做清空（互斥由校验兜底，与旧 applyConfigOptions 一致）。
bool cli_emit_c(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--emit-c"}); return false; }
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
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"emit_c"}); return false; }
    o.cOutputPath = j.get<std::string>();
    return true;
}

bool cli_emit_cpp(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--emit-cpp"}); return false; }
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
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"emit_cpp"}); return false; }
    o.cppOutputPath = j.get<std::string>();
    return true;
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
bool cli_emit_llvm(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--emit-llvm"}); return false; }
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
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"emit_llvm"}); return false; }
    o.llvmOutputPath = j.get<std::string>();
    return true;
}
#endif

bool cli_emit_native_dir(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--emit-native-dir"}); return false; }
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
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"emit_native_dir"}); return false; }
    o.nativeDirOutput = j.get<std::string>();
    return true;
}

bool cli_native_method_prefix(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--native-method-prefix"}); return false; }
    if (!o.cliNativePrefixSeen) {
        o.nativeMethodPrefixes.clear();
        o.cliNativePrefixSeen = true;
    }
    o.nativeMethodPrefixes.push_back(v);
    return true;
}
bool json_native_method_prefix(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_array()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_strarr", {"native_method_prefixes"}); return false; }
    o.nativeMethodPrefixes.clear();
    for (const auto& item : j) {
        if (!item.is_string()) {
            e = param_internal::trfs("mj2x.cli.err.cfg_arr_nonstr", {"native_method_prefixes"});
            return false;
        }
        o.nativeMethodPrefixes.push_back(item.get<std::string>());
    }
    return true;
}

// 类级 glob 过滤器（--native-include/--native-exclude）：CLI 值可逗号分隔
// （对标 jvmtp filter.* 在 properties 里的形态一致），`/` 统一化为 `.`。零片段报错
// 由 "a,,b" 之类的空片段默认模式。
bool cli_native_class_glob(CliOptions& o, const char* v, const char* flag, bool include,
                           std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag}); return false; }
    bool& seen = include ? o.cliNativeIncludeSeen : o.cliNativeExcludeSeen;
    if (!seen) {
        (include ? o.nativeClassIncludes : o.nativeClassExcludes).clear();
        seen = true;
    }
    std::vector<std::string> globs;
    if (!normalizeClassGlobs({std::string(v)}, globs)) {
        e = param_internal::trfs("mj2x.cli.err.class_glob_empty", {flag});
        return false;
    }
    auto& target = include ? o.nativeClassIncludes : o.nativeClassExcludes;
    for (auto& glob : globs) target.push_back(std::move(glob));
    return true;
}
bool json_native_class_glob(CliOptions& o, const json& j, const char* key, bool include,
                            std::string& e)
{
    if (!j.is_array()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_strarr", {key}); return false; }
    std::vector<std::string> raw;
    for (const auto& item : j) {
        if (!item.is_string()) {
            e = param_internal::trfs("mj2x.cli.err.cfg_arr_nonstr", {key});
            return false;
        }
        raw.push_back(item.get<std::string>());
    }
    std::vector<std::string> globs;
    if (!normalizeClassGlobs(raw, globs)) {
        e = param_internal::trfs("mj2x.cli.err.class_glob_empty", {key});
        return false;
    }
    auto& target = include ? o.nativeClassIncludes : o.nativeClassExcludes;
    target      = std::move(globs);
    return true;
}
// 混淆作用域 glob（--obf-scope-include/--obf-scope-exclude，issue #8）。语义与
// --native-include/exclude 完全一致（同一套 classPassesFilter），只作用在"要不要
// 施加 RegIR 变换"而非"进不进编译集"。
bool cli_obf_scope_glob(CliOptions& o, const char* v, const char* flag, bool include,
                        std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag}); return false; }
    bool& seen = include ? o.cliObfScopeIncludeSeen : o.cliObfScopeExcludeSeen;
    if (!seen) {
        (include ? o.obfScopeIncludes : o.obfScopeExcludes).clear();
        seen = true;
    }
    std::vector<std::string> globs;
    if (!normalizeClassGlobs({std::string(v)}, globs)) {
        e = param_internal::trfs("mj2x.cli.err.class_glob_empty", {flag});
        return false;
    }
    auto& target = include ? o.obfScopeIncludes : o.obfScopeExcludes;
    for (auto& glob : globs) target.push_back(std::move(glob));
    return true;
}
bool json_obf_scope_glob(CliOptions& o, const json& j, const char* key, bool include,
                         std::string& e)
{
    if (!j.is_array()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_strarr", {key}); return false; }
    std::vector<std::string> raw;
    for (const auto& item : j) {
        if (!item.is_string()) {
            e = param_internal::trfs("mj2x.cli.err.cfg_arr_nonstr", {key});
            return false;
        }
        raw.push_back(item.get<std::string>());
    }
    std::vector<std::string> globs;
    if (!normalizeClassGlobs(raw, globs)) {
        e = param_internal::trfs("mj2x.cli.err.class_glob_empty", {key});
        return false;
    }
    auto& target = include ? o.obfScopeIncludes : o.obfScopeExcludes;
    target      = std::move(globs);
    return true;
}
bool cli_obf_scope_include(CliOptions& o, const char* v, std::string& e)
{
    return cli_obf_scope_glob(o, v, "--obf-scope-include", true, e);
}
bool json_obf_scope_include(CliOptions& o, const json& j, std::string& e)
{
    return json_obf_scope_glob(o, j, "obf_scope_include", true, e);
}
bool cli_obf_scope_exclude(CliOptions& o, const char* v, std::string& e)
{
    return cli_obf_scope_glob(o, v, "--obf-scope-exclude", false, e);
}
bool json_obf_scope_exclude(CliOptions& o, const json& j, std::string& e)
{
    return json_obf_scope_glob(o, j, "obf_scope_exclude", false, e);
}

bool cli_native_include(CliOptions& o, const char* v, std::string& e)
{
    return cli_native_class_glob(o, v, "--native-include", true, e);
}
bool json_native_include(CliOptions& o, const json& j, std::string& e)
{
    return json_native_class_glob(o, j, "native_include", true, e);
}
bool cli_native_exclude(CliOptions& o, const char* v, std::string& e)
{
    return cli_native_class_glob(o, v, "--native-exclude", false, e);
}
bool json_native_exclude(CliOptions& o, const json& j, std::string& e)
{
    return json_native_class_glob(o, j, "native_exclude", false, e);
}

bool cli_compile_mode(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--compile-mode"}); return false; }
    if (!parseCompileModeName(v, o.compileMode)) {
        e = param_internal::trfs("mj2x.cli.err.unknown_flag_value", {"--compile-mode", "file or pipe"});
        return false;
    }
    o.compileModeSelected = true;
    return true;
}
bool json_compile_mode(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"compile_mode"}); return false; }
    if (!parseCompileModeName(j.get<std::string>(), o.compileMode)) {
        e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum", {"compile_mode", j.get<std::string>(), "file, pipe"});
        return false;
    }
    o.compileModeSelected = true;
    return true;
}

bool cli_c_compiler(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--c-compiler"}); return false; }
    o.compilerSelected = true;
    if (v == std::string("xmake")) {
        o.buildDriver = NativeBuildDriver::Xmake;
        return true;
    }
    if (!parseCompilerName(v, o.cCompilerKind)) {
        e = param_internal::trfs("mj2x.cli.err.unknown_flag_value", {"--c-compiler", "auto, msvc, gcc, clang, zig, or xmake"});
        return false;
    }
    return true;
}
bool json_c_compiler(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"c_compiler"}); return false; }
    const std::string v = j.get<std::string>();
    o.compilerSelected = true;
    if (v == "xmake") {
        o.buildDriver = NativeBuildDriver::Xmake;
        return true;
    }
    if (!parseCompilerName(v, o.cCompilerKind)) {
        e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum",
                {"c_compiler", v, "auto, msvc, gcc, clang, zig, xmake"});
        return false;
    }
    return true;
}

#ifndef MJ2X_DISABLE_LLVM_BACKEND
bool cli_llvm_tool(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--llvm-tool"}); return false; }
    if (!parseLlvmToolName(v, o.llvmTool)) {
        e = param_internal::trfs("mj2x.cli.err.unknown_flag_value", {"--llvm-tool", "clang or llc"});
        return false;
    }
    o.llvmToolSelected = true;
    return true;
}
bool json_llvm_tool(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"llvm_tool"}); return false; }
    if (!parseLlvmToolName(j.get<std::string>(), o.llvmTool)) {
        e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum", {"llvm_tool", j.get<std::string>(), "clang, llc"});
        return false;
    }
    o.llvmToolSelected = true;
    return true;
}
#endif

bool cli_native_lib_key(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--native-lib-key"}); return false; }
    if (v[0] == '-') {
        e = param_internal::trfs("mj2x.cli.err.negative_unsigned", {"--native-lib-key", v, "[0,0xFFFFFFFF]"});
        return false;
    }
    char*  end = nullptr;
    const unsigned long val = std::strtoul(v, &end, 16);
    if (end == v || *end != '\0') {
        e = param_internal::trfs("mj2x.cli.err.invalid_hex", {"--native-lib-key", v});
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
        if (!s.empty() && s[0] == '-') {
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned", {"native_lib_key", s, "[0,0xFFFFFFFF]"});
            return false;
        }
        char*             end = nullptr;
        const unsigned long val = std::strtoul(s.c_str(), &end, 16);
        if (end == s.c_str() || *end != '\0') {
            e = param_internal::trfs("mj2x.cli.err.cfg_invalid_hex", {"native_lib_key", s});
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
    e = param_internal::trfs("mj2x.cli.err.cfg_type_hex", {"native_lib_key"});
    return false;
}

// ---------- 基表（global/selection/compile + strings/../log 两段） ----------
// 后端参数段不在此声明：各后端接线单元持有，经 backend_registry 拼装。

// global + selection + compile（compile 段之后由注册表插入各后端段）
DEF_STR(nativeAnnotation, "--native-annotation", "native_annotation")
DEF_BOOL(dumpMIR, "--dump-mir", "dump_mir")
DEF_ENUM_STR(nativeOptLevel, "--native-opt-level", "native_opt_level", "0,1,2,3,s", "2")
DEF_BOOL(compileNative, "--compile", "compile")
DEF_STR(cmakeProjectDir, "--generate-cmake", "generate_cmake")
DEF_STR(xmakeProjectDir, "--generate-xmake", "generate_xmake")
DEF_INT(nativeTimeoutSec, "--native-timeout", "native_timeout", 0, 0)

const ParamDef kBaseParamsBefore[] = {
    // global
    PARAM("global.input", "-i", "input", ParamType::String, nullptr, 0, 0, "", "global", "", cli_input, json_input),
    PARAM("global.output", "-o", "output", ParamType::String, nullptr, 0, 0, "", "global", "", cli_output, json_output),
    // 枚举串拼装期自注册表回填（此处占位，从不被读取）。
    PARAM("global.backend", "--backend", "backend", ParamType::Enum,
          "",
          0, 0, "stackc", "global", "", cli_backend, json_backend),
    PARAM("global.emit_c", "--emit-c", "emit_c", ParamType::String, nullptr, 0, 0, "", "global", "", cli_emit_c, json_emit_c),
    PARAM("global.emit_cpp", "--emit-cpp", "emit_cpp", ParamType::String, nullptr, 0, 0, "", "global", "", cli_emit_cpp, json_emit_cpp),
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    PARAM("global.emit_llvm", "--emit-llvm", "emit_llvm", ParamType::String, nullptr, 0, 0, "", "global", "", cli_emit_llvm, json_emit_llvm),
#endif
    PARAM("global.emit_native_dir", "--emit-native-dir", "emit_native_dir", ParamType::String, nullptr, 0, 0, "", "global", "", cli_emit_native_dir, json_emit_native_dir),
    PARAM("global.dump_mir", "--dump-mir", "dump_mir", ParamType::Bool, nullptr, 0, 0, "false", "global", "", cli_dumpMIR, json_dumpMIR),
    // selection
    PARAM("selection.native_annotation", "--native-annotation", "native_annotation", ParamType::String, nullptr, 0, 0, "", "selection", "", cli_nativeAnnotation, json_nativeAnnotation),
    PARAM("selection.native_method_prefixes", "--native-method-prefix", "native_method_prefixes", ParamType::StringList, nullptr, 0, 0, "", "selection", "", cli_native_method_prefix, json_native_method_prefix),
    PARAM("selection.native_include", "--native-include", "native_include", ParamType::StringList, nullptr, 0, 0, "", "selection", "", cli_native_include, json_native_include),
    PARAM("obf_scope.include", "--obf-scope-include", "obf_scope_include", ParamType::StringList, nullptr, 0, 0, "", "obf_scope", "", cli_obf_scope_include, json_obf_scope_include),
    PARAM("obf_scope.exclude", "--obf-scope-exclude", "obf_scope_exclude", ParamType::StringList, nullptr, 0, 0, "", "obf_scope", "", cli_obf_scope_exclude, json_obf_scope_exclude),
    PARAM("selection.native_exclude", "--native-exclude", "native_exclude", ParamType::StringList, nullptr, 0, 0, "", "selection", "", cli_native_exclude, json_native_exclude),
    // compile
    PARAM("compile.enabled", "--compile", "compile", ParamType::Bool, nullptr, 0, 0, "false", "compile", "", cli_compileNative, json_compileNative),
    PARAM("compile.opt_level", "--native-opt-level", "native_opt_level", ParamType::Enum, "0,1,2,3,s", 0, 0, "2", "compile", "", cli_nativeOptLevel, json_nativeOptLevel),
    PARAM("compile.mode", "--compile-mode", "compile_mode", ParamType::Enum, "file,pipe", 0, 0, "file", "compile", "", cli_compile_mode, json_compile_mode),
    PARAM("compile.c_compiler", "--c-compiler", "c_compiler", ParamType::Enum, "auto,msvc,gcc,clang,zig,xmake", 0, 0, "auto", "compile", "", cli_c_compiler, json_c_compiler),
    PARAM("compile.timeout", "--native-timeout", "native_timeout", ParamType::Int, nullptr, 0, 0, "900", "compile", "", cli_nativeTimeoutSec, json_nativeTimeoutSec),
#ifndef MJ2X_DISABLE_LLVM_BACKEND
    PARAM("compile.llvm_tool", "--llvm-tool", "llvm_tool", ParamType::Enum, "clang,llc", 0, 0, "clang", "compile", "", cli_llvm_tool, json_llvm_tool),
#endif
    PARAM("compile.generate_cmake", "--generate-cmake", "generate_cmake", ParamType::String, nullptr, 0, 0, "", "compile", "", cli_cmakeProjectDir, json_cmakeProjectDir),
    PARAM("compile.generate_xmake", "--generate-xmake", "generate_xmake", ParamType::String, nullptr, 0, 0, "", "compile", "", cli_xmakeProjectDir, json_xmakeProjectDir),
};

// strings 及之后（compile 段与 strings 段之间为各后端参数段）
DEF_BOOL(stringsEncrypt, "--strings-encrypt", "strings_encrypt")
DEF_ENUM_STR(runtimeProtection, "--runtime-protection", "runtime_protection", "none,log,delay,fake,terminate", "none")
DEF_INT(runtimeProtectionStep, "--runtime-protection-step", "runtime_protection_step", 1, 0)
DEF_INT(runtimeProtectionDelayMs, "--runtime-protection-delay", "runtime_protection_delay_ms", 0, 0)
DEF_STR(nativeLibPackHook, "--native-lib-pack-hook", "native_lib_pack_hook")
DEF_STR(intrinsicConfigPath, "--intrinsic-config", "intrinsic_config")
DEF_STR_LIST(intrinsicSources, "--intrinsic-src", "intrinsic_src")
DEF_BOOL(quiet, "--quiet", "log_quiet")
DEF_BOOL(verbose, "--verbose", "log_verbose")
DEF_U64(clientId, "--client-id", "client_id")

const ParamDef kBaseParamsAfter[] = {
    // strings
    PARAM("strings.encrypt", "--strings-encrypt", "strings_encrypt", ParamType::Bool, nullptr, 0, 0, "false", "strings", "stackc|regc", cli_stringsEncrypt, json_stringsEncrypt),
    // runtime
    PARAM("runtime.protection", "--runtime-protection", "runtime_protection", ParamType::Enum, "none,log,delay,fake,terminate", 0, 0, "none", "runtime_protection", "regc|vmc4|llvm", cli_runtimeProtection, json_runtimeProtection),
    PARAM("runtime.protection_step", "--runtime-protection-step", "runtime_protection_step", ParamType::Int, nullptr, 1, 0, "16", "runtime_protection", "regc|vmc4|llvm", cli_runtimeProtectionStep, json_runtimeProtectionStep),
    PARAM("runtime.protection_delay_ms", "--runtime-protection-delay", "runtime_protection_delay_ms", ParamType::Int, nullptr, 0, 0, "500", "runtime_protection", "regc|vmc4|llvm", cli_runtimeProtectionDelayMs, json_runtimeProtectionDelayMs),
    // nativeLib
    PARAM("native_lib.key", "--native-lib-key", "native_lib_key", ParamType::Hex, nullptr, 0, 0, "", "native_lib", "", cli_native_lib_key, json_native_lib_key),
    PARAM("native_lib.pack_hook", "--native-lib-pack-hook", "native_lib_pack_hook", ParamType::String, nullptr, 0, 0, "", "native_lib", "", cli_nativeLibPackHook, json_nativeLibPackHook),
    // client
    PARAM("client.id", "--client-id", "client_id", ParamType::UInt64, nullptr, 0, 0, "0", "client", "", cli_clientId, json_clientId),
    // intrinsic
    PARAM("intrinsic.config", "--intrinsic-config", "intrinsic_config", ParamType::String, nullptr, 0, 0, "", "intrinsic", "", cli_intrinsicConfigPath, json_intrinsicConfigPath),
    PARAM("intrinsic.sources", "--intrinsic-src", "intrinsic_src", ParamType::StringList, nullptr, 0, 0, "", "intrinsic", "", cli_intrinsicSources, json_intrinsicSources),
    // log
    PARAM("log.quiet", "--quiet", "log_quiet", ParamType::Bool, nullptr, 0, 0, "false", "logging", "", cli_quiet, json_quiet),
    PARAM("log.verbose", "--verbose", "log_verbose", ParamType::Bool, nullptr, 0, 0, "false", "logging", "", cli_verbose, json_verbose),
};

/// --backend 枚举串：注册表单源（displayOrder 序）。逗号无空格——历史宏拼接
/// 口径，schema enum 与解析按 ',' 切分。
std::string backendEnumValues()
{
    std::string values;
    for (const BackendModule* module : backendModules()) {
        if (!values.empty()) values += ",";
        values += module->cliName;
    }
    return values;
}

std::vector<ParamDef> assembleParamTable(const std::string& backendEnum)
{
    const std::size_t beforeCount = sizeof(kBaseParamsBefore) / sizeof(kBaseParamsBefore[0]);
    const std::size_t afterCount  = sizeof(kBaseParamsAfter) / sizeof(kBaseParamsAfter[0]);
    std::vector<ParamDef> table;
    table.reserve(beforeCount + afterCount + 4);
    table.insert(table.end(), kBaseParamsBefore, kBaseParamsBefore + beforeCount);
    for (const BackendModule* module : backendModules()) {
        if (module->paramCount != 0) {
            table.insert(table.end(), module->params, module->params + module->paramCount);
        }
    }
    table.insert(table.end(), kBaseParamsAfter, kBaseParamsAfter + afterCount);
    // global.backend 行的枚举串拼装期回填（静态基表无法表达运行时裁剪面）。
    for (ParamDef& param : table) {
        if (std::strcmp(param.id, "global.backend") == 0) param.enumValues = backendEnum.c_str();
    }
    return table;
}

const std::vector<ParamDef>& paramTable()
{
    // 声明序保证枚举串先于表构造；首次访问在 main() 启动后，全部接线单元
    // 的静态注册已完成，拼装序与 TU 初始化顺序无关。
    static const std::string          kBackendEnum = backendEnumValues();
    static const std::vector<ParamDef> kTable      = assembleParamTable(kBackendEnum);
    return kTable;
}

}   // namespace

// ---------- 公开访问器（全量表，运行时拼装） ----------

const ParamDef* params()
{
    return paramTable().data();
}

std::size_t paramCount()
{
    return paramTable().size();
}

const ParamDef* findParamByCli(const std::string& cliFlag)
{
    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        if (cliFlag == table[i].cliFlag) return &table[i];
    }
    return nullptr;
}

const ParamDef* findParamByJsonPath(const std::string& jsonPath)
{
    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        if (jsonPath == table[i].jsonPath) return &table[i];
    }
    return nullptr;
}

std::string optCanonicalName(const char* cliFlag)
{
    std::string name = cliFlag;
    while (!name.empty() && name[0] == '-') name.erase(name.begin());
    return name;
}

bool parseBackendName(const std::string& name, CEmitterBackend& backend)
{
    return parseBackendNameImpl(name, backend);
}

// ---------- ca::opt Command 构建（CLI 解析单源，见头注） ----------

namespace {

/// 特殊参数（注册表之外）登记。
void addSpecialArg(ca::opt::Command& cmd, const char* name, ca::opt::OptKind kind)
{
    ca::opt::Arg arg;
    arg.name = name;
    arg.kind = kind;
    cmd.args.push_back(arg);
}

}   // namespace

ca::opt::Command buildOptCommand()
{
    ca::opt::Command cmd;
    cmd.name = "mj2x-cli";
    // config：预扫描已消费路径，此处登记仅为通过未知选项校验。
    addSpecialArg(cmd, "config", ca::opt::OptKind::String);
    // lang：i18n 预扫描已消费；String 形态下空格传值会消费后继 token（即使
    // 以 '-' 开头）、悬空报 MissingValue——较迁移前的容忍式扫描 fail-closed，
    // 行为变更由 DanglingLangFlagFailsClosed / LangFlagFollowedByFlagFailsClosed
    // 锁定；合法用法为 --lang=<lang> 或 --lang <lang> <args...>。
    addSpecialArg(cmd, "lang", ca::opt::OptKind::String);
    addSpecialArg(cmd, "dump-config-schema", ca::opt::OptKind::OptionalString);
    addSpecialArg(cmd, "dump-config-template", ca::opt::OptKind::OptionalString);
    ca::opt::MutexGroup dumpOnce;
    dumpOnce.names = {"dump-config-schema", "dump-config-template"};
    // 组标识回填 ParseError.group,main.cpp 按其分派错误文案。
    dumpOnce.label = "dump_once";
    cmd.mutex_groups.push_back(dumpOnce);

    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        const ParamDef& p = table[i];
        ca::opt::Arg arg;
        arg.name = optCanonicalName(p.cliFlag);
        switch (p.type) {
        case ParamType::Bool:
            arg.kind = ca::opt::OptKind::Flag;
            break;
        case ParamType::StringList:
            // ⚠️ 前提：StringList 的合法项本身不得含逗号——opt 侧按逗号
            // 拆分 + 跨次追加后逐项喂 parseCli（迁移前整串直达 parseCli，
            // 无拆分）。当前 67 参数均满足；新增含逗号项的列表参数时须
            // 改走 String kind 自行收集。
            arg.kind = ca::opt::OptKind::StringList;
            break;
        default:
            // 值语义校验（Int base0/范围、UInt64/Hex/Enum）统一在 parseCli。
            arg.kind = ca::opt::OptKind::String;
            break;
        }
        // 单 '-' 短形态(-i/-o)需显式别名;长形态由 --<canonical> 自动匹配。
        if (p.cliFlag[0] == '-' && p.cliFlag[1] != '-') arg.aliases.push_back(p.cliFlag);
        cmd.args.push_back(arg);
    }
    return cmd;
}

// ---------- JSON Schema 构建（--dump-config-schema） ----------

// ---------- i18n:参数文案单源表查询 ----------

namespace {
/// 参数描述 key:mj2x.param.<id>.desc。返回 std::string 持有拷贝,
/// 规避 tr 未命中时返回入参指针的生命周期陷阱。
std::string trParamDesc(const char* id)
{
    std::string key = "mj2x.param.";
    key += id;
    key += ".desc";
    return ca::i18n::tr(key.c_str());
}

/// 分组标题 key:mj2x.param.group.<slug>。
std::string trParamGroup(const char* slug)
{
    std::string key = "mj2x.param.group.";
    key += slug;
    return ca::i18n::tr(key.c_str());
}
}   // namespace

// ---------- JSON Schema 构建 ----------

nlohmann::json buildConfigSchemaJson()
{
    json props = json::object();
    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        const ParamDef& p = table[i];
        json            node;
        node["description"] = trParamDesc(p.id);
        switch (p.type) {
        case ParamType::Bool:
            node["type"]    = "boolean";
            node["default"] = (std::string(p.defaultStr) == "true");
            break;
        case ParamType::Int:
            node["type"] = "integer";
            // 负值统一 fail-closed（DEF_INT 恒拒），minValue=0（无追加下界）
            // 的 schema 下界即 0，与解析行为同源。
            node["minimum"] = p.minValue;
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
        {"description", ca::i18n::tr("mj2x.schema.description")},
        {"type", "object"},
        {"properties", std::move(props)},
        {"additionalProperties", false},
    };
}

// ---------- --help 分组输出 ----------

namespace {

/// 「名字|描述」结构化 flag help 行（\n 分行、| 分列，值内零对齐空格）→
/// HelpTable 渲染期对齐，与 mjt printUsage 的两列 help 同约定。列位来源不同：
/// mj2x 历史基线的 flag 描述列逐行手工摆放且并不统一（config/help 行在全行
/// 31 列、dump_schema/lang 行在 33 列，均含 printf 的 2 空格缩进），逐字节
/// 回归基线（#1075 验收项 4）要求锁死现状，故由调用方逐行给出描述列。
/// 无 '|' 的整行值按迁移前行为原样打印（自带名字列的遗留形态兜底）。
void printFlagHelp(const char* key, std::size_t descColumn)
{
    const std::string    value = ca::i18n::tr(key);
    std::string::size_type start = 0;
    while (start < value.size()) {
        const std::string::size_type nl = value.find('\n', start);
        const std::string row =
            nl == std::string::npos ? value.substr(start) : value.substr(start, nl - start);
        if (!row.empty()) {
            const std::string::size_type sep = row.find('|');
            if (sep == std::string::npos) {
                printf("  %s\n", row.c_str());
            } else {
                ca::opt::HelpTable table;
                table.add(row.substr(0, sep), row.substr(sep + 1));
                fputs(table.render_to_column(2, descColumn).c_str(), stdout);
            }
        }
        start = (nl == std::string::npos) ? value.size() : nl + 1;
    }
}

}   // namespace

void printHelp()
{
    printf("%s\n", ca::i18n::tr("mj2x.cli.help.usage"));
    printf("%s\n", ca::i18n::tr("mj2x.cli.help.options_header"));
    printFlagHelp("mj2x.cli.help.config_flag", 31);
    printFlagHelp("mj2x.cli.help.dump_schema_flag", 33);
    printf("  %s\n", ca::i18n::tr("mj2x.cli.help.dump_template_flag"));
    printFlagHelp("mj2x.cli.help.lang_flag", 33);

    // 参数表主体同样经 HelpTable 渲染：描述列 = 2 缩进 + 名字段宽 + 2 分隔
    // 空格，与原 printf("  %-*s  %s") 口径一致（名册均为 ASCII，码点计宽同
    // 字节宽）；分组标题仍整行打印，组间断开渲染即可，列位不变。
    std::size_t width = 2;
    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        width = std::max(width, std::strlen(table[i].cliFlag));
    }
    const std::size_t descColumn = width + 4;
    const char*       currentGroup = nullptr;
    ca::opt::HelpTable helpTable;
    auto flushTable = [&helpTable, descColumn]() {
        if (!helpTable.empty()) {
            fputs(helpTable.render_to_column(2, descColumn).c_str(), stdout);
            helpTable = ca::opt::HelpTable{};
        }
    };
    for (std::size_t i = 0; i < count; ++i) {
        const ParamDef& p = table[i];
        if (!currentGroup || std::strcmp(currentGroup, p.group) != 0) {
            flushTable();
            currentGroup = p.group;
            printf("\n  [%s]\n", trParamGroup(p.group).c_str());
        }
        helpTable.add(p.cliFlag, trParamDesc(p.id));
    }
    flushTable();
    printFlagHelp("mj2x.cli.help.help_flag", 31);
}

// ---------- 配置模板生成（--dump-config-template，JSONC） ----------

std::string buildConfigTemplateText()
{
    std::ostringstream out;
    out << "// " << ca::i18n::tr("mj2x.cli.template.header1") << "\n"
        << "// " << ca::i18n::tr("mj2x.cli.template.header2") << "\n"
        << "{\n";
    const std::size_t count = paramCount();
    const ParamDef*   table = params();
    for (std::size_t i = 0; i < count; ++i) {
        const ParamDef& p = table[i];
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
        if (i + 1 < count) out << ",";
        out << "  // " << p.id;
        if (p.requires && p.requires[0]) {
            out << "(" << ca::i18n::tr("mj2x.cli.template.requires") << ": " << p.requires << ")";
        }
        out << "\n";
    }
    out << "}\n";
    return out.str();
}

}   // namespace mj2x::cli

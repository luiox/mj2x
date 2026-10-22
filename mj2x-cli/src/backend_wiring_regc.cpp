// backend_wiring_regc.cpp
//
// RegC 后端接线单元：构造 + CLI 选项应用（含 R4 保护变换/safe-env/直调面）
// + pipeline 登记 + 参数表段，自注册进 backend_registry。#886 单后端构建：
// 本 TU 被 MJ2X_DISABLE_REGC_BACKEND 整体置空，模块（含参数段）不进注册表。

#include "backend_wiring.hpp"
#include "param_registry_internal.hpp"

#if !defined(MJ2X_DISABLE_REGC_BACKEND)

#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/core/seed_derivation.hpp"

#include <memory>

namespace mj2x::cli {
namespace {

bool wireRegC(BackendWiringContext& context)
{
    const CliOptions& options = context.options;
    auto              backend = std::make_unique<mj2x::RegCBackend>();
    if (context.hasUserIntrinsics()) backend->setUserIntrinsics(*context.userIntrinsics);
    if (!options.cOutputPath.empty()) backend->setOutputPath(options.cOutputPath);
    if (options.regcTraceCalls) backend->setTraceCallLog(true);
    // 直调档位：off/intra-class/all → DirectCallTier（与 LLVM 同一门控）。
    const mj2x::codegen::DirectCallTier directCallTier =
        options.regcDirectCalls == "all"           ? mj2x::codegen::DirectCallTier::All
        : options.regcDirectCalls == "intra-class" ? mj2x::codegen::DirectCallTier::IntraClass
                                                   : mj2x::codegen::DirectCallTier::Off;
    backend->setDirectCallTier(directCallTier);
    if (options.regcPodStruct) backend->setPodStruct(true);
    if (options.regcComputedJumps) backend->setComputedJumps(true);
    backend->setCommentLevel(options.regcCommentLevel);
    if (options.regcSafeEnv) backend->setSafeEnv(true);
    if (options.regcSafeEnvDebug) backend->setSafeEnvDebug(true);
    if (options.regcNoInline) backend->setInlineEnabled(false);
    // 基础混淆默认关闭；--regc-obfuscate 显式开启基础三项，#829 增强开关各自
    // 联动对应基础项。RegIR 级变换与 LLVM 后端共用（buildRegTransformOptions）。
    if (auto transform = buildRegTransformOptions(options)) {
        backend->setTransformOptions(*transform);
    }
    if (!options.nativeDirOutput.empty()) backend->setSplitOutputDir(options.nativeDirOutput);
    // --strings-encrypt：字面量 at-rest 加密（stackc/regc 专属开关，LLVM 后端
    // 不支持——PARAM 的 backends 面已限 stackc|regc）。
    if (options.stringsEncrypt) backend->setStringsEncrypt(true);
    if (options.runtimeProtection != "none") {
        backend->setRuntimeProtection(parseRuntimeProtectionAction(options.runtimeProtection),
                                      static_cast<uint32_t>(options.runtimeProtectionStep),
                                      static_cast<uint32_t>(options.runtimeProtectionDelayMs));
    }
    context.nativeSourceBackend = backend.get();
    // RegC 的 native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责。
    context.pipeline.registerBackend(std::move(backend));
    return true;
}

// ---------- 参数段（拼装于基表 compile 段与 strings 段之间，行序=历史表） ----------

DEF_BOOL(regcTraceCalls, "--regc-trace-calls", "regc_trace_calls")
DEF_ENUM_STR(regcDirectCalls, "--regc-direct-calls", "regc_direct_calls", "off,intra-class,all", "off")
DEF_BOOL(regcPodStruct, "--regc-pod-struct", "regc_pod_struct")
DEF_BOOL(regcComputedJumps, "--regc-computed-jumps", "regc_computed_jumps")
DEF_BOOL(regcSafeEnv, "--regc-safe-env", "regc_safe_env")
DEF_BOOL(regcNoInline, "--regc-no-inline", "regc_no_inline")
DEF_BOOL(regcObfuscate, "--regc-obfuscate", "regc_obfuscate")
DEF_INT(regcCffDead, "--regc-cff-dead", "regc_cff_dead", 0, 0)
DEF_BOOL(regcCffStateEncrypt, "--regc-cff-state-encrypt", "regc_cff_state_encrypt")
DEF_BOOL(regcMbaDeep, "--regc-mba-deep", "regc_mba_deep")
DEF_BOOL(regcOpaqueZero, "--regc-opaque-zero", "regc_opaque_zero")
DEF_BOOL(regcHideConstants, "--regc-hide-constants", "regc_hide_constants")
DEF_BOOL(regcIndirectCalls, "--regc-indirect-calls", "regc_indirect_calls")
DEF_U64(regcTransformSeed, "--regc-transform-seed", "regc_transform_seed")

bool cli_regc_comment_level(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--regc-comment-level"}); return false; }
    if (v == std::string("none")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::None;
    else if (v == std::string("summary")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Summary;
    else if (v == std::string("detailed")) o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Detailed;
    else {
        e = param_internal::trfs("mj2x.cli.err.unknown_flag_value", {"--regc-comment-level", "none, summary, or detailed"});
        return false;
    }
    return true;
}
bool json_regc_comment_level(CliOptions& o, const json& j, std::string& e)
{
    if (!j.is_string()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {"regc_comment_level"}); return false; }
    const std::string v = j.get<std::string>();
    if (v == "none") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::None;
    else if (v == "summary") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Summary;
    else if (v == "detailed") o.regcCommentLevel = mj2x::codegen::RegCCommentLevel::Detailed;
    else {
        e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum",
                {"regc_comment_level", v, "none, summary, detailed"});
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
    if (!j.is_boolean()) { e = param_internal::trfs("mj2x.cli.err.cfg_type_bool", {"regc_safe_env_debug"}); return false; }
    o.regcSafeEnvDebug = j.get<bool>();
    if (o.regcSafeEnvDebug) o.regcSafeEnv = true;
    return true;
}

const ParamDef kRegCParams[] = {
    PARAM("regc.trace_calls", "--regc-trace-calls", "regc_trace_calls", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regcTraceCalls, json_regcTraceCalls),
    PARAM("regc.direct_calls", "--regc-direct-calls", "regc_direct_calls", ParamType::Enum, "off,intra-class,all", 0, 0, "off", "regc_llvm", "regc|llvm", cli_regcDirectCalls, json_regcDirectCalls),
    PARAM("regc.pod_struct", "--regc-pod-struct", "regc_pod_struct", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regcPodStruct, json_regcPodStruct),
    PARAM("regc.computed_jumps", "--regc-computed-jumps", "regc_computed_jumps", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regcComputedJumps, json_regcComputedJumps),
    PARAM("regc.comment_level", "--regc-comment-level", "regc_comment_level", ParamType::Enum, "none,summary,detailed", 0, 0, "summary", "regc", "regc", cli_regc_comment_level, json_regc_comment_level),
    PARAM("regc.safe_env", "--regc-safe-env", "regc_safe_env", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regcSafeEnv, json_regcSafeEnv),
    PARAM("regc.safe_env_debug", "--regc-safe-env-debug", "regc_safe_env_debug", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regc_safe_env_debug, json_regc_safe_env_debug),
    PARAM("regc.no_inline", "--regc-no-inline", "regc_no_inline", ParamType::Bool, nullptr, 0, 0, "false", "regc", "regc", cli_regcNoInline, json_regcNoInline),
    PARAM("regc.obfuscate", "--regc-obfuscate", "regc_obfuscate", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcObfuscate, json_regcObfuscate),
    PARAM("regc.cff_dead", "--regc-cff-dead", "regc_cff_dead", ParamType::Int, nullptr, 0, 0, "0", "regc_llvm", "regc|llvm", cli_regcCffDead, json_regcCffDead),
    PARAM("regc.cff_state_encrypt", "--regc-cff-state-encrypt", "regc_cff_state_encrypt", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcCffStateEncrypt, json_regcCffStateEncrypt),
    PARAM("regc.mba_deep", "--regc-mba-deep", "regc_mba_deep", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcMbaDeep, json_regcMbaDeep),
    PARAM("regc.opaque_zero", "--regc-opaque-zero", "regc_opaque_zero", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcOpaqueZero, json_regcOpaqueZero),
    PARAM("regc.hide_constants", "--regc-hide-constants", "regc_hide_constants", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcHideConstants, json_regcHideConstants),
    PARAM("regc.indirect_calls", "--regc-indirect-calls", "regc_indirect_calls", ParamType::Bool, nullptr, 0, 0, "false", "regc_llvm", "regc|llvm", cli_regcIndirectCalls, json_regcIndirectCalls),
    PARAM("regc.transform_seed", "--regc-transform-seed", "regc_transform_seed", ParamType::UInt64, nullptr, 0, 0, "0", "regc_llvm", "regc|llvm", cli_regcTransformSeed, json_regcTransformSeed),
};

/// displayOrder 1 = 历史 CLI 清单次位。
const BackendModule kRegCModule = {
    CEmitterBackend::RegC,
    "regc",
    nullptr,
    0,
    1,
    &wireRegC,
    /*usesCloaderBridge*/ true,
    /*injectsNativeLoaderIntoClinit*/ true,
    /*afterPipelineRun*/ nullptr,
    kRegCParams,
    sizeof(kRegCParams) / sizeof(kRegCParams[0]),
};

const BackendModuleRegistration kRegCRegistration{kRegCModule};

}   // namespace
}   // namespace mj2x::cli

#endif   // !MJ2X_DISABLE_REGC_BACKEND

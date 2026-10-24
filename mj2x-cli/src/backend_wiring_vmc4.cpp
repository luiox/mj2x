// backend_wiring_vmc4.cpp
//
// VMC4 后端接线单元：构造 + CLI 选项应用（dispatcher 形态/粒度/漂移/加密面）
// + pipeline 登记 + 参数表段 + pipeline 后 trace surface 产物钩子（mjdbg 红队
// 契约），自注册进 backend_registry。#886：本 TU 被 MJ2X_DISABLE_VMC4_BACKEND
// 整体置空，模块（含参数段）不进注册表。

#include "backend_wiring.hpp"
#include "param_registry_internal.hpp"

#if !defined(MJ2X_DISABLE_VMC4_BACKEND)

#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/core/seed_derivation.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>

#include "libca/core/tag_cast.hpp"

namespace mj2x::cli {
namespace {

bool wireVmc4(BackendWiringContext& context)
{
    const CliOptions& options = context.options;
    auto              backend = std::make_unique<mj2x::Vmc4Backend>();
    if (context.hasUserIntrinsics()) backend->setUserIntrinsics(*context.userIntrinsics);
    if (!options.cOutputPath.empty()) backend->setOutputPath(options.cOutputPath);
    // dispatcher 形态字符串 -> 枚举（默认 switch_table）。
    mj2x::Vmc4Options vmc4Opts;
    vmc4Opts.seed = mj2x::deriveClientSeed(options.vmc4Seed, options.clientId);
    const std::string& disp = options.vmc4Dispatcher;
    if (disp == "funcptr_table" || disp == "funcptr")
        vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::FuncPtrTable;
    else if (disp == "computed_goto" || disp == "goto")
        vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::ComputedGoto;
    else if (disp == "if_nest" || disp == "ifnest")
        vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::IfNest;
    else if (disp == "state_dispatcher" || disp == "state")
        vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::StateDispatcher;
    else
        vmc4Opts.dispatcherKind = mj2x::VmDispatcherKind::SwitchTable;
    // 实例粒度：per-class（默认，同类方法共享 VM 实例）/ per-method /
    // per-program（全程序共享一个 VM 实例，opcode 空间全局统一）。
    if (options.vmc4Granularity == "method" || options.vmc4Granularity == "per-method")
        vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerMethod;
    else if (options.vmc4Granularity == "program" || options.vmc4Granularity == "per-program")
        vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerProgram;
    else
        vmc4Opts.granularity = mj2x::Vmc4Options::Granularity::PerClass;
    vmc4Opts.aliasCount             = options.vmc4Alias;
    vmc4Opts.bogusHandlers          = options.vmc4Bogus;
    vmc4Opts.driftVariants          = options.vmc4Drift;
    vmc4Opts.driftPeriod            = options.vmc4DriftPeriod;
    vmc4Opts.programBlobEncrypt     = options.vmc4EncryptProgram;
    vmc4Opts.programLazyDecrypt     = options.vmc4LazyDecrypt;
    vmc4Opts.programBlockLazyDecrypt = options.vmc4BlockLazyDecrypt;
    vmc4Opts.shuffleBlocks          = options.vmc4ShuffleBlocks;
    vmc4Opts.superOps               = options.vmc4SuperOps;
    vmc4Opts.fragment               = options.vmc4Fragment;
    vmc4Opts.regfile                = static_cast<uint32_t>(options.vmc4Regfile);
    if (options.runtimeProtection != "none") {
        vmc4Opts.runtimeProtection.action  = parseRuntimeProtectionAction(options.runtimeProtection);
        vmc4Opts.runtimeProtection.step    = static_cast<uint32_t>(options.runtimeProtectionStep);
        vmc4Opts.runtimeProtection.delayMs = static_cast<uint32_t>(options.runtimeProtectionDelayMs);
    }
    backend->setOptions(vmc4Opts);
    // VMC4 复用同样的 native-loader clinit 注入（NativeLoaderPostprocessor）。
    context.nativeSourceBackend = backend.get();
    context.pipeline.registerBackend(std::move(backend));
    return true;
}

/// VMC4：写 expected trace surface 文件（mjdbg 红队契约，同 seed+同输入 → 同内容）。
void afterPipelineRun(BackendWiringContext& context)
{
    if (context.options.vmc4TraceSurface.empty() || context.nativeSourceBackend == nullptr) return;
    auto*     vmc4 = ca::core::tag_cast::cast<mj2x::Vmc4Backend>(context.nativeSourceBackend);
    std::ofstream ts(context.options.vmc4TraceSurface, std::ios::binary | std::ios::trunc);
    if (ts) ts << vmc4->traceSurface();
}

// ---------- 参数段（拼装于基表 compile 段与 strings 段之间，行序=历史表） ----------

DEF_ENUM_STR(vmc4Dispatcher, "--vmc4-dispatcher", "vmc4_dispatcher", "switch_table,funcptr_table,computed_goto,if_nest,state_dispatcher", "switch_table")
DEF_ENUM_STR(vmc4Granularity, "--vmc4-granularity", "vmc4_granularity", "class,method,program", "class")
DEF_INT(vmc4Alias, "--vmc4-alias", "vmc4_alias", 0, 3)
DEF_INT(vmc4Bogus, "--vmc4-bogus", "vmc4_bogus", 0, 4)
DEF_INT(vmc4Drift, "--vmc4-drift", "vmc4_drift", 0, 4)
DEF_INT(vmc4DriftPeriod, "--vmc4-drift-period", "vmc4_drift_period", 1, 0)
DEF_STR(vmc4TraceSurface, "--vmc4-trace-surface", "vmc4_trace_surface")
DEF_BOOL(vmc4EncryptProgram, "--vmc4-encrypt-program", "vmc4_encrypt_program")
DEF_BOOL(vmc4LazyDecrypt, "--vmc4-lazy-decrypt", "vmc4_lazy_decrypt")
DEF_BOOL(vmc4BlockLazyDecrypt, "--vmc4-block-lazy-decrypt", "vmc4_block_lazy_decrypt")
DEF_BOOL(vmc4ShuffleBlocks, "--vmc4-shuffle-blocks", "vmc4_shuffle_blocks")
DEF_BOOL(vmc4SuperOps, "--vmc4-super-ops", "vmc4_super_ops")
DEF_BOOL(vmc4Fragment, "--vmc4-fragment", "vmc4_fragment")
DEF_INT(vmc4Regfile, "--vmc4-regfile", "vmc4_regfile", 0, 65535)

bool cli_vmc4_seed(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--vmc4-seed"}); return false; }
    if (v[0] == '-') {
        e = param_internal::trfs("mj2x.cli.err.negative_unsigned", {"--vmc4-seed", v, "[0,18446744073709551615]"});
        return false;
    }
    // 16 进制（0x...）或十进制均可，strtoull 自动识别（历史行为）。
    char*      end = nullptr;
    const unsigned long long val = std::strtoull(v, &end, 0);
    if (end == v || *end != '\0') {
        e = param_internal::trfs("mj2x.cli.err.invalid_value", {"--vmc4-seed", v});
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
        if (!s.empty() && s[0] == '-') {
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned",
                     {"vmc4_seed", s, "[0,18446744073709551615]"});
            return false;
        }
        char*             end = nullptr;
        const unsigned long long val = std::strtoull(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0') {
            e = param_internal::trfs("mj2x.cli.err.cfg_invalid_value", {"vmc4_seed", s});
            return false;
        }
        o.vmc4Seed         = static_cast<uint64_t>(val);
        o.vmc4SeedSelected = true;
        return true;
    }
    e = param_internal::trfs("mj2x.cli.err.cfg_type_num_hex", {"vmc4_seed"});
    return false;
}

const ParamDef kVmc4Params[] = {
    PARAM("vmc4.dispatcher", "--vmc4-dispatcher", "vmc4_dispatcher", ParamType::Enum, "switch_table,funcptr_table,computed_goto,if_nest,state_dispatcher", 0, 0, "switch_table", "vmc4", "vmc4", cli_vmc4Dispatcher, json_vmc4Dispatcher),
    PARAM("vmc4.seed", "--vmc4-seed", "vmc4_seed", ParamType::Hex, nullptr, 0, 0, "0", "vmc4", "vmc4", cli_vmc4_seed, json_vmc4_seed),
    PARAM("vmc4.granularity", "--vmc4-granularity", "vmc4_granularity", ParamType::Enum, "class,method,program", 0, 0, "class", "vmc4", "vmc4", cli_vmc4Granularity, json_vmc4Granularity),
    PARAM("vmc4.alias", "--vmc4-alias", "vmc4_alias", ParamType::Int, nullptr, 0, 3, "1", "vmc4", "vmc4", cli_vmc4Alias, json_vmc4Alias),
    PARAM("vmc4.bogus", "--vmc4-bogus", "vmc4_bogus", ParamType::Int, nullptr, 0, 4, "1", "vmc4", "vmc4", cli_vmc4Bogus, json_vmc4Bogus),
    PARAM("vmc4.drift", "--vmc4-drift", "vmc4_drift", ParamType::Int, nullptr, 0, 4, "0", "vmc4", "vmc4", cli_vmc4Drift, json_vmc4Drift),
    PARAM("vmc4.drift_period", "--vmc4-drift-period", "vmc4_drift_period", ParamType::Int, nullptr, 1, 0, "16", "vmc4", "vmc4", cli_vmc4DriftPeriod, json_vmc4DriftPeriod),
    PARAM("vmc4.trace_surface", "--vmc4-trace-surface", "vmc4_trace_surface", ParamType::String, nullptr, 0, 0, "", "vmc4", "vmc4", cli_vmc4TraceSurface, json_vmc4TraceSurface),
    PARAM("vmc4.encrypt_program", "--vmc4-encrypt-program", "vmc4_encrypt_program", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4EncryptProgram, json_vmc4EncryptProgram),
    PARAM("vmc4.lazy_decrypt", "--vmc4-lazy-decrypt", "vmc4_lazy_decrypt", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4LazyDecrypt, json_vmc4LazyDecrypt),
    PARAM("vmc4.block_lazy_decrypt", "--vmc4-block-lazy-decrypt", "vmc4_block_lazy_decrypt", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4BlockLazyDecrypt, json_vmc4BlockLazyDecrypt),
    PARAM("vmc4.shuffle_blocks", "--vmc4-shuffle-blocks", "vmc4_shuffle_blocks", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4ShuffleBlocks, json_vmc4ShuffleBlocks),
    PARAM("vmc4.super_ops", "--vmc4-super-ops", "vmc4_super_ops", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4SuperOps, json_vmc4SuperOps),
    PARAM("vmc4.fragment", "--vmc4-fragment", "vmc4_fragment", ParamType::Bool, nullptr, 0, 0, "false", "vmc4", "vmc4", cli_vmc4Fragment, json_vmc4Fragment),
    PARAM("vmc4.regfile", "--vmc4-regfile", "vmc4_regfile", ParamType::Int, nullptr, 0, 65535, "0", "vmc4", "vmc4", cli_vmc4Regfile, json_vmc4Regfile),
};

/// displayOrder 3 = 历史 CLI 清单末位。
const BackendModule kVmc4Module = {
    CEmitterBackend::Vmc4,
    "vmc4",
    nullptr,
    0,
    3,
    &wireVmc4,
    /*usesCloaderBridge*/ true,
    /*injectsNativeLoaderIntoClinit*/ false,
    &afterPipelineRun,
    kVmc4Params,
    sizeof(kVmc4Params) / sizeof(kVmc4Params[0]),
};

const BackendModuleRegistration kVmc4Registration{kVmc4Module};

}   // namespace
}   // namespace mj2x::cli

#endif   // !MJ2X_DISABLE_VMC4_BACKEND

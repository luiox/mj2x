// backend_wiring_vmx64.cpp
//
// vmx64 后端接线单元：构造 + CLI 选项应用（seed）+ pipeline 登记 + 参数表段，
// 自注册进 backend_registry。#886：本 TU 被 MJ2X_DISABLE_VMX64_BACKEND 整体
// 置空，模块（含参数段）不进注册表。Phase 1 面：--emit-c 出 .asm；
// 编译/工程生成在 main.cpp 校验环 fail-closed（见 mj2x.cli.err.vmx64_emit_only）。

#include "backend_wiring.hpp"
#include "param_registry_internal.hpp"

#if !defined(MJ2X_DISABLE_VMX64_BACKEND)

#include "mj2x/backend/vmx64/vmx64_backend.hpp"
#include "mj2x/core/seed_derivation.hpp"

#include <cstdlib>
#include <memory>

namespace mj2x::cli {
namespace {

bool wireVmx64(BackendWiringContext& context)
{
    const CliOptions& options = context.options;
    auto              backend = std::make_unique<mj2x::Vmx64Backend>();
    if (!options.cOutputPath.empty()) backend->setOutputPath(options.cOutputPath);
    mj2x::Vmx64Options vmx64Opts;
    vmx64Opts.seed = mj2x::deriveClientSeed(options.vmx64Seed, options.clientId);
    backend->setOptions(vmx64Opts);
    // Phase 1 无 RegisterNatives C TU（注册侧缺口）：不走 cloader 桥、不注入
    // native-loader clinit——产物消费面只有 --emit-c 的 .asm。
    context.nativeSourceBackend = backend.get();
    context.pipeline.registerBackend(std::move(backend));
    return true;
}

// ---------- 参数段（拼装于基表 compile 段与 strings 段之间，行序=历史表） ----------

bool cli_vmx64_seed(CliOptions& o, const char* v, std::string& e)
{
    if (!v) { e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {"--vmx64-seed"}); return false; }
    if (v[0] == '-') {
        e = param_internal::trfs("mj2x.cli.err.negative_unsigned", {"--vmx64-seed", v, "[0,18446744073709551615]"});
        return false;
    }
    // 16 进制（0x...）或十进制均可，strtoull 自动识别（与 --vmc4-seed 同口径）。
    char*                   end = nullptr;
    const unsigned long long val = std::strtoull(v, &end, 0);
    if (end == v || *end != '\0') {
        e = param_internal::trfs("mj2x.cli.err.invalid_value", {"--vmx64-seed", v});
        return false;
    }
    o.vmx64Seed         = static_cast<uint64_t>(val);
    o.vmx64SeedSelected = true;
    return true;
}
bool json_vmx64_seed(CliOptions& o, const json& j, std::string& e)
{
    if (j.is_number_unsigned()) {
        o.vmx64Seed         = j.get<uint64_t>();
        o.vmx64SeedSelected = true;
        return true;
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '-') {
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned",
                     {"vmx64_seed", s, "[0,18446744073709551615]"});
            return false;
        }
        char*                   end = nullptr;
        const unsigned long long val = std::strtoull(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0') {
            e = param_internal::trfs("mj2x.cli.err.cfg_invalid_value", {"vmx64_seed", s});
            return false;
        }
        o.vmx64Seed         = static_cast<uint64_t>(val);
        o.vmx64SeedSelected = true;
        return true;
    }
    e = param_internal::trfs("mj2x.cli.err.cfg_type_num_hex", {"vmx64_seed"});
    return false;
}

const ParamDef kVmx64Params[] = {
    PARAM("vmx64.seed", "--vmx64-seed", "vmx64_seed", ParamType::Hex, nullptr, 0, 0, "0", "vmx64", "vmx64", cli_vmx64_seed, json_vmx64_seed),
};

/// displayOrder 4 = 历史 CLI 清单末位之后（stackc, regc, llvm, vmc4 → vmx64）。
const BackendModule kVmx64Module = {
    CEmitterBackend::Vmx64,
    "vmx64",
    nullptr,
    0,
    4,
    &wireVmx64,
    /*usesCloaderBridge*/ false,
    /*injectsNativeLoaderIntoClinit*/ false,
    /*afterPipelineRun*/ nullptr,
    kVmx64Params,
    sizeof(kVmx64Params) / sizeof(kVmx64Params[0]),
};

const BackendModuleRegistration kVmx64Registration{kVmx64Module};

}   // namespace
}   // namespace mj2x::cli

#endif   // !MJ2X_DISABLE_VMX64_BACKEND

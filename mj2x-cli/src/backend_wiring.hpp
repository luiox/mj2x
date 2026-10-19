#pragma once

// backend_wiring.hpp
//
// 后端接线单元共享面：各 backend_wiring_<backend>.cpp（每后端一个 TU，自注册
// 进 backend_registry）共用的工具函数声明。主流程 main.cpp 经本头拿到接线
// 上下文，不直接接触具体后端头。

#include "backend_registry.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"

#include <optional>
#include <string>

namespace mj2x::cli {

/// @brief --runtime-protection 字符串 → codegen::RuntimeProtectionAction
///        （RegC/VMC4/LLVM 接线共用；未知值返回 None，合法性由 CLI 校验环兜底）。
inline mj2x::codegen::RuntimeProtectionAction parseRuntimeProtectionAction(
    const std::string& name)
{
    if (name == "log") return mj2x::codegen::RuntimeProtectionAction::Log;
    if (name == "delay") return mj2x::codegen::RuntimeProtectionAction::Delay;
    if (name == "fake") return mj2x::codegen::RuntimeProtectionAction::Fake;
    if (name == "terminate") return mj2x::codegen::RuntimeProtectionAction::Terminate;
    return mj2x::codegen::RuntimeProtectionAction::None;
}

/// @brief R4 保护变换选项（RegIR 级，RegC/LLVM 共用）：CLI 开关 → 变换选项。
///
/// 基础混淆默认关闭；--regc-obfuscate 显式开启基础三项，#829 增强开关各自
/// 联动对应基础项（mbaDeep→MBA、opaqueZero→opaque、cffDead/stateEncrypt→CFF）。
/// 双后端都被裁的构建（stackc-only/vmc4-only）无定义、也无人调用。
std::optional<mj2x::RegTransformOptions> buildRegTransformOptions(const CliOptions& options);

}   // namespace mj2x::cli

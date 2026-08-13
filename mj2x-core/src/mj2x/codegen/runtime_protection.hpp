/// @file runtime_protection.hpp
/// @brief 反侦测检查点注入的生成开关与 C 片段（RegC/VMC4 共用）。
///
/// @details
/// 对应 mj2x-crt 的 `rt_protect/mj2x_rt_protect.h`（C99 自包含检查点）。
/// 生成器在方法入口注入 `mj2x_rt_checkpoint_run` 调用；默认关闭（action=None），
/// 开启后产物依赖 mj2x-crt 的 rt_protect 模块（已在 resources.manifest 内嵌）。
#pragma once

#include <cstdint>
#include <string>

namespace mj2x {
namespace codegen {

class CSourceStream;

/// @brief 检查点响应动作（与 crt 的 mj2x_rt_protect_action 一一对应）。
enum class RuntimeProtectionAction : uint8_t
{
    None = 0,     ///< 不注入（默认）。
    Log = 1,      ///< 命中只计数。
    Delay = 2,    ///< 命中后睡眠 delayMs 毫秒。
    Fake = 3,     ///< 命中时方法返回默认值（假数据）。
    Terminate = 4,///< 命中即终止进程。
};

/// @brief 检查点注入选项（RegC 经 CodeOptions、VMC4 经 Vmc4Options 持有）。
struct RuntimeProtectionOptions
{
    RuntimeProtectionAction action  = RuntimeProtectionAction::None;
    std::uint32_t           step    = 16;    ///< 概率触发步长（每 step 次调用至少一次）。
    std::uint32_t           delayMs = 500;   ///< Delay 动作睡眠毫秒数。

    bool enabled() const { return action != RuntimeProtectionAction::None; }
};

/// @brief 生成方法入口检查点注入片段（static 状态变量 + 调用 + FAKE 返回）。
///
/// @param out           目标 C 流（已在函数体内、已缩进）。
/// @param opts          注入选项（enabled() 为 false 时不输出）。
/// @param checkpointVar 状态变量名（调用方保证 TU 内唯一，如
///                      `mj2x_rt_cp_<methodTag>`）。
/// @param fakeReturn    FAKE 命中时的返回语句文本（含分号与换行；空 = 仅
///                      return 语义由调用方处理——void 方法直接 return）。
void emitRuntimeProtectionCheckpoint(CSourceStream& out, const RuntimeProtectionOptions& opts,
                                     const std::string& checkpointVar,
                                     const std::string& fakeReturn);

/// @brief 动作的 C 枚举 token（如 "MJ2X_RT_PROTECT_LOG"）；None 返回空串。
const char* runtimeProtectionActionToken(RuntimeProtectionAction action);

}   // namespace codegen
}   // namespace mj2x

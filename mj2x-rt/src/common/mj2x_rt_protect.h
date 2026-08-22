/// @file mj2x_rt_protect.h
/// @brief 自包含轻量反侦测检查点（C99，供生成代码方法入口注入）。
///
/// 与 mj2x-runtime 的 C++ detection/enforcement 模块职责对应但独立实现，
/// 保持 C 可分发（随 mj2x-rt 进入生成工程，无外部依赖）。默认不注入；
/// 由生成器 --runtime-protection 开关注入调用（RegC/VMC4 通用）。
///
/// ## 检测面（R3 整合后）
///
/// - Windows：IsDebuggerPresent / CheckRemoteDebuggerPresent + PEB
///   BeingDebugged/NtGlobalFlag 调试堆位 + NtQueryInformationProcess
///   (DebugPort/DebugObjectHandle/DebugFlags) + 硬件断点调试寄存器
///   (Dr0-3/Dr7) + 命令行扫描（-javaagent: / -agentpath: / -agentlib: / jdwp）
/// - Linux：/proc/self/status TracerPid + /proc/self/maps 可写可执行段与
///   agent 库映射 + /proc/self/cmdline 扫描
///
/// 探测逻辑移植自 mj2x-runtime C++ antidebug 层（R3 整合，见
/// doc_ai/r3_protect_integration.md）；全部 fail-open。
///
/// ## 触发模型
///
/// 概率触发（xorshift 伪随机 + step 确定性下限），检测命中可 force 必然触发。
/// 与 mj2x-runtime enforcement 的 CheckpointState 语义一致（C 侧独立实现）。
#ifndef MJ2X_RT_PROTECT_H
#define MJ2X_RT_PROTECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 检查点命中后的响应动作。
typedef enum mj2x_rt_protect_action
{
    MJ2X_RT_PROTECT_LOG       = 1,   ///< 只计数，不动作。
    MJ2X_RT_PROTECT_DELAY     = 2,   ///< 命中后睡眠 delay_ms 毫秒（拖慢人工分析）。
    MJ2X_RT_PROTECT_FAKE      = 3,   ///< 命中返回 1，调用方应返回假数据/降级结果。
    MJ2X_RT_PROTECT_TERMINATE = 4,   ///< 命中即终止进程（不返回）。
} mj2x_rt_protect_action;

/// @brief 检查点状态（每检查点一个，调用方持有；零初始化即可用）。
typedef struct mj2x_rt_checkpoint
{
    uint32_t seed;         ///< xorshift 状态；0 = 首次运行时按地址/时间初始化。
    uint32_t invocations;  ///< 已推进次数。
    uint32_t triggers;     ///< 已命中次数。
} mj2x_rt_checkpoint;

/// @brief 执行一次检查点：探测当前进程调试/agent 信号，命中或概率触发时按
/// action 响应。
///
/// @param cp       检查点状态（首次调用自动初始化 seed）。
/// @param action   响应动作。
/// @param step     概率触发步长（每 step 次调用至少触发一次；0 = 默认 16）。
/// @param delay_ms DELAY 动作的睡眠毫秒数。
/// @param force    1 = 探测命中时必然触发（跳过概率判定）。
///
/// @return 1 = 调用方应返回假数据/降级（FAKE 命中时）；0 = 正常。TERMINATE
/// 命中时不返回。
int mj2x_rt_checkpoint_run(mj2x_rt_checkpoint* cp, mj2x_rt_protect_action action,
                           uint32_t step, uint32_t delay_ms, int force);

/// @brief NtGlobalFlag 调试堆标志判定（移植自 antidebug::hasSuspiciousNtGlobalFlag）。
/// 导出仅供单测。
int mj2x_rt_nt_global_flag_suspicious(uint32_t nt_global_flag);

/// @brief 硬件断点使能判定：Dr0-3 任一地址非零且 Dr7 低 8 位有使能位
/// （移植自 antidebug::hasEnabledHardwareBreakpoint）。导出仅供单测。
int mj2x_rt_hardware_breakpoint_enabled(uintptr_t dr0, uintptr_t dr1, uintptr_t dr2,
                                        uintptr_t dr3, uintptr_t dr7);

#ifdef __cplusplus
}
#endif

#endif   // MJ2X_RT_PROTECT_H

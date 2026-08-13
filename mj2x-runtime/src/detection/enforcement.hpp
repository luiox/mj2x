/// @file enforcement.hpp
/// @brief Policy-driven response layer above detection aggregation.
///
/// 分层：探测（antidebug/antijvmti）→ 聚合决策（runtime_detection）→ 响应
/// （本模块）。roadmap §3.4「从基础探测走向真实反调试/反 agent/反 dump」的
/// 响应面：检测命中后按可配置策略执行动作（只记录 / 延迟 / 假数据 / 终止）。
///
/// 多阶段检查点：检查点状态独立于检测信号——即使当前未检测到信号，检查点
/// 也会按概率推进（防「检测窗口外 attach」与单点绕过）；检测到信号时调用方可
/// force 触发。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <antidebug/antidebug.hpp>
#include <antijvmti/antijvmti.hpp>
#include <detection/runtime_detection.hpp>

namespace mj2x::runtime::detection {

/// @brief 检测命中的响应动作。
enum class EnforcementAction
{
    LogOnly,    ///< 只记录（默认；等于探测层现有行为）。
    Delay,      ///< 每次命中睡眠 delayMs 毫秒（拖慢人工分析）。
    FakeData,   ///< 命中时调用方应返回假数据/降级结果（不暴露真实语义）。
    Terminate,  ///< 命中时调用方应终止进程（由集成层执行 abort/_exit）。
};

/// @brief 检查点策略（每检查点独立配置）。
struct EnforcementPolicy
{
    EnforcementAction action           = EnforcementAction::LogOnly;
    std::uint32_t     delayMs          = 500;    ///< Delay 动作的睡眠时长（毫秒）。
    std::uint32_t     checkpointStep   = 16;     ///< 触发步长（见 checkpointShouldTrigger）。
};

/// @brief 检查点状态（调用方按检查点位置持有，跨调用累积）。
struct CheckpointState
{
    std::uint32_t seed        = 0x9E3779B9u;   ///< xorshift 状态（确定性序列）。
    std::uint32_t invocations = 0;             ///< 已推进次数。
    std::uint32_t triggers    = 0;             ///< 已命中次数。
};

/// @brief 创建检查点状态；seed 可注入（测试用固定值，生产用系统熵）。
CheckpointState makeCheckpointState(std::uint32_t seed);

/// @brief 检查点触发判定（纯逻辑，可测试）。
///
/// @details 多阶段 + 随机化：确定性下限（每 checkpointStep 次调用至少触发一次）
/// 与伪随机提前命中（xorshift % step == 0）结合——攻击者无法靠「精确等到第 N
/// 次调用」定位或规避检查点。每次调用推进 invocations。
bool checkpointShouldTrigger(CheckpointState& state, const EnforcementPolicy& policy);

/// @brief 检查点命中后的响应结果。
enum class EnforcementOutcome
{
    NoTrigger,          ///< 本次检查点未命中。
    TriggeredLogOnly,   ///< 命中且已按 LogOnly 处理。
    TriggeredDelay,     ///< 命中且已睡眠 delayMs。
    TriggeredFakeData,  ///< 命中：调用方应返回假数据。
    TriggeredTerminate, ///< 命中：调用方应终止进程。
};

/// @brief 检查点执行：判定 + 按 action 响应。
///
/// @param state  检查点状态（推进计数）。
/// @param policy 策略。
/// @param force  检测到信号时传 true：跳过概率判定必然触发（仍计数）。
///
/// @details Terminate 不在此处执行（返回 outcome，由调用方/集成层 abort），
/// 保证单测可覆盖全部 action。Delay 直接平台睡眠（测试传 delayMs=0 跳过）。
EnforcementOutcome enforceCheckpoint(CheckpointState& state, const EnforcementPolicy& policy,
                                     bool force = false);

/// @brief 便捷组合：收集检测报告、按策略响应。
///
/// @details 检测到信号时 force 触发；无信号时仍推进多阶段检查点（防检测窗口
/// 外 attach）。DetectionPolicy 使用默认（denyOnSignal/denyOnProbeFailure 均
/// 开启），需要自定义时请自行组合 collectRuntimeDetection + shouldFailClosed
/// + enforceCheckpoint。
EnforcementOutcome checkAndEnforce(CheckpointState& state, const EnforcementPolicy& policy,
                                   const std::vector<std::string>& arguments, void* javaVm,
                                   RuntimeDetectionReport* reportOut = nullptr);

}   // namespace mj2x::runtime::detection

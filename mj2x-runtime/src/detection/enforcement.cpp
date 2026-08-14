#include "enforcement.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

namespace mj2x::runtime::detection {

namespace {

/// @brief xorshift32 伪随机（自包含、确定性；seed 由调用方注入）。
std::uint32_t xorshift32(std::uint32_t& state)
{
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

void platformSleepMs(std::uint32_t ms)
{
    if (ms == 0) return;
#ifdef _WIN32
    Sleep(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

/// @brief 单调时钟（毫秒）：周期复查的时间基准。
/// Windows 用 QueryPerformanceCounter，POSIX 用 steady_clock（均单调，不受
/// 系统时间调整影响）。
std::uint64_t monotonicMs()
{
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    if (freq.QuadPart > 0) {
        return static_cast<std::uint64_t>((counter.QuadPart * 1000) / freq.QuadPart);
    }
    return 0;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

}   // namespace

CheckpointState makeCheckpointState(std::uint32_t seed)
{
    CheckpointState state;
    state.seed = seed != 0 ? seed : 0x9E3779B9u;
    return state;
}

bool checkpointShouldTrigger(CheckpointState& state, const EnforcementPolicy& policy)
{
    ++state.invocations;
    const std::uint32_t step          = policy.checkpointStep != 0 ? policy.checkpointStep : 1;
    const bool          deterministic = (state.invocations % step) == 0;
    const bool          randomHit     = (xorshift32(state.seed) % step) == 0;
    if (deterministic || randomHit) {
        ++state.triggers;
        return true;
    }
    return false;
}

EnforcementOutcome enforceCheckpoint(CheckpointState& state, const EnforcementPolicy& policy,
                                     bool force)
{
    if (force) {
        // 信号命中：必然触发（仍推进计数，保持调用/命中统计一致）。
        ++state.invocations;
        ++state.triggers;
    }
    else if (!checkpointShouldTrigger(state, policy)) {
        return EnforcementOutcome::NoTrigger;
    }
    switch (policy.action) {
    case EnforcementAction::LogOnly: return EnforcementOutcome::TriggeredLogOnly;
    case EnforcementAction::Delay:
        platformSleepMs(policy.delayMs);
        return EnforcementOutcome::TriggeredDelay;
    case EnforcementAction::FakeData: return EnforcementOutcome::TriggeredFakeData;
    case EnforcementAction::Terminate: return EnforcementOutcome::TriggeredTerminate;
    }
    return EnforcementOutcome::TriggeredLogOnly;
}

EnforcementOutcome checkAndEnforce(CheckpointState& state, const EnforcementPolicy& policy,
                                   const std::vector<std::string>& arguments, void* javaVm,
                                   RuntimeDetectionReport* reportOut)
{
    // 周期复查（竞品借鉴低成本项）：probeIntervalMs == 0 时每次检查点全量探测
    // （现状兼容）；> 0 时按单调时钟间隔复用缓存报告——高频检查点路径免去全量
    // 探测（模块枚举/JVMTI 查询）开销，攻击者中途 attach 后最多一个间隔内被检出。
    const std::uint64_t nowMs = monotonicMs();
    if (policy.probeIntervalMs == 0 || state.lastProbeMs == 0 ||
        nowMs - state.lastProbeMs >= policy.probeIntervalMs) {
        state.cachedReport = collectRuntimeDetection(arguments, javaVm);
        state.lastProbeMs  = nowMs;
    }
    RuntimeDetectionReport& report = state.cachedReport;
    if (reportOut != nullptr) *reportOut = report;
    const bool signaled = shouldFailClosed(report);
    return enforceCheckpoint(state, policy, signaled);
}

}   // namespace mj2x::runtime::detection

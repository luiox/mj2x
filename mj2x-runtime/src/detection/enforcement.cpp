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
    RuntimeDetectionReport report = collectRuntimeDetection(arguments, javaVm);
    if (reportOut != nullptr) *reportOut = report;
    const bool signaled = shouldFailClosed(report);
    return enforceCheckpoint(state, policy, signaled);
}

}   // namespace mj2x::runtime::detection

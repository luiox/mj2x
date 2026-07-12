/**
 * @file runtime_detection.hpp
 * @brief Policy-neutral aggregation for live anti-debug and anti-agent probes.
 */
#pragma once

#include <string>
#include <vector>

#include <antidebug/antidebug.hpp>
#include <antijvmti/antijvmti.hpp>

namespace mj2x::runtime::detection {

struct RuntimeDetectionReport
{
    antidebug::DebugSignals   debug;
    antijvmti::LiveAgentProbe agent;
    bool                      suspicious  = false;
    bool                      probeFailed = false;
};

struct DetectionPolicy
{
    bool denyOnSignal       = true;
    bool denyOnProbeFailure = true;
};

/**
 * @brief Collect current-process debugger, module, and JVMTI facts.
 */
RuntimeDetectionReport collectRuntimeDetection(const std::vector<std::string>& arguments,
                                               void*                           javaVm);

/**
 * @brief Apply fail-closed policy without terminating or throwing.
 */
bool shouldFailClosed(const RuntimeDetectionReport& report, const DetectionPolicy& policy = {});

}   // namespace mj2x::runtime::detection

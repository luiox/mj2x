#include "runtime_detection.hpp"

namespace mj2x::runtime::detection {

RuntimeDetectionReport collectRuntimeDetection(const std::vector<std::string>& arguments,
                                               void*                           javaVm)
{
    RuntimeDetectionReport report;
    report.debug = antidebug::collectDebugSignals();
    report.agent = antijvmti::collectLiveAgentSignals(arguments, javaVm);
    report.suspicious =
        antidebug::hasDebugSignal(report.debug) || antijvmti::hasAgentSignal(report.agent.signals);
    report.probeFailed = report.debug.nativeProbeFailed ||
                         report.agent.modules.status == antijvmti::LiveProbeStatus::Failed ||
                         report.agent.jvmti.status == antijvmti::LiveProbeStatus::Failed;
    return report;
}

bool shouldFailClosed(const RuntimeDetectionReport& report, const DetectionPolicy& policy)
{
    return (policy.denyOnSignal && report.suspicious) ||
           (policy.denyOnProbeFailure && report.probeFailed);
}

}   // namespace mj2x::runtime::detection

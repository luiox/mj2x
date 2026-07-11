#include <gmock/gmock.h>

#include "antidebug/antidebug.hpp"
#include "antijvmti/antijvmti.hpp"
#include "detection/runtime_detection.hpp"

TEST(RuntimeAntiJvmtiTest, DetectsJniTableReplacement) {
    int originalTable = 1;
    int replacementTable = 2;

    auto snapshot = mj2x::runtime::antijvmti::captureJniTable(&originalTable);
    EXPECT_TRUE(snapshot.valid);
    EXPECT_TRUE(mj2x::runtime::antijvmti::isJniTableStable(&originalTable, snapshot));
    EXPECT_FALSE(mj2x::runtime::antijvmti::isJniTableStable(&replacementTable, snapshot));
    EXPECT_FALSE(mj2x::runtime::antijvmti::isJniTableStable(nullptr, snapshot));
}

TEST(RuntimeAntiJvmtiTest, DetectsAgentOptions) {
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsAgentOption({"-Xmx256m", "-javaagent:c2j.jar"}));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsAgentOption({"-agentpath:C:/tmp/hook.dll"}));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsAgentOption({"-agentlib:jdwp=transport=dt_socket"}));
    EXPECT_FALSE(mj2x::runtime::antijvmti::containsAgentOption({"-Xmx256m", "-Dname=value"}));
}

TEST(RuntimeAntiJvmtiTest, DetectsJdwpOptionsSeparately) {
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsJdwpOption({"-agentlib:jdwp=transport=dt_socket"}));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsJdwpOption({"-Xrunjdwp:transport=dt_socket"}));
    EXPECT_FALSE(mj2x::runtime::antijvmti::containsJdwpOption({"-Xmx256m", "-Dname=value"}));
}

TEST(RuntimeAntiJvmtiTest, DetectsSuspiciousModulesWithoutLaunchArgs) {
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsSuspiciousAgentModule({
        "C:/app/bin/arthas-spy.dll",
    }));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsSuspiciousAgentModule({
        "C:/tmp/ByteBuddyAgent.dll",
    }));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsSuspiciousJvmtiDumpModule({
        "C:/tools/mjt-dump-agent.dll",
    }));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsSuspiciousJvmtiDumpModule({
        "libjvmti_classdump.so",
    }));
    EXPECT_FALSE(mj2x::runtime::antijvmti::containsSuspiciousAgentModule({
        "C:/Windows/System32/kernel32.dll",
    }));
}

TEST(RuntimeAntiJvmtiTest, DetectsDumpRelevantCapabilities) {
    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot none;
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(none));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot bytecodes;
    bytecodes.canGetBytecodes = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(bytecodes));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot retransformTag;
    retransformTag.canTagObjects = true;
    retransformTag.canRetransformClasses = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(retransformTag));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot classHooks;
    classHooks.canGenerateAllClassHookEvents = true;
    classHooks.canRetransformClasses = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(classHooks));
}

TEST(RuntimeAntiJvmtiTest, AggregatesAgentSignals) {
    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot capabilities;
    capabilities.canGenerateAllClassHookEvents = true;
    capabilities.canRetransformClasses = true;

    auto signals = mj2x::runtime::antijvmti::collectAgentSignals(
        {"-Xmx256m"},
        {"C:/tmp/arthas-agent.dll"},
        capabilities);
    EXPECT_TRUE(signals.suspiciousJavaAgentModule);
    EXPECT_TRUE(signals.dumpRelevantJvmtiCapabilities);
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasAgentSignal(signals));

    mj2x::runtime::antijvmti::AgentSignals none;
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasAgentSignal(none));
}

TEST(RuntimeAntiJvmtiTest, EnumeratesLiveModulesWithoutAssumingAgentNames) {
    const auto modules = mj2x::runtime::antijvmti::enumerateLoadedModules();
    if (modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Success) {
        EXPECT_FALSE(modules.moduleNames.empty());
    } else {
        EXPECT_TRUE(modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable ||
                    modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Failed);
    }
}

TEST(RuntimeAntiJvmtiTest, LiveJvmtiQueryRequiresAnExplicitJavaVm) {
    const auto query = mj2x::runtime::antijvmti::queryLiveJvmtiCapabilities(nullptr);
    EXPECT_EQ(query.status, mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable);
}

TEST(RuntimeAntiJvmtiTest, CollectsLiveAgentProbeWithModuleFacts) {
    const auto probe = mj2x::runtime::antijvmti::collectLiveAgentSignals({}, nullptr);
    EXPECT_EQ(probe.jvmti.status, mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable);
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasAgentSignal(probe.signals));
}

TEST(RuntimeAntiDebugTest, AggregatesDebugSignals) {
    mj2x::runtime::antidebug::DebugSignals none;
    EXPECT_FALSE(mj2x::runtime::antidebug::hasDebugSignal(none));

    mj2x::runtime::antidebug::DebugSignals local;
    local.debuggerPresent = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(local));

    auto current = mj2x::runtime::antidebug::collectDebugSignals();
    (void)current;
}

TEST(RuntimeAntiDebugTest, DetectsSoftwareBreakpointPatterns) {
    const unsigned char clean[] = {0x55, 0x48, 0x89, 0xE5};
    EXPECT_FALSE(mj2x::runtime::antidebug::containsSoftwareBreakpoint(clean, sizeof(clean)));

    const unsigned char int3Short[] = {0x55, 0xCC, 0x90};
    EXPECT_TRUE(mj2x::runtime::antidebug::containsSoftwareBreakpoint(int3Short, sizeof(int3Short)));

    const unsigned char int3Long[] = {0x55, 0xCD, 0x03, 0x90};
    EXPECT_TRUE(mj2x::runtime::antidebug::containsSoftwareBreakpoint(int3Long, sizeof(int3Long)));
    EXPECT_FALSE(mj2x::runtime::antidebug::containsSoftwareBreakpoint(nullptr, 16));

    mj2x::runtime::antidebug::DebugSignals signals;
    signals.softwareBreakpointPattern = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(signals));
}

TEST(RuntimeAntiDebugTest, DetectsWindowsSnapshotPrimitives) {
    EXPECT_TRUE(mj2x::runtime::antidebug::hasSuspiciousNtGlobalFlag(0x70));
    EXPECT_FALSE(mj2x::runtime::antidebug::hasSuspiciousNtGlobalFlag(0));
    EXPECT_FALSE(mj2x::runtime::antidebug::hasEnabledHardwareBreakpoint(0, 0, 0, 0, 0));
    EXPECT_TRUE(mj2x::runtime::antidebug::hasEnabledHardwareBreakpoint(
        0x1234, 0, 0, 0, 0x1));
}

TEST(RuntimeDetectionTest, AppliesFailClosedPolicyWithoutTerminating) {
    mj2x::runtime::detection::RuntimeDetectionReport clean;
    EXPECT_FALSE(mj2x::runtime::detection::shouldFailClosed(clean));

    clean.suspicious = true;
    EXPECT_TRUE(mj2x::runtime::detection::shouldFailClosed(clean));

    clean.suspicious = false;
    clean.probeFailed = true;
    EXPECT_TRUE(mj2x::runtime::detection::shouldFailClosed(clean));
    EXPECT_FALSE(mj2x::runtime::detection::shouldFailClosed(
        clean, mj2x::runtime::detection::DetectionPolicy{true, false}));
}

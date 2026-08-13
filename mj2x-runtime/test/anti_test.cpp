#include <gmock/gmock.h>

#include "antidebug/antidebug.hpp"
#include "antijvmti/antijvmti.hpp"
#include "detection/enforcement.hpp"
#include "detection/runtime_detection.hpp"

TEST(RuntimeAntiJvmtiTest, DetectsJniTableReplacement)
{
    int originalTable    = 1;
    int replacementTable = 2;

    auto snapshot = mj2x::runtime::antijvmti::captureJniTable(&originalTable);
    EXPECT_TRUE(snapshot.valid);
    EXPECT_TRUE(mj2x::runtime::antijvmti::isJniTableStable(&originalTable, snapshot));
    EXPECT_FALSE(mj2x::runtime::antijvmti::isJniTableStable(&replacementTable, snapshot));
    EXPECT_FALSE(mj2x::runtime::antijvmti::isJniTableStable(nullptr, snapshot));
}

TEST(RuntimeAntiJvmtiTest, DetectsAgentOptions)
{
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsAgentOption({"-Xmx256m", "-javaagent:c2j.jar"}));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsAgentOption({"-agentpath:C:/tmp/hook.dll"}));
    EXPECT_TRUE(
        mj2x::runtime::antijvmti::containsAgentOption({"-agentlib:jdwp=transport=dt_socket"}));
    EXPECT_FALSE(mj2x::runtime::antijvmti::containsAgentOption({"-Xmx256m", "-Dname=value"}));
}

TEST(RuntimeAntiJvmtiTest, DetectsJdwpOptionsSeparately)
{
    EXPECT_TRUE(
        mj2x::runtime::antijvmti::containsJdwpOption({"-agentlib:jdwp=transport=dt_socket"}));
    EXPECT_TRUE(mj2x::runtime::antijvmti::containsJdwpOption({"-Xrunjdwp:transport=dt_socket"}));
    EXPECT_FALSE(mj2x::runtime::antijvmti::containsJdwpOption({"-Xmx256m", "-Dname=value"}));
}

TEST(RuntimeAntiJvmtiTest, DetectsSuspiciousModulesWithoutLaunchArgs)
{
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

TEST(RuntimeAntiJvmtiTest, DetectsDumpRelevantCapabilities)
{
    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot none;
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(none));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot bytecodes;
    bytecodes.canGetBytecodes = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(bytecodes));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot retransformTag;
    retransformTag.canTagObjects         = true;
    retransformTag.canRetransformClasses = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(retransformTag));

    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot classHooks;
    classHooks.canGenerateAllClassHookEvents = true;
    classHooks.canRetransformClasses         = true;
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasDumpRelevantJvmtiCapabilities(classHooks));
}

TEST(RuntimeAntiJvmtiTest, AggregatesAgentSignals)
{
    mj2x::runtime::antijvmti::JvmtiCapabilitySnapshot capabilities;
    capabilities.canGenerateAllClassHookEvents = true;
    capabilities.canRetransformClasses         = true;

    auto signals = mj2x::runtime::antijvmti::collectAgentSignals(
        {"-Xmx256m"}, {"C:/tmp/arthas-agent.dll"}, capabilities);
    EXPECT_TRUE(signals.suspiciousJavaAgentModule);
    EXPECT_TRUE(signals.dumpRelevantJvmtiCapabilities);
    EXPECT_TRUE(mj2x::runtime::antijvmti::hasAgentSignal(signals));

    mj2x::runtime::antijvmti::AgentSignals none;
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasAgentSignal(none));
}

TEST(RuntimeAntiJvmtiTest, EnumeratesLiveModulesWithoutAssumingAgentNames)
{
    const auto modules = mj2x::runtime::antijvmti::enumerateLoadedModules();
    if (modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Success) {
        EXPECT_FALSE(modules.moduleNames.empty());
    }
    else {
        EXPECT_TRUE(modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable ||
                    modules.status == mj2x::runtime::antijvmti::LiveProbeStatus::Failed);
    }
}

TEST(RuntimeAntiJvmtiTest, LiveJvmtiQueryRequiresAnExplicitJavaVm)
{
    const auto query = mj2x::runtime::antijvmti::queryLiveJvmtiCapabilities(nullptr);
    EXPECT_EQ(query.status, mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable);
}

TEST(RuntimeAntiJvmtiTest, CollectsLiveAgentProbeWithModuleFacts)
{
    const auto probe = mj2x::runtime::antijvmti::collectLiveAgentSignals({}, nullptr);
    EXPECT_EQ(probe.jvmti.status, mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable);
    EXPECT_FALSE(mj2x::runtime::antijvmti::hasAgentSignal(probe.signals));
}

TEST(RuntimeAntiJvmtiTest, SplitsCommandLineWithQuotedTokens)
{
    using mj2x::runtime::antijvmti::splitCommandLineArgs;
    const auto simple = splitCommandLineArgs("java -Xmx256m -jar app.jar");
    ASSERT_EQ(simple.size(), 4u);
    EXPECT_EQ(simple[0], "java");
    EXPECT_EQ(simple[3], "app.jar");

    const auto quoted =
        splitCommandLineArgs("java -javaagent:\"C:\\path with space\\agent.jar\" Main");
    ASSERT_EQ(quoted.size(), 3u);
    EXPECT_EQ(quoted[1], "-javaagent:C:\\path with space\\agent.jar");
    EXPECT_EQ(quoted[2], "Main");

    const auto singleQuoted = splitCommandLineArgs("java '-agentlib:jdwp=transport=dt_socket' -cp a");
    ASSERT_EQ(singleQuoted.size(), 4u);
    EXPECT_EQ(singleQuoted[1], "-agentlib:jdwp=transport=dt_socket");

    EXPECT_TRUE(splitCommandLineArgs("").empty());
    EXPECT_EQ(splitCommandLineArgs("   ").size(), 0u);
}

TEST(RuntimeAntiJvmtiTest, AutoCollectsProcessArgumentsWithoutCrashing)
{
    // 自动收集版本：进程命令行（Windows GetCommandLineW / Linux /proc）不可
    // mock，只断言不崩且聚合路径可用；显式参数版本覆盖信号逻辑。
    const auto probe = mj2x::runtime::antijvmti::collectLiveAgentSignals(nullptr);
    EXPECT_EQ(probe.jvmti.status, mj2x::runtime::antijvmti::LiveProbeStatus::Unavailable);
}

TEST(RuntimeAntiDebugTest, AggregatesDebugSignals)
{
    mj2x::runtime::antidebug::DebugSignals none;
    EXPECT_FALSE(mj2x::runtime::antidebug::hasDebugSignal(none));

    mj2x::runtime::antidebug::DebugSignals local;
    local.debuggerPresent = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(local));

    auto current = mj2x::runtime::antidebug::collectDebugSignals();
    (void)current;
}

TEST(RuntimeAntiDebugTest, DetectsSoftwareBreakpointPatterns)
{
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

TEST(RuntimeAntiDebugTest, DetectsWindowsSnapshotPrimitives)
{
    EXPECT_TRUE(mj2x::runtime::antidebug::hasSuspiciousNtGlobalFlag(0x70));
    EXPECT_FALSE(mj2x::runtime::antidebug::hasSuspiciousNtGlobalFlag(0));
    EXPECT_FALSE(mj2x::runtime::antidebug::hasEnabledHardwareBreakpoint(0, 0, 0, 0, 0));
    EXPECT_TRUE(mj2x::runtime::antidebug::hasEnabledHardwareBreakpoint(0x1234, 0, 0, 0, 0x1));
}

TEST(RuntimeAntiDebugTest, DetectsTracerPidFromProcStatusText)
{
    using mj2x::runtime::antidebug::hasTracerPid;
    // 正常：TracerPid 为 0。
    EXPECT_FALSE(hasTracerPid("Name:\ttest\nState:\tS (sleeping)\nTracerPid:\t0\nUid:\t0\n"));
    // 被跟踪：TracerPid 非零。
    EXPECT_TRUE(hasTracerPid("TracerPid:\t12345\n"));
    EXPECT_TRUE(hasTracerPid("Name:\ttest\nTracerPid: 9876\n"));
    // 字段缺失 / 空输入。
    EXPECT_FALSE(hasTracerPid("Name:\ttest\nState:\tR\n"));
    EXPECT_FALSE(hasTracerPid(""));
    EXPECT_FALSE(hasTracerPid(nullptr));
    // 只匹配字段前缀，不匹配 TracerPidXxx。
    EXPECT_FALSE(hasTracerPid("TracerPidX:\t5\n"));
}

TEST(RuntimeAntiDebugTest, DetectsWritableExecutableMappings)
{
    using mj2x::runtime::antidebug::hasWritableExecutableMapping;
    const char* clean =
        "00400000-00452000 r-xp 00000000 08:01 123 /usr/bin/test\n"
        "00652000-00653000 rw-p 00052000 08:01 123 /usr/bin/test\n"
        "7f000000-7f001000 r-xp 00000000 08:01 456 /lib/libc.so\n";
    EXPECT_FALSE(hasWritableExecutableMapping(clean));
    // rwxp 段（注入器/自修改代码特征）。
    const char* injected =
        "7f123000-7f124000 rwxp 00000000 00:00 0 \n"
        "00400000-00452000 r-xp 00000000 08:01 123 /usr/bin/test\n";
    EXPECT_TRUE(hasWritableExecutableMapping(injected));
    // 首行不是地址段的防御：行无空格前缀时不应误报。
    EXPECT_FALSE(hasWritableExecutableMapping("rwxp\n"));
    EXPECT_FALSE(hasWritableExecutableMapping(""));
    EXPECT_FALSE(hasWritableExecutableMapping(nullptr));
}

TEST(RuntimeAntiDebugTest, DetectsSuspiciousTimingGap)
{
    using mj2x::runtime::antidebug::hasSuspiciousTimingGap;
    EXPECT_TRUE(hasSuspiciousTimingGap(200ULL * 1000 * 1000, 100ULL * 1000 * 1000));
    EXPECT_FALSE(hasSuspiciousTimingGap(50ULL * 1000 * 1000, 100ULL * 1000 * 1000));
    // 边界：等于阈值不算超。
    EXPECT_FALSE(hasSuspiciousTimingGap(100ULL * 1000 * 1000, 100ULL * 1000 * 1000));
    // 弱信号不进 hasDebugSignal 聚合。
    mj2x::runtime::antidebug::DebugSignals weak;
    weak.timingGapSuspicious = true;
    EXPECT_FALSE(mj2x::runtime::antidebug::hasDebugSignal(weak));
}

TEST(RuntimeAntiDebugTest, AggregatesLinuxStrongSignals)
{
    mj2x::runtime::antidebug::DebugSignals traced;
    traced.tracerPidAttached = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(traced));

    mj2x::runtime::antidebug::DebugSignals rwx;
    rwx.writableExecutableMapping = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(rwx));

    // 探测状态字段不进聚合。
    mj2x::runtime::antidebug::DebugSignals healthy;
    healthy.ptraceTracemeSucceeded = true;
    healthy.procFsProbeAvailable   = true;
    EXPECT_FALSE(mj2x::runtime::antidebug::hasDebugSignal(healthy));
}

TEST(RuntimeDetectionTest, AppliesFailClosedPolicyWithoutTerminating)
{
    mj2x::runtime::detection::RuntimeDetectionReport clean;
    EXPECT_FALSE(mj2x::runtime::detection::shouldFailClosed(clean));

    clean.suspicious = true;
    EXPECT_TRUE(mj2x::runtime::detection::shouldFailClosed(clean));

    clean.suspicious  = false;
    clean.probeFailed = true;
    EXPECT_TRUE(mj2x::runtime::detection::shouldFailClosed(clean));
    EXPECT_FALSE(mj2x::runtime::detection::shouldFailClosed(
        clean, mj2x::runtime::detection::DetectionPolicy{true, false}));
}

TEST(RuntimeEnforcementTest, CheckpointHasDeterministicLowerBound)
{
    using mj2x::runtime::detection::CheckpointState;
    using mj2x::runtime::detection::EnforcementPolicy;
    using mj2x::runtime::detection::makeCheckpointState;
    using mj2x::runtime::detection::checkpointShouldTrigger;

    CheckpointState state = makeCheckpointState(0x1234u);
    EnforcementPolicy policy;
    policy.checkpointStep = 3;
    bool hit = false;
    for (int i = 0; i < 3; ++i) {
        hit = checkpointShouldTrigger(state, policy) || hit;
    }
    // step=3：3 次调用内必然至少一次触发（确定性下限）。
    EXPECT_TRUE(hit);
    EXPECT_EQ(state.invocations, 3u);
    EXPECT_GE(state.triggers, 1u);

    // 相同 seed → 相同触发序列（确定性，可复现测试）。
    CheckpointState a = makeCheckpointState(0xABCDu);
    CheckpointState b = makeCheckpointState(0xABCDu);
    policy.checkpointStep = 5;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(checkpointShouldTrigger(a, policy), checkpointShouldTrigger(b, policy));
    }
}

TEST(RuntimeEnforcementTest, ForceTriggerSkipsProbability)
{
    using mj2x::runtime::detection::CheckpointState;
    using mj2x::runtime::detection::EnforcementAction;
    using mj2x::runtime::detection::EnforcementOutcome;
    using mj2x::runtime::detection::EnforcementPolicy;
    using mj2x::runtime::detection::enforceCheckpoint;
    using mj2x::runtime::detection::makeCheckpointState;

    CheckpointState state = makeCheckpointState(1u);
    EnforcementPolicy policy;
    policy.checkpointStep = 1000;   // 概率路径几乎不可能命中，验证 force 语义。

    // LogOnly + force → 必然命中。
    policy.action = EnforcementAction::LogOnly;
    EXPECT_EQ(enforceCheckpoint(state, policy, true), EnforcementOutcome::TriggeredLogOnly);
    EXPECT_EQ(state.invocations, 1u);
    EXPECT_EQ(state.triggers, 1u);

    // FakeData / Terminate 只返回 outcome，不执行。
    policy.action = EnforcementAction::FakeData;
    EXPECT_EQ(enforceCheckpoint(state, policy, true), EnforcementOutcome::TriggeredFakeData);
    policy.action = EnforcementAction::Terminate;
    EXPECT_EQ(enforceCheckpoint(state, policy, true), EnforcementOutcome::TriggeredTerminate);

    // Delay：delayMs=0 不睡眠，仍返回 TriggeredDelay。
    policy.action = EnforcementAction::Delay;
    policy.delayMs = 0;
    EXPECT_EQ(enforceCheckpoint(state, policy, true), EnforcementOutcome::TriggeredDelay);

    // 非 force 且未到步长 → NoTrigger（step 很大，概率忽略不计）。
    policy.action = EnforcementAction::LogOnly;
    CheckpointState idle = makeCheckpointState(2u);
    EXPECT_EQ(enforceCheckpoint(idle, policy), EnforcementOutcome::NoTrigger);
}

TEST(RuntimeEnforcementTest, CheckAndEnforceRunsLiveProbes)
{
    using mj2x::runtime::detection::CheckpointState;
    using mj2x::runtime::detection::EnforcementAction;
    using mj2x::runtime::detection::EnforcementOutcome;
    using mj2x::runtime::detection::EnforcementPolicy;
    using mj2x::runtime::detection::checkAndEnforce;
    using mj2x::runtime::detection::makeCheckpointState;

    EnforcementPolicy policy;
    policy.action = EnforcementAction::LogOnly;
    CheckpointState state = makeCheckpointState(3u);
    auto            outcome = checkAndEnforce(state, policy, {}, nullptr);
    // 干净进程下：NoTrigger 或概率触发 LogOnly（探测本身不终止/不睡眠）。
    EXPECT_TRUE(outcome == EnforcementOutcome::NoTrigger ||
                outcome == EnforcementOutcome::TriggeredLogOnly);
    EXPECT_GE(state.invocations, 1u);
}

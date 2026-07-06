#include <gmock/gmock.h>

#include "antidebug/antidebug.hpp"
#include "antijvmti/antijvmti.hpp"

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

TEST(RuntimeAntiDebugTest, AggregatesDebugSignals) {
    mj2x::runtime::antidebug::DebugSignals none;
    EXPECT_FALSE(mj2x::runtime::antidebug::hasDebugSignal(none));

    mj2x::runtime::antidebug::DebugSignals local;
    local.debuggerPresent = true;
    EXPECT_TRUE(mj2x::runtime::antidebug::hasDebugSignal(local));

    auto current = mj2x::runtime::antidebug::collectDebugSignals();
    (void)current;
}

#include "antijvmti.hpp"

#include <algorithm>
#include <cctype>

namespace mj2x::runtime::antijvmti {

namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsAnyToken(const std::vector<std::string>& values,
                      const std::vector<const char*>& tokens) {
    for (const auto& value : values) {
        std::string lowered = lowerAscii(value);
        for (const char* token : tokens) {
            if (lowered.find(token) != std::string::npos) return true;
        }
    }
    return false;
}

}  // namespace

JniTableSnapshot captureJniTable(const void* functions) {
    return JniTableSnapshot{functions, functions != nullptr};
}

bool isJniTableStable(const void* currentFunctions, const JniTableSnapshot& snapshot) {
    return snapshot.valid && currentFunctions != nullptr && currentFunctions == snapshot.functions;
}

bool containsAgentOption(const std::vector<std::string>& arguments) {
    return std::any_of(arguments.begin(), arguments.end(), [](const std::string& arg) {
        std::string lowered = lowerAscii(arg);
        return lowered.rfind("-javaagent:", 0) == 0 ||
               lowered.rfind("-agentpath:", 0) == 0 ||
               lowered.rfind("-agentlib:", 0) == 0;
    });
}

bool containsJdwpOption(const std::vector<std::string>& arguments) {
    return containsAnyToken(arguments, {"jdwp", "xrunjdwp", "transport=dt_socket"});
}

bool containsSuspiciousAgentModule(const std::vector<std::string>& moduleNames) {
    return containsAnyToken(moduleNames, {
        "arthas",
        "bytebuddy",
        "byteman",
        "frida",
        "instrument",
        "javaagent",
        "jdwp",
        "jattach",
    });
}

bool containsSuspiciousJvmtiDumpModule(const std::vector<std::string>& moduleNames) {
    return containsAnyToken(moduleNames, {
        "jvmti",
        "dump",
        "mjt-dump",
        "morpher-dump",
        "classdump",
        "jvmtiagent",
    });
}

bool hasDumpRelevantJvmtiCapabilities(const JvmtiCapabilitySnapshot& capabilities) {
    return capabilities.canGetBytecodes ||
           capabilities.canGenerateClassFileLoadHookEvents ||
           (capabilities.canTagObjects && capabilities.canRetransformClasses) ||
           (capabilities.canGenerateAllClassHookEvents && capabilities.canRetransformClasses);
}

AgentSignals collectAgentSignals(const std::vector<std::string>& arguments,
                                 const std::vector<std::string>& moduleNames,
                                 const JvmtiCapabilitySnapshot& capabilities) {
    AgentSignals signals;
    signals.launchAgentOption = containsAgentOption(arguments);
    signals.jdwpOption = containsJdwpOption(arguments);
    signals.suspiciousJavaAgentModule = containsSuspiciousAgentModule(moduleNames);
    signals.suspiciousJvmtiDumpModule = containsSuspiciousJvmtiDumpModule(moduleNames);
    signals.dumpRelevantJvmtiCapabilities = hasDumpRelevantJvmtiCapabilities(capabilities);
    return signals;
}

bool hasAgentSignal(const AgentSignals& signals) {
    return signals.launchAgentOption ||
           signals.jdwpOption ||
           signals.suspiciousJavaAgentModule ||
           signals.suspiciousJvmtiDumpModule ||
           signals.dumpRelevantJvmtiCapabilities;
}

}  // namespace mj2x::runtime::antijvmti

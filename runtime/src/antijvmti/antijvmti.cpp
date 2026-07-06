#include "antijvmti.hpp"

#include <algorithm>

namespace mj2x::runtime::antijvmti {

JniTableSnapshot captureJniTable(const void* functions) {
    return JniTableSnapshot{functions, functions != nullptr};
}

bool isJniTableStable(const void* currentFunctions, const JniTableSnapshot& snapshot) {
    return snapshot.valid && currentFunctions != nullptr && currentFunctions == snapshot.functions;
}

bool containsAgentOption(const std::vector<std::string>& arguments) {
    return std::any_of(arguments.begin(), arguments.end(), [](const std::string& arg) {
        return arg.rfind("-javaagent:", 0) == 0 ||
               arg.rfind("-agentpath:", 0) == 0 ||
               arg.rfind("-agentlib:", 0) == 0 ||
               arg.find("jdwp") != std::string::npos;
    });
}

}  // namespace mj2x::runtime::antijvmti

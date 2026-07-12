#include "antijvmti.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <utility>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

#    include <tlhelp32.h>
#endif

#ifdef MJ2X_RUNTIME_HAS_JVMTI
#    include <jni.h>
#    include <jvmti.h>
#endif

namespace mj2x::runtime::antijvmti {

namespace {

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsAnyToken(const std::vector<std::string>& values,
                      const std::vector<const char*>& tokens)
{
    for (const auto& value : values) {
        std::string lowered = lowerAscii(value);
        for (const char* token : tokens) {
            if (lowered.find(token) != std::string::npos) return true;
        }
    }
    return false;
}

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* value)
{
    if (value == nullptr || *value == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}
#endif

}   // namespace

JniTableSnapshot captureJniTable(const void* functions)
{
    return JniTableSnapshot{functions, functions != nullptr};
}

bool isJniTableStable(const void* currentFunctions, const JniTableSnapshot& snapshot)
{
    return snapshot.valid && currentFunctions != nullptr && currentFunctions == snapshot.functions;
}

bool containsAgentOption(const std::vector<std::string>& arguments)
{
    return std::any_of(arguments.begin(), arguments.end(), [](const std::string& arg) {
        std::string lowered = lowerAscii(arg);
        return lowered.rfind("-javaagent:", 0) == 0 || lowered.rfind("-agentpath:", 0) == 0 ||
               lowered.rfind("-agentlib:", 0) == 0;
    });
}

bool containsJdwpOption(const std::vector<std::string>& arguments)
{
    return containsAnyToken(arguments, {"jdwp", "xrunjdwp", "transport=dt_socket"});
}

bool containsSuspiciousAgentModule(const std::vector<std::string>& moduleNames)
{
    return containsAnyToken(moduleNames,
                            {
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

bool containsSuspiciousJvmtiDumpModule(const std::vector<std::string>& moduleNames)
{
    return containsAnyToken(moduleNames,
                            {
                                "jvmti",
                                "dump",
                                "mjt-dump",
                                "morpher-dump",
                                "classdump",
                                "jvmtiagent",
                            });
}

bool hasDumpRelevantJvmtiCapabilities(const JvmtiCapabilitySnapshot& capabilities)
{
    return capabilities.canGetBytecodes ||
           (capabilities.canTagObjects && capabilities.canRetransformClasses) ||
           (capabilities.canGenerateAllClassHookEvents && capabilities.canRetransformClasses);
}

LoadedModuleSnapshot enumerateLoadedModules()
{
    LoadedModuleSnapshot snapshot;
#ifdef _WIN32
    HANDLE moduleSnapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (moduleSnapshot == INVALID_HANDLE_VALUE) {
        snapshot.status = LiveProbeStatus::Failed;
        return snapshot;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(moduleSnapshot, &entry) == FALSE) {
        CloseHandle(moduleSnapshot);
        snapshot.status = LiveProbeStatus::Failed;
        return snapshot;
    }
    BOOL hasEntry = TRUE;
    do {
        std::string path = wideToUtf8(entry.szExePath);
        if (path.empty()) path = wideToUtf8(entry.szModule);
        if (!path.empty()) snapshot.moduleNames.push_back(std::move(path));
        entry.dwSize = sizeof(entry);
        hasEntry     = Module32NextW(moduleSnapshot, &entry);
    } while (hasEntry != FALSE);
    const DWORD enumerationError = GetLastError();
    CloseHandle(moduleSnapshot);
    snapshot.status = enumerationError == ERROR_NO_MORE_FILES ? LiveProbeStatus::Success
                                                              : LiveProbeStatus::Failed;
#elif defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        snapshot.status = LiveProbeStatus::Failed;
        return snapshot;
    }
    std::set<std::string> paths;
    std::string           line;
    while (std::getline(maps, line)) {
        const auto pathStart = line.find('/');
        if (pathStart != std::string::npos) paths.insert(line.substr(pathStart));
    }
    snapshot.moduleNames.assign(paths.begin(), paths.end());
    snapshot.status = LiveProbeStatus::Success;
#else
    snapshot.status = LiveProbeStatus::Unavailable;
#endif
    return snapshot;
}

JvmtiCapabilityQuery queryLiveJvmtiCapabilities(void* javaVm)
{
    JvmtiCapabilityQuery query;
    if (javaVm == nullptr) return query;
#ifdef MJ2X_RUNTIME_HAS_JVMTI
    auto*      vm           = static_cast<JavaVM*>(javaVm);
    jvmtiEnv*  jvmti        = nullptr;
    const jint getEnvStatus = vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2);
    if (getEnvStatus != JNI_OK || jvmti == nullptr) return query;

    jvmtiCapabilities capabilities{};
    if (jvmti->GetCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
        query.status = LiveProbeStatus::Failed;
        return query;
    }
    query.status                     = LiveProbeStatus::Success;
    query.capabilities.canTagObjects = capabilities.can_tag_objects != 0;
    query.capabilities.canGenerateAllClassHookEvents =
        capabilities.can_generate_all_class_hook_events != 0;
    query.capabilities.canGetBytecodes       = capabilities.can_get_bytecodes != 0;
    query.capabilities.canRetransformClasses = capabilities.can_retransform_classes != 0;
#else
    (void)javaVm;
#endif
    return query;
}

AgentSignals collectAgentSignals(const std::vector<std::string>& arguments,
                                 const std::vector<std::string>& moduleNames,
                                 const JvmtiCapabilitySnapshot&  capabilities)
{
    AgentSignals signals;
    signals.launchAgentOption             = containsAgentOption(arguments);
    signals.jdwpOption                    = containsJdwpOption(arguments);
    signals.suspiciousJavaAgentModule     = containsSuspiciousAgentModule(moduleNames);
    signals.suspiciousJvmtiDumpModule     = containsSuspiciousJvmtiDumpModule(moduleNames);
    signals.dumpRelevantJvmtiCapabilities = hasDumpRelevantJvmtiCapabilities(capabilities);
    return signals;
}

LiveAgentProbe collectLiveAgentSignals(const std::vector<std::string>& arguments, void* javaVm)
{
    LiveAgentProbe probe;
    probe.modules = enumerateLoadedModules();
    probe.jvmti   = queryLiveJvmtiCapabilities(javaVm);
    probe.signals =
        collectAgentSignals(arguments, probe.modules.moduleNames, probe.jvmti.capabilities);
    return probe;
}

bool hasAgentSignal(const AgentSignals& signals)
{
    return signals.launchAgentOption || signals.jdwpOption || signals.suspiciousJavaAgentModule ||
           signals.suspiciousJvmtiDumpModule || signals.dumpRelevantJvmtiCapabilities;
}

}   // namespace mj2x::runtime::antijvmti

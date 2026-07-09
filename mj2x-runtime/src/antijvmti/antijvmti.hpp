/**
 * @file antijvmti.hpp
 * @brief Mockable anti-JVMTI probes for the R3 runtime.
 *
 * @details
 * The first c2j-facing anchor is JNI vtable interception.  This module keeps
 * the low-level probe small and testable by snapshotting the `env->functions`
 * pointer as an opaque address.  Generated/runtime JNI adapters can pass the
 * real table pointer, while unit tests can inject fake addresses without a JVM.
 */
#pragma once

#include <string>
#include <vector>

namespace mj2x::runtime::antijvmti {

/// @brief Snapshot of an expected JNI function table address.
struct JniTableSnapshot {
    const void* functions = nullptr;
    bool valid = false;
};

/**
 * @brief Capture the current JNI function table pointer.
 *
 * @param functions Opaque `env->functions` address.
 * @return A valid snapshot when `functions` is non-null.
 */
JniTableSnapshot captureJniTable(const void* functions);

/**
 * @brief Check whether the current JNI function table still matches a snapshot.
 */
bool isJniTableStable(const void* currentFunctions, const JniTableSnapshot& snapshot);

/// @brief Java/JVMTI 侧可疑信号集合。
struct AgentSignals {
    bool launchAgentOption = false;
    bool jdwpOption = false;
    bool suspiciousJavaAgentModule = false;
    bool suspiciousJvmtiDumpModule = false;
    bool dumpRelevantJvmtiCapabilities = false;
};

/**
 * @brief JVMTI capability 的最小可测试快照。
 *
 * @details 真实 JVMTI capability 查询在 JVM 集成层完成;runtime core 只消费已经
 *          抽取出的布尔位,避免单测依赖 live JVM。
 */
struct JvmtiCapabilitySnapshot {
    bool canTagObjects = false;
    bool canGenerateAllClassHookEvents = false;
    bool canGenerateClassFileLoadHookEvents = false;
    bool canGetBytecodes = false;
    bool canRetransformClasses = false;
};

/**
 * @brief Detect command-line options that usually indicate a Java agent.
 *
 * @details
 * This is a deterministic string-level probe for tests and early runtime
 * integration.  Process/JVM argument collection stays platform-specific and is
 * layered above this function.
 */
bool containsAgentOption(const std::vector<std::string>& arguments);

/**
 * @brief 检测 JDWP/IDE 调试参数。
 */
bool containsJdwpOption(const std::vector<std::string>& arguments);

/**
 * @brief 从已加载模块名中识别常见 Java agent / attach 工具痕迹。
 *
 * @param moduleNames 进程模块文件名或完整路径,大小写不敏感。
 */
bool containsSuspiciousAgentModule(const std::vector<std::string>& moduleNames);

/**
 * @brief 从已加载模块名中识别 JVMTI dump/trace 类工具痕迹。
 */
bool containsSuspiciousJvmtiDumpModule(const std::vector<std::string>& moduleNames);

/**
 * @brief 判断 capability 组合是否足以支撑 class dump / bytecode dump。
 */
bool hasDumpRelevantJvmtiCapabilities(const JvmtiCapabilitySnapshot& capabilities);

/**
 * @brief 聚合参数、模块和 capability 信号。
 */
AgentSignals collectAgentSignals(const std::vector<std::string>& arguments,
                                 const std::vector<std::string>& moduleNames,
                                 const JvmtiCapabilitySnapshot& capabilities);

/// @brief 任一 Java/JVMTI agent 信号命中时返回 true。
bool hasAgentSignal(const AgentSignals& signals);

}  // namespace mj2x::runtime::antijvmti

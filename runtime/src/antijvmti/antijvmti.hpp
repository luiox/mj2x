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

/**
 * @brief Detect command-line options that usually indicate a Java agent.
 *
 * @details
 * This is a deterministic string-level probe for tests and early runtime
 * integration.  Process/JVM argument collection stays platform-specific and is
 * layered above this function.
 */
bool containsAgentOption(const std::vector<std::string>& arguments);

}  // namespace mj2x::runtime::antijvmti

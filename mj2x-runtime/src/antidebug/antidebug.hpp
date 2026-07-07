/**
 * @file antidebug.hpp
 * @brief Platform anti-debug probes for the mj2x R3 runtime.
 *
 * @details
 * The module reports observable debugger signals without deciding policy.
 * Callers can combine the returned flags with anti-JVMTI and resource HMAC
 * checks to fail closed.  Windows uses documented debugger probes in this first
 * slice; unsupported platforms return a clean "not detected" result.
 */
#pragma once

namespace mj2x::runtime::antidebug {

/// @brief Collected debugger signal flags.
struct DebugSignals {
    bool debuggerPresent = false;
    bool remoteDebuggerPresent = false;
};

/**
 * @brief Collect platform debugger signals.
 *
 * @return Debug signal flags.  All fields are false when the platform probe is
 * unavailable.
 */
DebugSignals collectDebugSignals();

/// @brief Convenience predicate for any collected debugger signal.
bool hasDebugSignal(const DebugSignals& signals);

}  // namespace mj2x::runtime::antidebug

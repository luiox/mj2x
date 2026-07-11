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

#include <cstddef>
#include <cstdint>

namespace mj2x::runtime::antidebug {

/// @brief Collected debugger signal flags.
struct DebugSignals {
    bool debuggerPresent = false;
    bool remoteDebuggerPresent = false;
    bool pebBeingDebugged = false;
    bool pebDebugHeapFlags = false;
    bool processDebugPort = false;
    bool processDebugObject = false;
    bool processDebugFlags = false;
    bool hardwareBreakpoint = false;
    bool softwareBreakpointPattern = false;
    bool platformProbeAvailable = false;
    bool pebProbeAvailable = false;
    bool ntQueryProbeAvailable = false;
    bool hardwareBreakpointProbeAvailable = false;
    bool nativeProbeFailed = false;
};

/**
 * @brief Collect platform debugger signals.
 *
 * @return Debug signal flags.  All fields are false when the platform probe is
 * unavailable.
 */
DebugSignals collectDebugSignals();

/**
 * @brief Check the Windows PEB NtGlobalFlag debugger-related heap bits.
 *
 * @details This helper is platform-neutral so snapshot mapping remains directly
 * testable without attaching a debugger.
 */
bool hasSuspiciousNtGlobalFlag(std::uint32_t ntGlobalFlag);

/**
 * @brief Check whether x86/x64 debug registers describe an enabled breakpoint.
 */
bool hasEnabledHardwareBreakpoint(std::uintptr_t dr0,
                                  std::uintptr_t dr1,
                                  std::uintptr_t dr2,
                                  std::uintptr_t dr3,
                                  std::uintptr_t dr7);

/**
 * @brief 扫描内存片段中是否存在常见软件断点指令。
 *
 * @param data 起始地址;允许为空,为空时返回 false。
 * @param size 可读字节数。
 * @return 检测到 `INT3`(`0xCC`) 或 `INT 3`(`0xCD 0x03`) 时返回 true。
 *
 * @note 该函数只做纯字节匹配,用于 mockable 单测和未来代码段探测的底层积木;
 *       不负责枚举模块或修改页保护。
 */
bool containsSoftwareBreakpoint(const void* data, std::size_t size);

/// @brief Convenience predicate for any collected debugger signal.
bool hasDebugSignal(const DebugSignals& signals);

}  // namespace mj2x::runtime::antidebug

/// @defgroup runtime_antidebug 运行时 Anti-Debug
///
/// 为 R3 fail-closed 策略决策收集平台调试器信号。
/// 补充 anti-JVMTI 检查；两个信号家族单独都不够强。
///
/// ## 设计
///
/// 模块返回简单信号结构而非抛出或退出。
/// Windows 组合文档化调试器探测（`IsDebuggerPresent`、`CheckRemoteDebuggerPresent`）
/// 与三个独立 native 表面：
/// - PEB `BeingDebugged` 和 `NtGlobalFlag` heap 位
/// - 动态解析的 `NtQueryInformationProcess` 查询
/// - x86/x64 调试寄存器检查硬件断点
///
/// Linux 侧（`__linux__`）：
/// - `/proc/self/status` 的 `TracerPid` 字段（ptrace 附着检测，强信号）
/// - `ptrace(PTRACE_TRACEME)` 自检（成功 = 未被跟踪；EPERM 且 TracerPid 非零
///   = 已被跟踪；EPERM 且 TracerPid 为零 = 探测不可用，见 `nativeProbeFailed`）
/// - `/proc/self/maps` 的可写可执行段（`rwxp`/`rwx`，注入/调试器特征）
/// - 单调时钟时间差探测（弱信号，默认不进 `hasDebugSignal` 聚合，防调度误报）
///
/// mockable 核心还暴露字节模式扫描（`0xCC`、`0xCD 0x03`）、纯 NtGlobalFlag/
/// 调试寄存器谓词，以及 Linux 的文本解析谓词（`hasTracerPid`、
/// `hasWritableExecutableMapping`、`hasSuspiciousTimingGap`）。

/// @file antidebug.hpp
/// @brief Platform anti-debug probes for the mj2x R3 runtime.
///
/// @details
/// The module reports observable debugger signals without deciding policy.
/// Callers can combine the returned flags with anti-JVMTI and resource HMAC
/// checks to fail closed.  Windows uses documented debugger probes in this first
/// slice; unsupported platforms return a clean "not detected" result.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mj2x::runtime::antidebug {

/// @brief Collected debugger signal flags.
struct DebugSignals
{
    // Windows 家族。
    bool debuggerPresent                  = false;
    bool remoteDebuggerPresent            = false;
    bool pebBeingDebugged                 = false;
    bool pebDebugHeapFlags                = false;
    bool processDebugPort                 = false;
    bool processDebugObject               = false;
    bool processDebugFlags                = false;
    bool hardwareBreakpoint               = false;
    bool softwareBreakpointPattern        = false;
    bool platformProbeAvailable           = false;
    bool pebProbeAvailable                = false;
    bool ntQueryProbeAvailable            = false;
    bool hardwareBreakpointProbeAvailable = false;
    bool nativeProbeFailed                = false;
    // Linux 家族。
    bool tracerPidAttached           = false;   ///< /proc/self/status TracerPid != 0。
    bool ptraceTracemeSucceeded      = false;   ///< PTRACE_TRACEME 成功 = 未被跟踪。
    bool writableExecutableMapping   = false;   ///< /proc/self/maps 存在 rwx/rwxp 段。
    bool timingGapSuspicious         = false;   ///< 单调时钟时间差超阈值（弱信号）。
    bool procFsProbeAvailable        = false;   ///< /proc/self/status 可读。
    bool mapsProbeAvailable          = false;   ///< /proc/self/maps 可读。
    bool ptraceProbeAvailable        = false;   ///< ptrace 探测可用。
    bool timingProbeAvailable        = false;   ///< 时间差探测可用。
};

/// @brief Collect platform debugger signals.
///
/// @return Debug signal flags.  All fields are false when the platform probe is
/// unavailable.
DebugSignals collectDebugSignals();

/// @brief Check the Windows PEB NtGlobalFlag debugger-related heap bits.
///
/// @details This helper is platform-neutral so snapshot mapping remains directly
/// testable without attaching a debugger.
bool hasSuspiciousNtGlobalFlag(std::uint32_t ntGlobalFlag);

/// @brief Check whether x86/x64 debug registers describe an enabled breakpoint.
bool hasEnabledHardwareBreakpoint(std::uintptr_t dr0, std::uintptr_t dr1, std::uintptr_t dr2,
                                  std::uintptr_t dr3, std::uintptr_t dr7);

/// @brief 扫描内存片段中是否存在常见软件断点指令。
///
/// @param data 起始地址;允许为空,为空时返回 false。
/// @param size 可读字节数。
/// @return 检测到 `INT3`(`0xCC`) 或 `INT 3`(`0xCD 0x03`) 时返回 true。
///
/// @note 该函数只做纯字节匹配,用于 mockable 单测和未来代码段探测的底层积木;
///       不负责枚举模块或修改页保护。
bool containsSoftwareBreakpoint(const void* data, std::size_t size);

/// @brief 解析 `/proc/self/status` 文本，检测 `TracerPid` 字段非零。
///
/// @param statusText status 文件全文（或至少含 `TracerPid:` 行的前缀）；空指针
/// 或找不到字段时返回 false。
///
/// @note 纯文本解析谓词，供单测 mock 与 Linux 探测共用。
bool hasTracerPid(const char* statusText);

/// @brief 解析 `/proc/self/maps` 文本，检测可写可执行段（perms 含 `w` 与 `x`）。
///
/// @param mapsText maps 文件全文；空指针时返回 false。
///
/// @note 纯文本解析谓词。正常共享库为 r-xp/rw-p；rwxp/rwx 段常见于注入器、
/// 调试器 shellcode 或自修改代码，是 JVM 进程的可疑特征。
bool hasWritableExecutableMapping(const char* mapsText);

/// @brief 单调时钟时间差是否超阈值。
///
/// @param elapsedNs   实测耗时（纳秒）。
/// @param thresholdNs 阈值（纳秒）。
///
/// @note 纯比较谓词；时间差探测本身脆弱（调度/节能/超线程），作为弱信号
/// 报告，不进 `hasDebugSignal` 聚合。
bool hasSuspiciousTimingGap(std::uint64_t elapsedNs, std::uint64_t thresholdNs);

/// @brief Convenience predicate for any collected debugger signal.
///
/// @details 聚合所有强/中信号：Windows 全部字段 + Linux 的 `tracerPidAttached`
/// 与 `writableExecutableMapping`。弱信号（`timingGapSuspicious`）与探测状态
/// 字段（`*ProbeAvailable`/`ptraceTracemeSucceeded`）不进聚合，防调度误报。
bool hasDebugSignal(const DebugSignals& signals);

}   // namespace mj2x::runtime::antidebug

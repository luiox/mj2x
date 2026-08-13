// ptrace 请求常量与 CLOCK_MONOTONIC 需要 _GNU_SOURCE（或 POSIX 特性宏）。
// 必须在任何系统头（含 antidebug.hpp 间接引入的 <cstdint>）之前定义。
#ifdef __linux__
#    define _GNU_SOURCE
#endif

#include "antidebug.hpp"

#include <cerrno>
#include <cstdio>

#ifdef __linux__
#    include <sys/ptrace.h>
#    include <time.h>
#endif

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

#    include <intrin.h>
#endif

#include <cstdint>
#include <cstring>

namespace mj2x::runtime::antidebug {

namespace {

constexpr std::uint32_t kNtGlobalFlagDebugHeapMask = 0x70;

#ifdef _WIN32

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

constexpr ULONG kProcessDebugPort         = 7;
constexpr ULONG kProcessDebugObjectHandle = 30;
constexpr ULONG kProcessDebugFlags        = 31;
constexpr LONG  kStatusPortNotSet         = static_cast<LONG>(0xC0000353L);

bool ntSuccess(LONG status)
{
    return status >= 0;
}

void collectPebSignals(DebugSignals& signals)
{
#    if defined(_M_X64)
    const auto*           peb = reinterpret_cast<const std::uint8_t*>(__readgsqword(0x60));
    constexpr std::size_t ntGlobalFlagOffset = 0xBC;
#    elif defined(_M_IX86)
    const auto*           peb = reinterpret_cast<const std::uint8_t*>(__readfsdword(0x30));
    constexpr std::size_t ntGlobalFlagOffset = 0x68;
#    else
    const std::uint8_t*   peb                = nullptr;
    constexpr std::size_t ntGlobalFlagOffset = 0;
#    endif
    if (peb == nullptr) return;
    signals.platformProbeAvailable = true;
    signals.pebProbeAvailable      = true;
    signals.pebBeingDebugged       = peb[2] != 0;
    std::uint32_t ntGlobalFlag     = 0;
    std::memcpy(&ntGlobalFlag, peb + ntGlobalFlagOffset, sizeof(ntGlobalFlag));
    signals.pebDebugHeapFlags = hasSuspiciousNtGlobalFlag(ntGlobalFlag);
}

void collectNtQuerySignals(DebugSignals& signals)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        signals.nativeProbeFailed = true;
        return;
    }
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (query == nullptr) {
        signals.nativeProbeFailed = true;
        return;
    }
    signals.platformProbeAvailable = true;
    signals.ntQueryProbeAvailable  = true;

    ULONG_PTR  debugPort = 0;
    const LONG debugPortStatus =
        query(GetCurrentProcess(), kProcessDebugPort, &debugPort, sizeof(debugPort), nullptr);
    if (ntSuccess(debugPortStatus)) {
        signals.processDebugPort = debugPort != 0;
    }
    else {
        signals.nativeProbeFailed = true;
    }

    HANDLE     debugObject       = nullptr;
    const LONG debugObjectStatus = query(
        GetCurrentProcess(), kProcessDebugObjectHandle, &debugObject, sizeof(debugObject), nullptr);
    if (ntSuccess(debugObjectStatus)) {
        signals.processDebugObject = debugObject != nullptr;
        if (debugObject != nullptr) CloseHandle(debugObject);
    }
    else if (debugObjectStatus != kStatusPortNotSet) {
        signals.nativeProbeFailed = true;
    }

    ULONG      noDebugInherit   = 1;
    const LONG debugFlagsStatus = query(
        GetCurrentProcess(), kProcessDebugFlags, &noDebugInherit, sizeof(noDebugInherit), nullptr);
    if (ntSuccess(debugFlagsStatus)) {
        signals.processDebugFlags = noDebugInherit == 0;
    }
    else {
        signals.nativeProbeFailed = true;
    }
}

void collectHardwareBreakpointSignal(DebugSignals& signals)
{
#    if defined(_M_X64) || defined(_M_IX86)
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &context) == FALSE) {
        signals.nativeProbeFailed = true;
        return;
    }
    signals.platformProbeAvailable           = true;
    signals.hardwareBreakpointProbeAvailable = true;
    signals.hardwareBreakpoint =
        hasEnabledHardwareBreakpoint(static_cast<std::uintptr_t>(context.Dr0),
                                     static_cast<std::uintptr_t>(context.Dr1),
                                     static_cast<std::uintptr_t>(context.Dr2),
                                     static_cast<std::uintptr_t>(context.Dr3),
                                     static_cast<std::uintptr_t>(context.Dr7));
#    endif
}

#endif   // _WIN32

#ifdef __linux__

constexpr std::uint64_t kTimingThresholdNs  = 100ULL * 1000 * 1000;   // 100 ms，宽松防误报
constexpr std::uint64_t kTimingLoopIters    = 100000ULL;

bool readProcFile(const char* path, char* buffer, std::size_t capacity)
{
    if (buffer == nullptr || capacity == 0) return false;
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return false;
    const std::size_t read = std::fread(buffer, 1, capacity - 1, file);
    std::fclose(file);
    buffer[read] = '\0';
    return read > 0;
}

/// @brief /proc/self/status 文本（thread_local 缓存，避免多次读文件）。
/// 读取失败时返回空串。
const char* procStatusText()
{
    static thread_local char buffer[4096];
    if (!readProcFile("/proc/self/status", buffer, sizeof(buffer))) return "";
    return buffer;
}

void collectLinuxProcSignals(DebugSignals& signals)
{
    const char* status = procStatusText();
    if (status[0] != '\0') {
        signals.procFsProbeAvailable = true;
        signals.tracerPidAttached    = hasTracerPid(status);
    }
    char buffer[4096];
    if (readProcFile("/proc/self/maps", buffer, sizeof(buffer))) {
        signals.mapsProbeAvailable        = true;
        signals.writableExecutableMapping = hasWritableExecutableMapping(buffer);
    }
}

void collectLinuxPtraceSignal(DebugSignals& signals)
{
    // PTRACE_TRACEME 成功 = 当前未被跟踪（进程进入可被父进程跟踪的挂起标记，
    // 父进程不 attach 则无任何影响）。失败 EPERM：
    //   - TracerPid 非零 → 已被调试器/跟踪器附着（由 tracerPidAttached 表达）；
    //   - TracerPid 为零 → 探测被限制（seccomp/容器），标记 nativeProbeFailed，
    //     由上层 fail-closed policy 决策，不当作"被跟踪"。
    errno            = 0;
    const long trace = ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    if (trace == 0) {
        signals.ptraceProbeAvailable   = true;
        signals.ptraceTracemeSucceeded = true;
    }
    else if (errno == EPERM) {
        signals.ptraceProbeAvailable = true;
        signals.tracerPidAttached    = hasTracerPid(procStatusText());
        if (!signals.tracerPidAttached) signals.nativeProbeFailed = true;
    }
    else {
        signals.nativeProbeFailed = true;
    }
}

void collectLinuxTimingSignal(DebugSignals& signals)
{
    timespec start{};
    timespec end{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        signals.nativeProbeFailed = true;
        return;
    }
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < kTimingLoopIters; ++i) {
        sink += i;
    }
    (void)sink;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        signals.nativeProbeFailed = true;
        return;
    }
    const std::uint64_t elapsedNs = static_cast<std::uint64_t>(end.tv_sec - start.tv_sec) *
                                        1000ULL * 1000 * 1000 +
                                    static_cast<std::uint64_t>(end.tv_nsec - start.tv_nsec);
    signals.timingProbeAvailable = true;
    signals.timingGapSuspicious  = hasSuspiciousTimingGap(elapsedNs, kTimingThresholdNs);
}

#endif   // __linux__

}   // namespace

DebugSignals collectDebugSignals()
{
    DebugSignals signals;
#ifdef _WIN32
    signals.platformProbeAvailable = true;
    signals.debuggerPresent        = IsDebuggerPresent() != FALSE;
    BOOL remote                    = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) != FALSE) {
        signals.remoteDebuggerPresent = remote != FALSE;
    }
    else {
        signals.nativeProbeFailed = true;
    }
    collectPebSignals(signals);
    collectNtQuerySignals(signals);
    collectHardwareBreakpointSignal(signals);
#endif
#ifdef __linux__
    collectLinuxProcSignals(signals);
    collectLinuxPtraceSignal(signals);
    collectLinuxTimingSignal(signals);
#endif
    return signals;
}

bool hasSuspiciousNtGlobalFlag(std::uint32_t ntGlobalFlag)
{
    return (ntGlobalFlag & kNtGlobalFlagDebugHeapMask) != 0;
}

bool hasEnabledHardwareBreakpoint(std::uintptr_t dr0, std::uintptr_t dr1, std::uintptr_t dr2,
                                  std::uintptr_t dr3, std::uintptr_t dr7)
{
    const bool anyAddress     = dr0 != 0 || dr1 != 0 || dr2 != 0 || dr3 != 0;
    const bool anyEnabledSlot = (dr7 & 0xFFu) != 0;
    return anyAddress && anyEnabledSlot;
}

bool containsSoftwareBreakpoint(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] == 0xCC) return true;
        if (i + 1 < size && bytes[i] == 0xCD && bytes[i + 1] == 0x03) return true;
    }
    return false;
}

bool hasTracerPid(const char* statusText)
{
    if (statusText == nullptr) return false;
    // /proc/self/status 格式：`TracerPid:\t<pid>`（字段间用 tab）。
    // 逐行扫描，找 "TracerPid:" 前缀，解析其后的十进制数。
    const char* cursor = statusText;
    while (*cursor != '\0') {
        if (std::strncmp(cursor, "TracerPid:", 10) == 0) {
            const char* value = cursor + 10;
            while (*value == ' ' || *value == '\t' || *value == '\r') ++value;
            unsigned long pid = 0;
            if (*value >= '0' && *value <= '9') {
                while (*value >= '0' && *value <= '9') {
                    pid = pid * 10 + static_cast<unsigned long>(*value - '0');
                    ++value;
                }
            }
            return pid != 0;
        }
        // 跳到下一行。
        while (*cursor != '\0' && *cursor != '\n') ++cursor;
        if (*cursor == '\n') ++cursor;
    }
    return false;
}

bool hasWritableExecutableMapping(const char* mapsText)
{
    if (mapsText == nullptr) return false;
    // /proc/self/maps 行格式：`start-end perms offset dev inode path`。
    // perms 是 4 字符（如 r-xp / rw-p / rwxp）：位置 1 = 'w'，位置 2 = 'x'。
    const char* cursor = mapsText;
    while (*cursor != '\0') {
        // 跳过地址区间 `xxxx-xxxx `。
        while (*cursor != '\0' && *cursor != ' ') ++cursor;
        if (*cursor == ' ') ++cursor;
        // perms：rwx + private/shared 标志。
        if (cursor[0] != '\0' && cursor[1] == 'w' && cursor[2] == 'x') return true;
        // 跳到下一行。
        while (*cursor != '\0' && *cursor != '\n') ++cursor;
        if (*cursor == '\n') ++cursor;
    }
    return false;
}

bool hasSuspiciousTimingGap(std::uint64_t elapsedNs, std::uint64_t thresholdNs)
{
    return elapsedNs > thresholdNs;
}

bool hasDebugSignal(const DebugSignals& signals)
{
    // Windows 家族全部进聚合；Linux 只聚合强/中信号（tracerPidAttached 与
    // writableExecutableMapping）。timingGapSuspicious 是弱信号（调度/节能易
    // 误报），ptraceTracemeSucceeded 是"健康确认"而非可疑信号，均不进聚合；
    // 需要时由上层 policy 单独消费。
    return signals.debuggerPresent || signals.remoteDebuggerPresent || signals.pebBeingDebugged ||
           signals.pebDebugHeapFlags || signals.processDebugPort || signals.processDebugObject ||
           signals.processDebugFlags || signals.hardwareBreakpoint ||
           signals.softwareBreakpointPattern || signals.tracerPidAttached ||
           signals.writableExecutableMapping;
}

}   // namespace mj2x::runtime::antidebug

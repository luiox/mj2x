#include "antidebug.hpp"

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

#endif

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

bool hasDebugSignal(const DebugSignals& signals)
{
    return signals.debuggerPresent || signals.remoteDebuggerPresent || signals.pebBeingDebugged ||
           signals.pebDebugHeapFlags || signals.processDebugPort || signals.processDebugObject ||
           signals.processDebugFlags || signals.hardwareBreakpoint ||
           signals.softwareBreakpointPattern;
}

}   // namespace mj2x::runtime::antidebug

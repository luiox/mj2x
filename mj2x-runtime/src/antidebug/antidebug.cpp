#include "antidebug.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdint>

namespace mj2x::runtime::antidebug {

DebugSignals collectDebugSignals() {
    DebugSignals signals;
#ifdef _WIN32
    signals.debuggerPresent = IsDebuggerPresent() != FALSE;
    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) != FALSE) {
        signals.remoteDebuggerPresent = remote != FALSE;
    }
#endif
    return signals;
}

bool containsSoftwareBreakpoint(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] == 0xCC) return true;
        if (i + 1 < size && bytes[i] == 0xCD && bytes[i + 1] == 0x03) return true;
    }
    return false;
}

bool hasDebugSignal(const DebugSignals& signals) {
    return signals.debuggerPresent ||
           signals.remoteDebuggerPresent ||
           signals.softwareBreakpointPattern;
}

}  // namespace mj2x::runtime::antidebug

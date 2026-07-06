#include "antidebug.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

bool hasDebugSignal(const DebugSignals& signals) {
    return signals.debuggerPresent || signals.remoteDebuggerPresent;
}

}  // namespace mj2x::runtime::antidebug

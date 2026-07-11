# Runtime Anti-Debug Design

## Purpose

`runtime/src/antidebug/` collects platform debugger signals for R3 fail-closed
policy decisions.  It complements anti-JVMTI checks; neither signal family is
strong enough alone.

## Design

The module returns a simple signal struct rather than throwing or exiting.
Windows combines documented debugger probes (`IsDebuggerPresent` and
`CheckRemoteDebuggerPresent`) with three independent native surfaces:

- PEB `BeingDebugged` and debugger-related `NtGlobalFlag` heap bits;
- dynamically resolved `NtQueryInformationProcess` queries for debug port,
  debug object, and `ProcessDebugFlags`;
- x86/x64 debug-register inspection for enabled hardware breakpoints.

The mockable core also exposes byte-pattern scanning for software breakpoints
(`0xCC` and `0xCD 0x03`) plus pure NtGlobalFlag/debug-register predicates.
`DebugSignals` reports probe availability and unexpected native query failure
separately from suspicious findings, so policy can distinguish unsupported from
failed inspection.

## Boundaries

The module does not terminate the process. `src/detection/runtime_detection.*`
combines these signals with module/JVMTI facts and exposes an explicit
fail-closed policy decision.

## Validation

`mj2x_runtime_test` checks signal aggregation, software-breakpoint matching,
NtGlobalFlag/debug-register mapping, and calls the live platform probe without
assuming the developer machine is or is not under a debugger.

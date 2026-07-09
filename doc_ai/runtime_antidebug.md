# Runtime Anti-Debug Design

## Purpose

`runtime/src/antidebug/` collects platform debugger signals for R3 fail-closed
policy decisions.  It complements anti-JVMTI checks; neither signal family is
strong enough alone.

## Design

The module returns a simple signal struct rather than throwing or exiting.
Windows currently uses documented debugger probes (`IsDebuggerPresent` and
`CheckRemoteDebuggerPresent`).  The mockable core also exposes byte-pattern
scanning for software breakpoints (`0xCC` and `0xCD 0x03`) so future module or
code-section probes can be tested without attaching a real native debugger.
Unsupported platforms return no signal until a platform-specific implementation
is added.

## Boundaries

The module does not decide how a protected sample reacts.  Generated code,
runtime loader code, or future policy code should combine these signals with
resource HMAC and anti-JVMTI results.

## Validation

`mj2x_runtime_test` checks signal aggregation logic, software-breakpoint byte
matching, and calls the platform probe without assuming the developer machine is
or is not under a debugger.

# StackCExceptionMonitorSample Trace Surface

Sample: `protected_samples.stackc.StackCExceptionMonitorSample`

Backend: `stackc`

Target issue: https://github.com/luiox/morpher/issues/462

Purpose: extend mjdbg observation reports to StackC samples covering the Java
exception edge and monitor edge in one fixture. `throwCatch` raises and catches
its own exception; `synchronizedAdd` enters/exits a `synchronized` block through
the generated StackC C JNI `MonitorEnter`/`MonitorExit` calls; `throwThroughNative`
raises without a handler so mjdbg can observe the uncaught propagation surface.
This complements the StackC JNI data-access fixture, which focuses on value
materialization rather than control and locking edges.

Expected surfaces:

Entry methods (consumed by the protected-sample driver via `--entry-method`):

- `throwCatch=(I)I`
- `synchronizedAdd=(I)I`
- `throwThroughNative=(I)I`

- Java method trace for `throwCatch`, `synchronizedAdd`, and
  `throwThroughNative`.
- Native/JNI entry wrapper trace for the generated StackC native methods.
- JNI monitor surface: `MonitorEnter` and `MonitorExit` around the
  `synchronized` block body.
- JNI exception surface for the throw/catch decision: `ThrowNew` (or
  `Throw`), `ExceptionCheck`, `ExceptionOccurred`, `ExceptionClear`.
- ATHROW propagation surface: the matched `throwCatch` catch path must clear the
  pending exception; `throwThroughNative` must propagate it uncaught back to
  Java.
- Negative surface expectation: `synchronizedAdd` should not report an
  exception edge (no raise path), and the monitor exit must not be skipped on
  the normal return.

Report gate:

- Generate the StackC protected class and DLL from the committed Java source.
- Run mjdbg Java/native trace against the generated sample, covering the
  matched-catch (`throwCatch`, `x > 0`) and the uncaught-raise
  (`throwThroughNative`, `x == 0`) inputs.
- Build `report observation --sample StackCExceptionMonitorSample` from daemon
  trace records.
- The report must include expected surfaces, observed surfaces, missing
  surfaces, analyzer findings, and hardening suggestions that can feed the
  mj2x StackC exception/monitor hardening backlog.

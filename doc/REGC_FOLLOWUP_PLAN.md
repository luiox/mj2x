# RegC Follow-up Plan

## Purpose

This document replaces the stale "start SSA/RegIR" task packets for the current
`dev.3` state.  SSA, RegIR, RegCBackend, helper-backed execution, try/catch
dispatch, integer traps, narrow primitive helpers, the CLI entry, and the first
R3 runtime core have already landed.

The remaining work should now move in small, testable phases that improve the
RegC input surface first, then move helper execution behind the R3 runtime
boundary, then hand stronger samples to mjdbg.

## Phase 0: State Cleanup

Status: current branch.

Goals:

- remove doc drift that still describes R1/R2/R3 as not started
- update public RegC comments to match the helper-backed emitter
- keep old CBackend notes clearly scoped to CBackend only

Validation:

- documentation review
- `xmake build mj2x-core`
- targeted RegC tests if any source comments touch nearby implementation

## Phase 1: SSA/RegC Readiness Hardening

Status: next implementation phase.

Goals:

- support javac numeric narrowing shapes around `i2b`, `i2s`, and `i2c`
- support boolean ternary StackMap shapes that currently trigger fail-closed
  stack underflow in selected methods
- support null field/call/array protected regions where the exception handler
  returns a value
- keep `invokedynamic` fail-closed until a real expansion strategy is chosen

Validation:

- add Java source samples under mj2x test resources or E2E harness source
  strings
- extend `SSABuilderJavaSamples` for the new shapes
- extend `RegCBackendE2E` so each shape compiles to DLL and matches Java output
- keep `RegCTest.*:RegCBackendE2E.*` green

## Phase 2: R3 Runtime ABI Integration

Status: design-to-code after Phase 1.

Goals:

- identify which inline `_mj2x_regc_*` helpers move behind runtime symbols first
- define the runtime context passed from JNI wrappers to helper code
- connect string-pool decode and anti-JVMTI/anti-debug probes to generated
  RegC samples without claiming protection strength before mjdbg feedback

Validation:

- runtime unit tests for context and helper dispatch
- RegCBackend E2E for at least one method using runtime-backed helpers
- generated source still passes the no-stack-anchor gate

## Phase 3: mjdbg Sample Hand-off

Status: after runtime-backed helpers exist.

Goals:

- generate paired CBackend and RegC/R3 protected samples
- document expected trace-surface differences for mjdbg
- collect c2j/mjdbg feedback before calling any runtime feature hardened

Validation:

- sample generator tests
- manual or automated mjdbg report attached to the follow-up PR or issue

## Phase 4: L4 Transform Passes

Status: intentionally after input-surface and runtime-boundary stabilization.

Goals:

- add `src/mj2x/mir/passes/` with deterministic pass planning
- implement constant propagation, DCE, MBA, and control-flow flattening slices
- make pass effects visible in RegC output without changing Java behavior

Validation:

- per-pass unit tests
- RegC E2E before/after transformed methods
- mjdbg/c2j degradation report for transformed samples

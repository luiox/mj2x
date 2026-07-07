# RegC Current Status

> 2026-07-06 status note after the SSA, RegIR, RegC MVP, helper ABI, CBackend
> Wave 0-7, RegC exception dispatch, integer traps, and R3 runtime core landed
> on `dev.3`.

## Purpose

This document tracks the near-term RegC work that is still active.  Historical
"start with SSA/RegIR" planning has been retired because those layers now exist
and are covered by unit and E2E tests.

The actionable phase plan lives in `regc_followup_plan.md`.

RegC remains the stackless native backend line:

```text
L1 stack MIR -> SSA -> RegIR -> RegC helper C -> JNI wrapper
```

CBackend remains the broad compatibility backend and mjdbg baseline.  RegC is
the protection-oriented path that removes operand-stack anchors from generated
C.

## Landed Baseline

- SSA and RegIR lower selected L1 MIR into register operands with structural
  assertions that no JVM stack slot survives the L3 boundary.
- RegC emits virtual-register locals and helper calls without `cstack[]`,
  `union jvalue`, `sp`, global `cclasses[]`, or "Cannot invoke" throw strings.
- RegCBackend emits JNI wrappers and only marks Java methods native when the
  SSA -> RegIR -> RegC path succeeds.
- Runtime-helper MVP covers object allocation, fields, arrays, casts,
  multi-arrays, `instanceof`, monitors, descriptor-driven Java calls,
  uncaught `ATHROW` propagation, try/catch dispatch, Java integer div/rem
  traps, and narrow primitive field/array helpers.
- Real native E2E covers pure computation, loops, arrays, fields, reference
  checks, constructors, virtual calls, native-to-native calls, multi-arrays, and
  uncaught/caught throws when the local JDK/MSVC toolchain is available.
- The CLI exposes RegC through `mj2x-cli --emit-c <file> --backend regc`.
- The R3 runtime core has standalone crypto, kernel, anti-JVMTI, anti-debug, and
  string-pool modules with unit tests and design docs.

## Active Gaps

- SSA readiness still needs broader javac-shape coverage before RegC can claim
  broad Java method coverage.  Known shapes include numeric narrowing
  (`i2b`/`i2s`/`i2c`) around stores, boolean ternary StackMap layouts, null
  field/call/array handlers, and non-expanded `invokedynamic`.
- The emitted helper layer is now mixed: integer div/rem and barrier call sites
  use the first R3 ABI slice, while object/field/array/call/exception helpers
  remain inline execution bridges.
- R4 transform passes (`src/mj2x/mir/passes/`) have not started yet.  Constant
  propagation, DCE, MBA, and control-flow flattening are still design-to-code
  work.
- R3 runtime probes still need mjdbg/c2j feedback before mj2x can claim improved
  protection strength.

## Next Sequence

1. Refresh docs and public comments so the current `dev.3` state is not
   confused with retired Wave R1/R2 task packets.
2. Harden SSA/RegC readiness against common javac bytecode shapes, starting
   with numeric narrowing and null-handler cases that currently fail closed.
3. Continue moving selected inline RegC helpers onto the R3 runtime ABI after
   the initial div/rem/barrier slice is validated.
4. Hand new RegC/runtime samples to mjdbg for trace comparison against CBackend.
5. Start the L4 pass framework after the RegC input surface is stable enough to
   avoid hardening a moving target.

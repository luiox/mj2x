# Vmc4JniSurfaceSample Trace Surface

Sample: `protected_samples.vmc4.Vmc4JniSurfaceSample`

Backend: `vmc4`

Purpose: JNI-surface companion to `Vmc4IsaSurfaceSample` (which covers the
pure-computation ISA only). Exercises the per-callsite helper surface that
capabilities.md §4 documents — static/instance field access, array
load/store/length, allocation, interface dispatch, and in-method throw/catch —
so mjdbg observation reports and the ISA-recovery cross-check can validate the
`mj2x_vm_call_table` dispatch path in addition to the dispatcher loop.

Entry methods: `scale` / `sumArray` / `fillSum` / `throwCatch` / `greet` /
`dispatch` / `main` (main stays bytecode; the rest virtualize).

Expected surfaces:

- Java method trace for the public entry methods above.
- VM execution side effects per virtualized method (`mj2x_vm_exec_<tag>`).
- JNI helper activity: field get (factor/counter), array load/store/length,
  allocation (`fillSum`), interface call dispatch (`dispatch`), exception
  region matching (`throwCatch`).
- RegisterNatives binding via `mj2x/cloader/Loader` in `<clinit>`.

Negative surface expectations:

- No reflection / dynamic class loading / custom-bootstrap invokedynamic in
  the virtualized bodies (fail-closed boundary, same as RegC).
- `main` and the nested `Adder`/`SIMPLE` holder classes stay bytecode (single
  `-i` class processing).

Cross-check criterion: native run stdout must match the plain-JVM baseline
byte-for-byte (`scale=26`, `sumArray=6`, `fillSum=33`, `throwCatch(neg)=-1`,
`throwCatch(pos)=14`, `greet=hi vmc4!`, `dispatch=7`).

# mj2x MVP Release Scope

> 2026-07-06 freeze decision: this document is the authority for the mj2x 0.1
> internal MVP release scope.  It supersedes the historical `mvp_plan.md`,
> which described the first single-class StackC prototype.

## Release Goal

The MVP proves that mj2x has a usable native-protection pipeline, not that the
protection is already hardened against mjdbg/c2j.  The release is acceptable
when a developer can feed ordinary `.class` or `.jar` inputs through the CLI,
choose StackC or RegC, inspect the emitted C, and validate the result with the
documented automated tests and protected samples.

## Frozen Functional Scope

### Input and Output

- Single `.class` input to rewritten `.class` plus emitted C source.
- `.jar` input to rewritten `.jar` plus emitted C source.
- JAR output preserves input entry order and non-class resource payloads.
- JAR class entries are rewritten from the mutable compiler index after native
  method injection.
- CLI supports both backend modes:
  - `--backend stackc`
  - `--backend regc`
- CLI supports annotation-scoped method selection with
  `--native-annotation <desc>`.
- CLI can compile emitted C with `--compile` when a supported local native
  toolchain and JDK headers are available.
- CLI can generate a standalone CMake native project with
  `--generate-cmake <dir>`.

### StackC

StackC is frozen as the broad correctness backend:

- L1 stack-MIR to JNI C.
- Arithmetic, conversions, control flow, switches, calls, objects, fields,
  arrays, exceptions, and monitor paths already covered by StackC E2E.
- StackC protected samples are part of the MVP handoff to mjdbg.

StackC is not the protection-strength claim for this MVP.  Its stack-shaped
`union jvalue` / `sp` output remains an accepted limitation.

### RegC

RegC is frozen as the protection-oriented MVP backend:

- L1 stack-MIR -> SSA -> RegIR -> RegC.
- Generated C must keep the no-stack-anchor structural assertion: no `cstack`,
  `stack[]`, `union jvalue`, or `sp--` pattern in RegC output.
- Current supported shapes include arithmetic, branches, loops, switches,
  fields, objects, calls, arrays, narrow primitives, throw, try/catch dispatch,
  null-trap handlers, and integer div/rem traps.
- R3 runtime ABI integration is frozen to the current div/rem/barrier slice.
- RegC protected samples are part of the MVP handoff to mjdbg.

### R3 Runtime

The MVP includes the current R3 runtime core and ABI foundation:

- crypto/kernel/stringpool/antidebug/antijvmti unit-testable modules
- RegC runtime ABI for div/rem trap helpers and barrier hook
- initial intrinsic helpers for Math, Integer.parseInt, boxed primitive
  value/unbox, and java.util.Random narrow non-escaping paths
- runtime protected sample surface for mjdbg

The MVP does not claim that anti-debug or anti-JVMTI behavior has already been
validated against live mjdbg/c2j agents.

## In-Scope Hardening Before Release

The following work is allowed after freeze because it improves reliability
inside the frozen scope without adding new backend features:

- JAR fixture tests with committed Java sources, manifest, multiple classes, and
  non-class resources.
- Signed-JAR metadata handling: fail closed for `META-INF/*.SF`, `*.RSA`,
  `*.DSA`, and `*.EC` before producing misleading output.
- CLI negative-path tests: bad args, unknown backend, missing input, unsupported
  backend selection, unreadable input, empty JAR, and no-class JAR.
- Unsupported bytecode boundary tests: JSR/RET and unsupported RegC lowering
  must fail closed or leave the method unmodified rather than silently
  generating wrong native code. StackC `invokedynamic` now has a runtime
  MethodHandle bridge and is covered by E2E tests.
- Multi-release JAR tests: `META-INF/versions/*.class` entries stay passthrough
  for the MVP and must be documented.
- Documentation cleanup where old docs contradict this frozen scope.
- Test flake fixes and deterministic test-resource/output cleanup.

## Explicitly Out of Scope

The following items are postponed until after the MVP release:

- L4 transform pass system: MBA, DCE, control-flow flattening, opaque
  predicates, and pass schema/planner.
- VMC4 native VM backend and VM runtime.
- Product-grade native packaging beyond the local `--compile` helper and
  generated CMake project.
- Signed-JAR re-signing or signature-stripping policy.
- RegC `invokedynamic`, lambda, method-handle, and string-concat lowering.
- JNI ID cache optimization.
- StackC JVM-accurate integer div/rem trap semantics.
- Moving all RegC object/field/array/call helpers behind the R3 runtime ABI.
- Claims that mjdbg/c2j recovery is harder before red-team feedback confirms it.
- JSON/config-driven method selection beyond the descriptor-based
  `--native-annotation` MVP path.

## Release Gates

An MVP release candidate must pass:

```powershell
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
git diff --check HEAD
```

The PR must also link the backend support matrix and verification index:

- `stackc_backend_readme.md`
- `regc_backend_readme.md`
- `../doc_ai/mvp_backend_verification.md`

## Current Residual Risks

- JAR output is an MVP writer: stored ZIP entries, preserved resources, class
  replacement, multi-release class passthrough, and signed metadata fail-closed,
  but no signature-stripping or re-signing policy.
- RegC is a protection-oriented preview, not full bytecode coverage.
- StackC is broad and executable but intentionally stack-shaped.
- Runtime anti-debug and anti-JVMTI modules have unit coverage but still need
  mjdbg/c2j live feedback before stronger claims.

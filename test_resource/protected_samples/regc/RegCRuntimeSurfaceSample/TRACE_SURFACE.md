# RegCRuntimeSurfaceSample Trace Surface

Sample: `protected_samples.regc.RegCRuntimeSurfaceSample`

Backend: `regc`

Target issue: https://github.com/luiox/morpher/issues/462

Purpose: extend mjdbg observation reports to RegC samples whose Java semantics
have moved behind the C runtime ABI rather than the intrinsic/helper layer.
`divOrCatch` and `remOrCatch` exercise integer div/rem plus an
`ArithmeticException` catch handler; `longDiv` exercises the i64 CRT ABI path.
This contrasts `RegCIntrinsicSurfaceSample`, which keeps arithmetic inside the
intrinsic layer and out of the JVM boundary.

Expected surfaces:

Entry methods (consumed by the protected-sample driver via `--entry-method`):

- `divOrCatch=(II)I`
- `remOrCatch=(II)I`
- `longDiv=(JJ)J`

- Java method trace for `divOrCatch`, `remOrCatch` and `longDiv`.
- Native/JNI entry wrapper trace for the generated RegC native methods.
- C runtime ABI div/rem surface: `mj2x_regc_div_i32`, `mj2x_regc_rem_i32`
  (and `mj2x_regc_div_i64` for `longDiv`).
- Exception dispatch surface for the catch handlers:
  `_mj2x_regc_exception_check`, `_mj2x_regc_exception_occurred`,
  `_mj2x_regc_exception_clear`, `_mj2x_regc_exception_matches`.
- `ArithmeticException` raise path through the generated adapter callback when
  the divisor is zero; the catch handler must suppress rethrow.
- Negative surface expectation: the div/rem/div_i64 call sites should avoid
  `_mj2x_regc_call_*` JNI dispatch (arithmetic is behind the CRT ABI, not the
  JVM boundary).

Report gate:

- Generate the RegC protected class and DLL from the committed Java source.
- Run mjdbg Java/native trace against the generated sample, including the
  zero-divisor input that drives the `ArithmeticException` catch path.
- Build `report observation --sample RegCRuntimeSurfaceSample` from daemon
  trace records.
- The report must include expected surfaces, observed surfaces, missing
  surfaces, analyzer findings, and hardening suggestions that can feed the
  mj2x RegC runtime ABI hardening backlog.

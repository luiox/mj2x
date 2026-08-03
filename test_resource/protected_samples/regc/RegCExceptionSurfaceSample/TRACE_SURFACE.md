# RegCExceptionSurfaceSample Trace Surface

Sample: `protected_samples.regc.RegCExceptionSurfaceSample`

Backend: `regc`

Target issue: https://github.com/luiox/morpher/issues/462

Purpose: extend mjdbg observation reports to RegC exception-table dispatch and
rethrow surfaces. `catchOwn` throws and catches its own exception;
`missCatch` throws `IllegalStateException` into a handler that does not match,
exercising the rethrow path; `fail` / `catchCall` move the throw across a
generated C call boundary so mjdbg can observe cross-frame exception
propagation. This isolates the exception-dispatch surface from the div/rem
arithmetic path covered by `RegCRuntimeSurfaceSample`.

Expected surfaces:

- Java method trace for `catchOwn`, `missCatch`, `fail`, and `catchCall`.
- Native/JNI entry wrapper trace for the generated RegC native methods.
- ATHROW propagation surface through `_mj2x_regc_throw` (including the unmatched
  `missCatch` rethrow and the `fail` cross-frame throw).
- Exception-table dispatch surface: `_mj2x_regc_exception_check`,
  `_mj2x_regc_exception_occurred`, `_mj2x_regc_exception_clear`, and
  `_mj2x_regc_exception_matches`.
- JNI exception surface used by the catch decision:
  `ExceptionCheck`, `ExceptionOccurred`, `ExceptionClear`.
- Negative surface expectation: matched-exception call sites should avoid
  continued native control flow after a pending Java exception (handler jump
  must follow `_mj2x_regc_exception_matches`).

Report gate:

- Generate the RegC protected class and DLL from the committed Java source.
- Run mjdbg Java/native trace against the generated sample, covering the
  matched-catch (`catchOwn`, `catchCall`) and unmatched-rethrow (`missCatch`)
  inputs.
- Build `report observation --sample RegCExceptionSurfaceSample` from daemon
  trace records.
- The report must include expected surfaces, observed surfaces, missing
  surfaces, analyzer findings, and hardening suggestions that can feed the
  mj2x RegC exception-dispatch hardening backlog.

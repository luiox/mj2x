# RegC Samples for mjdbg

These Java sources are stable inputs for generating RegC helper-style C samples.

Generated artifacts are not checked in.  The generator test writes them under:

```text
test/mj2x/protected_samples/regc/<SampleName>/
```

Each generated sample directory also includes `REGC_METRICS.md`, a structural
benchmark note with generated C byte size, `_mj2x_regc_call_` symbol
occurrences, and TU cache declaration counts.  This replaces the old dependency
on an external `test/java_obfuscator_test/TEST.jar` for intrinsic/cache surface
tracking.

Run:

```powershell
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=RegCTraceSamples.GenerateMjdbgFixtures
```

The generated C includes RegC JNI/native glue plus the RegIR-to-RegC helper
surface used to compare against CBackend's stack-machine C output.  mjdbg should
expect register locals such as `r0`/`r1`, labels such as `block_b0`, helper calls
such as `_mj2x_regc_alloc_object`, runtime ABI calls such as
`mj2x_regc_div_i32`, and exception-dispatch helpers such as
`_mj2x_regc_exception_matches` instead of `stack[]` / `sp` patterns.

## Intrinsic surface

`RegCIntrinsicSurfaceSample` exercises JDK calls RegC lowers to native
intrinsics.  For these call sites the generated C must NOT emit
`_mj2x_regc_call_*` JNI dispatch; instead mjdbg should observe:

- `java.lang.Math.*` -> `mj2x_math_*` runtime symbols (abs/min/max/sin/cos/
  sqrt/pow/floor/ceil/round/random), resolved against `mj2x-runtime-core`
- `Integer.parseInt(String)` -> `_mj2x_intrinsic_parse_int` (the only JNI
  touchpoint of the parseInt path is `GetStringUTFChars` inside that helper)
- String LDC -> `_mj2x_intrinsic_string_literal` (calls `NewStringUTF`)
- Non-escaping primitive wrappers -> `mj2x_int_*` / `mj2x_long_*` shadow
  structs.  These call sites should not emit `_mj2x_regc_call_ref` for
  `valueOf` or `_mj2x_regc_call_i32/i64` for `xxxValue`.
- Non-escaping `java.util.Random` -> `mj2x_random native_r*` plus
  `mj2x_random_init` / `mj2x_random_next_*` calls.  These call sites should not
  emit `_mj2x_regc_alloc_object("java/util/Random")` or
  `_mj2x_regc_call_*("java/util/Random", ...)`.

Trace surface contrast vs. CBackend stack style: intrinsic call sites keep the
JVM boundary out of arithmetic, parsing, wrapper unboxing, and Random stepping;
only the entry JNI wrapper and the String intrinsic helper touch `JNIEnv`.
Escaping wrapper/Random object cases are not fixtures yet: the current RegC
slice fails closed with `native object escaped through ...` instead of silently
materializing an imprecise jobject.

## Benchmark fixture

`RegCIntrinsicBenchmarkSample` is a small committed fixture for repeated
intrinsic/cache measurements.  It intentionally stresses:

- String read hot paths (`length`, `charAt`, `hashCode`, `isEmpty`,
  `startsWith`, `endsWith`, `indexOf`, `contains`);
- `StringBuilder.append(char[])` plus primitive append chaining;
- wrapper parse/compare, including floating compare edge helpers;
- multi-array allocation through cached child descriptors.

# RegC Samples for mjdbg

These Java sources are stable inputs for generating RegC helper-style C samples.

Generated artifacts are not checked in.  The generator test writes them under:

```text
test/mj2x/protected_samples/regc/<SampleName>/
```

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

Trace surface contrast vs. CBackend stack style: intrinsic call sites keep the
JVM boundary out of arithmetic and parsing entirely; only the entry JNI wrapper
and the String intrinsic helper touch `JNIEnv`.

# RegCIntrinsicSurfaceSample Trace Surface

Sample: `protected_samples.regc.RegCIntrinsicSurfaceSample`

Backend: `regc`

Purpose: first RegC protected sample for mjdbg observation reports. It contrasts
StackC's JNI-heavy helper surface with RegC intrinsic/runtime calls.

Expected surfaces:

- Java method trace for the public static entry methods.
- Native/JNI entry wrapper trace for the generated RegC native methods.
- JNI string access around `Integer.parseInt(String)` and string literal helpers:
  `GetStringUTFChars`, `ReleaseStringUTFChars`, and `NewStringUTF`.
- RegC runtime/intrinsic helper surface: `mj2x_math_*`,
  `_mj2x_intrinsic_parse_int`, `mj2x_int_*`, `mj2x_long_*`,
  and `mj2x_random_*`.
- Negative surface expectation: matched intrinsic call sites should avoid stable
  `_mj2x_regc_call_*` JNI dispatch anchors for Math, wrapper unbox, and Random
  stepping.

Report gate:

- Generate the RegC protected class and DLL from the committed Java source.
- Run mjdbg Java/native trace against the generated sample.
- Build `report observation --sample RegCIntrinsicSurfaceSample` from daemon
  trace records.
- The report must include expected surfaces, observed surfaces, missing
  surfaces, analyzer findings, and hardening suggestions that can feed mj2x
  RegC hardening backlog.

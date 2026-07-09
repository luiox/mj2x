# StackC Protected Samples for mjdbg

These Java sources are stable mj2x input fixtures for generating StackC
protected classes and C/JNI sources that mjdbg can attach to, trace, and analyze.

Generated artifacts are not checked in.  The generator test writes them under:

```text
test/mj2x/protected_samples/stackc/<SampleName>/
```

Run:

```powershell
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=StackCBackendProtectedSamples.GenerateMjdbgFixtures
```

The output directory contains the original class, protected class, generated C
file, copied Java source, and a per-sample trace note.

## Expected mjdbg Trace Surface

`StackCTinySample`

- Java static methods become native methods after mj2x processing.
- mjdbg should still observe Java method entry/exit for the public static entry
  methods.
- Native/JNI tracing should see arithmetic/control-flow work inside the
  generated C body rather than Java bytecode stepping through the original
  method body.

`StackCObjectArraySample`

- Exercises object allocation, constructor calls, instance/static fields,
  primitive arrays, `instanceof`, `checkcast`, and String LDC.
- mjdbg should expect JNI calls such as class/method/field lookup, object/array
  allocation, field get/set, array region access, `IsInstanceOf`, and
  `NewStringUTF`.
- Source-side gate note: `StackCObjectArraySample/TRACE_SURFACE.md`.

`StackCExceptionMonitorSample`

- Exercises exception paths and monitor enter/exit.
- mjdbg should expect native code to create/propagate Java exceptions and call
  monitor JNI APIs for synchronized regions.

These samples are correctness and trace-surface fixtures, not strength claims.
They should be used by mjdbg to build first trace capture, grouping, and
weakness-report development before stronger RegC/runtime protections land.

# LLVM IR Samples for mjdbg

These Java sources are stable inputs for generating LLVM textual-IR samples.

Generated artifacts are not checked in.  The generator test writes them under:

```text
test/mj2x/protected_samples/llvm/<SampleName>/
```

Each sample directory contains the transformed class plus `native.ll`
(`--emit-llvm`), and the samples test additionally compiles the IR with clang
and runs the native library against a JVM driver.

Run:

```powershell
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=LlvmSamples.GenerateMjdbgFixtures
```

mjdbg should expect LLVM surface markers:

- `define internal i32 @mj2x_llvm_Java_<class>_<method>__<args>` helper plus
  `define dllexport ... @Java_<class>_<method>__<args>` JNI wrapper;
- `mj2x_llvm_call_*` JNI dispatch helpers for object/static/interface calls;
- `mj2x_llvm_class_const` / `mj2x_llvm_string_const` for ldc constants
  (class literals including primitive TYPE forms);
- `mj2x_llvm_array_length` / `mj2x_llvm_array_load_*` / `mj2x_llvm_array_store_*`;
- `mj2x_llvm_throw_*` / exception-region dispatch for try-catch;
- `mj2x_llvm_monitor_enter` / `mj2x_llvm_monitor_exit`;
- invokedynamic runtime resolution helpers `mj2x_llvm_indy_bsm_<n>` with the
  global cache `@_mj2x_llvm_indy_cache_<n>` when custom-bootstrap indy sites
  remain after pipeline expansion (StringConcatFactory/LambdaMetafactory
  sites are expanded before LLVM lowering).

## Surfaces

- `LlvmTinySample`: arithmetic, loops, branches, register slots.
- `LlvmIndySurfaceSample`: string concat (expanded), class literals,
  System.arraycopy as a plain JNI static call (the arraycopy intrinsic is
  RegC-only).
- `LlvmExceptionMonitorSample`: exception dispatch and monitor helpers.

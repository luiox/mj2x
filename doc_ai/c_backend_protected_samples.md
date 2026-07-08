# CBackend Protected Samples

This note describes the CBackend sample fixtures that mjdbg can use for trace
capture and red-team development.

## Purpose

CBackend protected samples are small Java programs that exercise known native
lowering surfaces. mj2x keeps the Java source files in `test_resource/` and
generates protected `.class` files plus C/JNI sources under `test/` during a
test run.

The samples are intended for repeatable mjdbg development:

- start from a tiny arithmetic/control-flow class
- expand to object, field, array, and type-checking JNI surfaces
- expand again to exception propagation and monitor operations

They are correctness and trace-surface fixtures. They are not protection
strength claims.

## Source Layout

The authoritative sample sources live in:

```text
mj2x/test_resource/protected_samples/cbackend/
```

Each committed Java file should stay small enough for a debugger trace to be
readable by a human. When a new runtime or backend capability needs mjdbg
coverage, add a focused sample rather than growing an existing one into a broad
integration program.

## Generated Output

The generator test writes artifacts to:

```text
test/mj2x/protected_samples/cbackend/<SampleName>/
```

Each output directory contains:

- copied Java source
- original compiled class under its package path
- CBackend-protected class
- emitted C/JNI source
- `TRACE_SURFACE.md` describing what mjdbg should inspect

Generated artifacts are intentionally not checked in. Re-running the test is
the reproducible way to refresh them.

## Generator Test

The test entry point is:

```text
mj2x/src/unittest/backend/c_backend_protected_samples_test.cpp
```

It probes for a local JDK and built `mj2x-cli`, compiles each Java source with
debug metadata, runs CBackend through `mj2x-cli`, and verifies that the emitted
C source exposes `JNI_OnLoad` and `RegisterNatives` bindings for the protected
package. CBackend method bodies are emitted as `static __mj2x_c_*` functions,
so mjdbg should not expect per-method `Java_*` exports.

Run:

```powershell
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=CBackendProtectedSamples.GenerateMjdbgFixtures
```

If the toolchain is unavailable, the test skips instead of failing CI.

## mjdbg Handoff

mjdbg should treat the generated directory as a fixture bundle. The expected
workflow is:

1. Run the generator test on the mj2x side.
2. Launch or attach to a JVM that loads the generated protected class.
3. Capture Java method entry/exit around the public entry methods.
4. Capture `JNI_OnLoad` / `RegisterNatives`, then native/JNI activity inside the emitted CBackend implementation.
5. Record observations as weakness reports for mj2x hardening.

When the fixture format or trace expectations change, update this document,
`mj2x/test_strategy.md`, and the per-sample README in the same change.

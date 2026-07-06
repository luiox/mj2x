# RegC Trace Samples

## Purpose

RegC trace samples give mjdbg a register-style C surface to compare with the
existing CBackend protected samples.  They are not final protected native
artifacts yet because RegC does not have JNI glue or runtime helpers wired into
the pipeline.  They are deterministic fixture bundles for understanding the
shape RegC will expose.

## Source Layout

Authoritative Java sources live in:

```text
mj2x/test_resource/protected_samples/regc/
```

The samples are split into two surfaces:

- `RegCTinySample`: arithmetic, loops, switches, and edge-copy control flow
- `RegCHelperSurfaceSample`: object allocation, fields, arrays, calls, and
  reference checks through `_mj2x_regc_*` helper calls
- `RegCExceptionSurfaceSample`: try/catch dispatch, handler exception
  registers, unmatched rethrow, and callee-throw/caller-catch paths

## Generated Output

The generator test writes artifacts to:

```text
test/mj2x/protected_samples/regc/<SampleName>/
```

Each sample gets one generated `.c` translation unit plus a protected class.
The output directory also contains a copied Java source file, compiled original
class, and `TRACE_SURFACE.md`.

Generated artifacts are intentionally not committed.

## Generator Test

Run:

```powershell
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=RegCTraceSamples.GenerateMjdbgFixtures
```

The test compiles Java with `javac`, then runs the normal RegC backend pipeline:

```text
ClassFile -> Pipeline -> L1 CodeConverter -> SSA -> RegIR -> RegCBackend
```

It checks that generated C has no `stack[]`, `union jvalue`, or `sp--` anchors
and that each sample contains its expected register/control-flow/helper or
exception-dispatch surface.

## mjdbg Handoff

mjdbg should use these samples to compare:

- CBackend: JNI native functions with explicit operand-stack storage
- RegC: helper-style C using virtual registers, labels, edge copies, and
  `_mj2x_regc_*` calls

This comparison should guide what trace grouping and weakness reports need to
look like once RegC gains JNI/native glue.

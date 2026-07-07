# RegC Native Backend

## Purpose

RegCBackend is the native facade for the stackless RegC line.  It connects the
shared mj2x backend interface to the newer SSA -> RegIR -> RegC pipeline so
RegC can start replacing the old operand-stack-shaped CBackend surface for
methods that fit the register path.

## Pipeline Position

The backend keeps the existing front of the pipeline:

```text
MethodNode -> CodeConverter L1 -> SSA -> RegIR -> RegC helper -> JNI wrapper
```

RegCBackend does not translate bytecode directly.  It receives L1 blocks from
the shared `Pipeline`, builds SSA and RegIR, asks `RegCEmitter` for one
stackless helper function, and appends a JNI-exported wrapper with normal JVM
native symbol naming.

## Output Shape

Each successful method emits two layers:

- a static `regc_*` helper using virtual-register locals such as `r0`
- a `JNIEXPORT Java_*` wrapper that casts JNI parameters into helper arguments

The helper remains the defensive trace surface.  The wrapper only adapts JVM
native entry arguments and return values; it must not reintroduce `stack[]`,
`union jvalue`, or `sp`-style operand-stack anchors.

## Failure Policy

Lowering and emission are fail-closed.  If SSA, RegIR, or RegC cannot handle a
method, RegCBackend returns `BackendResult::Kind::NONE` and does not mark the
Java method as native.  This avoids producing a class whose bytecode body was
removed without a working native implementation.

## Runtime Boundary

Pure computation and control flow can execute through the generated JNI wrapper;
`RegCBackendE2E.PureArithmeticLoopAndArrayRoundTrip` compiles the generated C into a
DLL and compares protected Java calls against the original Java behavior when
JDK and MSVC are available.

Object, field, array, monitor, and type-check operations lower to
`_mj2x_regc_*` helper calls that are emitted by RegCBackend.  The helper layer
is described in `regc_runtime_helpers.md`.  Method calls are wired through
descriptor-driven `jvalue` packing helpers.  Multi-arrays use recursive JNI
allocation and are covered by native E2E for primitive nested arrays.

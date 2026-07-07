# RegC MVP Design

## Purpose

RegC is the stackless C backend line for mj2x.  The MVP proves that RegIR can be
emitted as C locals and labels without rebuilding a JVM operand stack.

This stage is intentionally smaller than CBackend.  CBackend is the broad,
executable JNI reference path.  RegC starts as the defensive shape: virtual
registers in, stackless C out.

## Input

The MVP consumes `RegFunction` from `src/mj2x/mir/register_ir/`.  The expected
pipeline in tests is:

```text
MethodNode -> CodeConverter L1 -> SSAFunction -> RegFunction -> RegC source
```

RegC does not read raw bytecode and does not perform SSA or RegIR construction.
It assumes earlier layers have already removed operand-stack movement and
eliminated block arguments into edge copies.

## Output Shape

The core emitter emits ordinary C helper functions.  Method parameters map to
`r{id}` arguments and non-parameter virtual registers become C locals:

```text
static int32_t regc_Class_method__I_I(int32_t r0) {
    int32_t r1 = 0;
    goto block_b0;

block_b0:
    r1 = (int32_t)((uint32_t)r0 + (uint32_t)1);
    return r1;
}
```

The generated source must not contain stack-machine anchors such as `cstack[`,
`stack[`, `union jvalue`, `sp--`, or `Cannot invoke`.

`RegCBackend` is the native facade layered above this emitter.  It wraps each
successful helper in a JNI-exported `Java_*` function and marks the Java method
as native.  Its design is documented in `regc_native_backend.md`.

## Supported Surface

The core register surface supports pure computation and control flow:

- arithmetic and bitwise binary ops
- unary negation
- numeric conversions
- comparisons
- jumps, branches, switches, returns
- RegIR edge copies produced by block-argument elimination

Edge copies are emitted with temporary variables so cyclic phi transfers remain
well-defined.

The helper-backed surface keeps the same stackless C shape but emits calls to
generated `_mj2x_regc_*` helpers or R3 `mj2x_regc_*` runtime ABI helpers for
JVM operations:

- field access
- array access and allocation
- object allocation
- calls
- monitor and reference-check intrinsics
- throw terminators for uncaught exception propagation
- try/catch exception-table dispatch
- integer division/remainder traps through runtime-backed arithmetic helpers
- narrow primitive field/array access for boolean, byte, char, and short
- barriers

Most generated helpers are still a structured placeholder so RegC can cover the
instruction surface while native/JNI runtime policy is moved behind the R3
boundary in slices.  Integer div/rem and barrier call sites are the first
runtime-backed slice.

Try/catch support is modeled as method-level exception regions carried from L1
through SSA and RegIR.  After a may-throw helper call or `ATHROW` in a protected
block, generated C checks for a pending JNI exception, clears it temporarily,
tests handlers in exception-table order, materializes the handler's synthetic
exception register, and jumps to the handler block.  If no handler matches, the
exception is rethrown and native control returns immediately.

## Testing

`src/unittest/backend/regc_test.cpp` verifies:

- arithmetic RegIR emits register C
- loops emit labels, branches, and edge-copy temporaries
- switches emit direct C switch control flow
- fields, arrays, calls, allocation, reference checks, and monitors emit helper
  calls without reintroducing stack-machine anchors
- throw terminators emit `_mj2x_regc_throw` followed by an immediate C return
- try/catch methods emit `_mj2x_regc_exception_*` dispatch and handler jumps
- integer div/rem emits `mj2x_regc_div_*` / `mj2x_regc_rem_*` runtime ABI calls
- narrow primitive fields and arrays select descriptor/element-specific JNI
  helpers for boolean, byte, char, and short instead of reusing int helpers
- generated source passes the no-stack-anchor gate

RegCBackend tests verify JNI wrapper emission and native-method injection.  The
native E2E layer covers helper-backed arrays, fields, calls, multi-arrays, and
exception propagation/dispatch when the local JDK/MSVC toolchain is available.
It also covers Java integer division/remainder traps, including divide by zero
catching and the `MIN_VALUE / -1` overflow edge.
`NarrowPrimitiveRoundTrip` extends the DLL path to byte/short/char/boolean
fields plus byte/short/char arrays.

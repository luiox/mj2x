# RegC Runtime Helpers

## Purpose

RegC helper calls are the first runtime ABI between stackless generated C and
the JVM.  They let RegC keep virtual-register code shape while delegating Java
object, array, field, monitor, and reference-check operations to small JNI
helpers.

## Current Shape

`RegCBackend` emits helper definitions into the generated C translation unit.
Each JNI wrapper stores the current `JNIEnv*` in a translation-unit local before
calling the `regc_*` helper.  The generated helper body can then call functions
such as `_mj2x_regc_alloc_array`, `_mj2x_regc_load_i32_array`, and
`_mj2x_regc_instanceof` without receiving `JNIEnv*` as an explicit RegIR value.

This is a transitional bridge.  Integer div/rem and barrier call sites now use
the first R3 runtime ABI described in `regc_r3_runtime_abi.md`; object, field,
array, call, monitor, and exception-dispatch helpers remain generated inline
until those larger surfaces have stable runtime contracts.

## Implemented Surface

The MVP helper layer implements:

- object allocation through `AllocObject`
- primitive and object array allocation
- primitive and object array load/store; boolean/byte/char/short arrays use
  dedicated JNI region helpers instead of being collapsed onto `int[]`
- array length
- instance/static field load/store; the generated call passes an explicit
  static/instance flag so a null instance receiver throws
  `NullPointerException` instead of being mistaken for a static access
- `checkcast`, `instanceof`, and monitor enter/exit
- static, virtual, special, and constructor calls through descriptor-driven
  `jvalue` argument packing
- multi-dimensional arrays through recursive JNI allocation; dimensions supplied
  by `MULTIANEWARRAY` are allocated eagerly, while lower dimensions not supplied
  by the bytecode remain `null` like the JVM partial-allocation form
- uncaught `ATHROW` propagation through `_mj2x_regc_throw`, followed by an
  immediate generated C return so native control flow does not continue with a
  pending Java exception
- try/catch dispatch through `_mj2x_regc_exception_check`,
  `_mj2x_regc_exception_occurred`, `_mj2x_regc_exception_clear`, and
  `_mj2x_regc_exception_matches`; generated C rethrows unmatched exceptions
- Java integer division/remainder through R3 ABI calls
  `mj2x_regc_div_i32`, `mj2x_regc_div_i64`, `mj2x_regc_rem_i32`, and
  `mj2x_regc_rem_i64`; these helpers throw `ArithmeticException` for zero
  divisors through the generated adapter callback and preserve `MIN_VALUE / -1`
  and `MIN_VALUE % -1` JVM edge semantics
- runtime policy barriers through `mj2x_regc_barrier`, currently a no-op ABI
  hook reserved for later anti-debug/anti-JVMTI/key-state checks

## Testing

`RegCBackendE2E.PureArithmeticLoopAndArrayRoundTrip` covers pure register code,
loop control flow, and an `int[]` method through a real DLL round trip when JDK
and MSVC are available.  `RegCBackendE2E.FieldAndReferenceHelpersRoundTrip`
covers instance/static fields and `instanceof`.  Structural RegC tests continue
to assert that generated C has no
operand-stack anchors such as `stack[]`, `union jvalue`, or `sp`.
`RegCBackendE2E.CallHelpersRoundTrip` covers static native-to-native calls plus
object allocation, constructor invocation, and virtual calls.
`RegCBackendE2E.MultiArrayHelpersRoundTrip` covers `int[][][]` allocation,
element store/load, and nested array length through a real DLL round trip.
`RegCBackendE2E.ThrowHelperRoundTrip` covers a method that returns normally for
one input and propagates `IllegalStateException` for another.
`RegCBackendE2E.TryCatchDispatchRoundTrip` covers a local throw caught by a
matching handler, an unmatched catch that rethrows, and a native callee throw
caught by the caller's RegC dispatch.
`RegCBackendE2E.IntegerDivisionTrapRoundTrip` covers i32/i64 runtime-backed
division helpers, zero-divisor catching/propagation, and signed overflow edge
cases.
`RegCBackendE2E.NarrowPrimitiveRoundTrip` covers byte/short/char/boolean fields
and byte/short/char array helpers through a real DLL round trip.  Boolean array
load/store shares the JVM `baload`/`bastore` opcode shape with byte arrays, so
the runtime helper detects boolean arrays before falling back to byte regions.

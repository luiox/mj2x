# RegC R3 Runtime ABI

## Purpose

RegC is the register-style backend in the mj2x protection line.  Its generated
C should not keep growing large inline helper bodies, because that makes every
protected translation unit expose the same JNI-heavy template.  The R3 runtime
ABI is the boundary where selected helper semantics move into
`mj2x-runtime-core`.

This first ABI slice is intentionally small:

- a per-entry `mj2x_regc_context`
- JVM-compatible integer division/remainder helpers
- Java `ArithmeticException` routing through a generated adapter callback
- a runtime barrier hook reserved for later anti-debug/anti-JVMTI/key checks

Fields, arrays, object allocation, method calls, and exception-table dispatch
still use the existing generated helper layer.  They will migrate behind the
runtime ABI in later phases after this smaller path is stable.

## Context Ownership

Generated JNI wrappers initialize one `mj2x_regc_context` before calling the
stackless RegC function body.  The context stores the JVM environment as an
opaque pointer plus a callback for throwing Java exceptions.  This keeps
`mj2x-runtime-core` independent from JDK headers while still letting generated
JNI adapters materialize Java exceptions with the real `JNIEnv*`.

The runtime owns helper semantics, not local JNI adapter mechanics:

```text
JNI wrapper
  -> initialize mj2x_regc_context(env, throw-callback)
  -> call RegC register function
      -> call mj2x_regc_div_i32(context, lhs, rhs)
          -> runtime checks JVM trap rules
          -> callback throws ArithmeticException when needed
```

## ABI Boundary

The public authority is `runtime/src/regc/regc_runtime_abi.hpp`.  Public
contracts belong in that header's Doxygen comments; this document only explains
the module shape and migration direction.

Current generated RegC C emits ABI-compatible declarations in the translation
unit and links against `regc_runtime_abi.cpp` in native E2E tests.  A later
packaging step can replace those declarations with a distributed C-facing
header once runtime packaging is formalized.

## Trace Surface

Compared with the old inline helper body, mjdbg should now see runtime-backed
calls such as `mj2x_regc_div_i32` and `mj2x_regc_barrier` from register-style
RegC code.  This is not yet a protection-strength claim.  It is the first stable
call boundary that lets mjdbg compare:

- StackC stack style: `stack[]`, `sp`, JNI calls in each generated function
- RegC/R3 style: virtual registers, labels, and runtime ABI helper calls

Future phases should add protected samples that pair the generated RegC C file,
runtime object code, and an expected trace note for mjdbg.

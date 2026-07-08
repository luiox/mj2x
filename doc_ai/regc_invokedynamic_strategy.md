# RegC Invokedynamic Strategy

## Purpose

`invokedynamic` is intentionally not expanded in this RegC/R3 runtime ABI
slice.  The current safe behavior is fail-closed: the preprocessor detects indy
sites, `CodeConverter` preserves a `MIRInvokeDynamic` marker, and SSA rejects it
with a clear reason instead of silently emitting incorrect native code.

This document records the intended expansion order so the next implementation
does not blur three different cases: Java string concatenation, Java lambdas,
and obfuscator-specific bootstrap methods.

## Current Boundary

Current behavior:

- `InvokeDynamicPreprocessor` detects indy instructions and logs them.
- It does not rewrite bytecode yet.
- SSA rejects non-expanded indy with `invokedynamic not yet expanded`.
- CBackend has a separate runtime bridge that invokes the bootstrap method,
  reads `CallSite.getTarget()`, and executes `MethodHandle.invokeWithArguments`.
- RegC should not mark a method native when indy lowering fails.

This is the correct RegC boundary until expansion has deterministic tests.
CBackend's runtime bridge is a correctness path, not a RegC no-stack lowering.

## Expansion Order

1. String concat first.

   Support `java/lang/invoke/StringConcatFactory.makeConcatWithConstants` for
   javac-generated concatenation.  Expand it before SSA into ordinary
   `StringBuilder`-style bytecode or a small synthetic static method.  This is
   the lowest-risk useful shape because the recipe and constants are visible at
   compile time.

2. Stateless lambda factories second.

   Support `java/lang/invoke/LambdaMetafactory.metafactory` only when the target
   method handle and functional interface signature can be resolved locally.
   Capturing lambdas need explicit captured-argument storage and should not be
   folded into the first implementation.

3. Obfuscator bootstrap methods last.

   For custom bootstrap methods, do not try to execute arbitrary bootstrap code
   during mj2x compilation.  Prefer either a documented fail-closed diagnostic
   or a later protected runtime call-site helper designed with mjdbg feedback.

## Tests Required Before Enabling

Each supported indy shape needs:

- a committed Java source sample under `mj2x/test_resource/`
- an SSA/RegIR test showing the indy marker is gone before lowering
- a RegC native E2E comparison against the original Java behavior
- a negative test proving unsupported bootstrap shapes still fail closed

Until those gates exist, `invokedynamic` remains outside the RegC accepted
surface.

# mj2x Architecture

> Current architecture overview for mj2x. Historical redesign notes are archived
> under `doc/_archive/`.

---

## 1. Goal

mj2x moves selected Java method or class semantics into native code. It is not a
general Java AOT compiler. The protection goal is to reduce bytecode recovery
surface and JNI/native trace anchors, then validate that claim through mjdbg
reports.

The development loop is:

```text
Java method/class
  -> mj2x native output
  -> protected sample with expected trace surface
  -> mjdbg observation / weakness report
  -> backend/runtime/intrinsic hardening
```

---

## 2. Pipeline

```text
InputLoader
  -> CompilerIndex
  -> Preprocessors
  -> L1 MIR
  -> backend
  -> Postprocessors
  -> OutputWriter
```

Current backends:

| Backend | Purpose | Output style |
|---|---|---|
| StackC | Broad correctness baseline | stack-style JNI C using `union jvalue` and `sp` |
| RegC | Protection-oriented backend | register-style C without stack-machine anchors |

Planned analysis path:

```text
L1 MIR -> SSA -> RegIR -> L4 transforms -> RegC
```

SSA/RegIR/L4 are platform capabilities for stronger RegC output and future
whole-class sinking. StackC does not depend on SSA.

---

## 3. Module Layout

| Module | Role |
|---|---|
| `mj2x-cli/` | CLI, argument parsing, compiler/toolchain invocation |
| `mj2x-core/src/mj2x/core/` | pipeline, compiler index, shared orchestration |
| `mj2x-core/src/mj2x/io/` | `.class` / `.jar` input and output |
| `mj2x-core/src/mj2x/frontend/` | MIR, blocks, code conversion |
| `mj2x-core/src/mj2x/backend/stackc/` | StackC backend |
| `mj2x-core/src/mj2x/backend/regc/` | RegC backend and intrinsic matcher/registry |
| `mj2x-core/src/mj2x/codegen/` | C source helpers shared by emitters |
| `mj2x-core/src/mj2x/preprocessor/` | method marking and bytecode shape preprocessing |
| `mj2x-runtime/src/` | runtime ABI, crypto, antidebug, antijvmti, intrinsic helpers |
| `mj2x-annotations/` | user-facing `@mj2x.Native` annotation |
| `test_resource/protected_samples/` | samples for mjdbg attack/observation |

---

## 4. Backend Boundaries

### StackC

StackC is the broad-coverage backend. It is expected to preserve behavior across
many javac output shapes, including object/array/field/call/exception paths. It
is not the main anti-c2j protection backend because its emitted code still has
stack-machine anchors.

Use StackC for:

- correctness coverage,
- fallback samples,
- fixture generation,
- release confidence.

### RegC

RegC is the protection-oriented backend. Its output must not contain `cstack`,
`stack[]`, `union jvalue`, or `sp--` style anchors. Unsupported bytecode must
fail closed with a clear reason rather than generating incorrect native code.

Use RegC for:

- protected samples,
- intrinsic experiments,
- runtime ABI validation,
- mjdbg attack/defense feedback.

---

## 5. Runtime

The runtime is a native support library for RegC and protected samples.

Current/runtime domains:

- ABI helpers: div/rem/barrier and exception/error routing.
- Crypto/kernel/stringpool.
- Antidebug and antijvmti checks.
- JDK intrinsic helpers: wrappers, Math, Random, parseInt.

The runtime must remain testable as a standalone component. ABI-affecting
changes require deterministic unit tests and platform/toolchain notes.

---

## 6. Intrinsic Strategy

Intrinsic support is incremental:

1. non-escaping wrappers and numeric helpers,
2. Math / Random,
3. String / StringBuilder,
4. arrays, collections, time, regex, custom user intrinsic,
5. whole-class sinking when enough calls can stay native.

Escaping values, reflection, dynamic class loading, and unknown JDK calls must
fall back or fail closed.

---

## 7. Non-Goals

- Full Java AOT.
- Full JVM runtime or GC.
- Reflection/native semantics for arbitrary libraries.
- VMC4 as the default path.
- Protection claims without mjdbg observation.

---

## 8. Validation

Core validation:

```powershell
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
```

Protected sample work must also state the expected mjdbg trace surface and record
whether a weakness report was produced.

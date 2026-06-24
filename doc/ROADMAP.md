# mj2x ROADMAP

> Product-line roadmap for the mj2x blue-team AI.

---

## 1. Mission

mj2x owns native conversion and protection:

- bytecode -> C/native conversion
- MIR and backend architecture
- CBackend
- VM Backend / VMC4
- native runtime
- protected sample generation

mj2x is the blue-team half of the morpher attack/defense loop. Its outputs must
eventually be attacked by mjdbg.

---

## 2. Backend Lines

### CBackend

Purpose:

- practical native conversion path
- lower-risk correctness target
- baseline for JNI/native boundary tracing

Near-term:

- stabilize selected method subset
- improve JNI ID/cache handling
- produce reproducible native samples
- document unsupported bytecode cases

### VM Backend

Purpose:

- stronger protection through VMC4/native VM
- produce samples that require semantic recovery

V0 is correctness, not protection strength:

- MIR subset -> VMC4
- clear constant pool
- fixed opcode
- small vm_core
- one pure computation method executes correctly

Later:

- opcode alias
- HMAC/resource integrity
- CP encryption
- resident opcode masking
- keygen
- anti-debug/anti-dump gates

---

## 3. Near-Term Waves

### Wave B0: Sample Contract

Goal: define the first protected sample and how mjdbg should attack it.

- source Java method
- expected CBackend or VMC4 output
- expected trace surface
- success/failure criteria

### Wave B1: CBackend MVP

Goal: prove native conversion on a small method subset.

- compile generated C/native code
- run behavior comparison
- document unsupported constructs

### Wave B2: VMC4 V0

Goal: execute one method through native VM.

- VMC4 header/code format
- VM interpreter subset
- runtime entry path
- deterministic tests for format and execution

### Wave B3: First Hardening Feedback

Goal: respond to mjdbg findings.

- take one weakness report
- add one hardening change
- regenerate sample
- document whether mjdbg recovery got harder

---

## 4. Shared Contracts

| Contract | Other owner | Rule |
|---|---|---|
| protected sample format | mjdbg | sample must document expected trace surface |
| trace expectations | mjdbg | coordinate before hiding or randomizing signals |
| libjbytecode usage | nbase | direct use is OK; shared wrappers need coordination |
| runtime build options | build owner | do not silently add heavy default dependencies |

---

## 5. Validation

Baseline:

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
```

For native runtime changes, document:

- OS
- compiler/toolchain
- JDK if JNI is involved
- whether behavior was compared with original Java

---

## 6. Documentation Duties

mj2x AI maintains:

- `AGENTS.md`
- `doc/ROADMAP.md`
- `dev_plan.md`
- `doc/ARCHITECTURE.md`
- `doc/MVP_PLAN.md`
- `doc/NATIVE_VM_ROADMAP.md`
- `doc/TEST_STRATEGY.md`

Update root `doc/capability_map.md` only when mj2x creates a shared capability
that other lines should discover and reuse.

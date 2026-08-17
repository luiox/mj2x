# mj2x runtime

The runtime is the R3 native support layer for RegC and future protected
samples.

## Modules

- `src/crypto/`: SHA-256, HMAC-SHA256, constant-time comparison.
- `src/kernel/`: HKDF-SHA256, HMAC verification, volatile wipe.
- `src/antijvmti/`: JNI table checks, live module enumeration, agent/JDWP
  scanning, and JVMTI capability query.
- `src/antidebug/`: platform debugger, PEB/NtQuery, hardware/software breakpoint
  signal collection.
- `src/detection/`: policy-neutral live report aggregation and fail-closed
  decision.
- `src/stringpool/`: HKDF-derived string-pool decode.

Design notes live in `mj2x/doc_ai/capabilities.md` (runtime capability surface),
`mj2x/doc_ai/regc_safe_env.md` (JNI function-table recovery) and
`mj2x/doc/techniques/` (protection techniques).

## Build

```bash
xmake build mj2x-runtime-core
xmake build mj2x_runtime_test
xmake run mj2x_runtime_test
```

`jni_lib` remains the shared-library wrapper target for later JNI integration:

```bash
xmake build jni_lib
```

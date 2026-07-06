# mj2x runtime

The runtime is the R3 native support layer for RegC and future protected
samples.

## Modules

- `src/crypto/`: SHA-256, HMAC-SHA256, constant-time comparison.
- `src/kernel/`: HKDF-SHA256, HMAC verification, volatile wipe.
- `src/antijvmti/`: mockable JNI function-table tamper checks and agent option
  scanning.
- `src/antidebug/`: platform debugger signal collection.
- `src/stringpool/`: HKDF-derived string-pool decode.

Design notes live in `mj2x/doc/RUNTIME_*.md`.

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

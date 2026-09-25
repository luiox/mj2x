# mj2x-rt

`mj2x-rt` is the native-side runtime source pack copied into projects generated
by `mj2x-cli --generate-xmake`. Its public ABI is C99/JNI; source packs may add
C++ implementation files behind that ABI without changing generated C.

Source mounts (see `rt.resources`): `common/` cross-backend runtime sources,
`regc/` RegC backend ABI, `stackc/` StackC backend ABI, `vmc4/` VMC4 backend
ABI, `llvmir/` LLVM IR runtime helpers, and `templates/` generation-time
project templates (#894 keeps runtime code fixed and linked, never assembled
per TU by the emitters).

## Per-backend distribution (#891)

Generated projects receive only the mounts their backend consumes — the
single source of truth is `rtFoldersForBackend` in
`mj2x-cli/src/embed_rt.hpp`:

| Backend | Mounts distributed | Backend define |
|---|---|---|
| StackC | `stackc` | `MJ2X_RT_BACKEND_STACKC=1` |
| RegC   | `common`, `regc` | `MJ2X_RT_BACKEND_REGC=1` |
| VMC4   | `common`, `vmc4` | `MJ2X_RT_BACKEND_VMC4=1` |
| LLVM   | `common`, `regc`, `llvmir` | `MJ2X_RT_BACKEND_LLVM=1` |

`common` (rt_protect checkpoints) ships unconditionally for RegC/VMC4/LLVM so
`--runtime-protection` never changes the distribution set. Exactly one
`MJ2X_RT_BACKEND_*` define is injected into every build surface (xmake
template `@RT_BACKEND_DEFINES@` token, CMake `target_compile_definitions`,
LLVM inline scripts, and in-process `--compile` commands as `/D`/`-D`);
runtime sources and user intrinsics may gate on it.

It is intentionally separate from `mj2x-runtime` and has no `libca` dependency.
This layer is the sole distributable runtime entry for generated RegC code and
generated native build templates. Higher-level runtime features can be connected
later without making generated projects depend on the in-repo runtime layout.

The current ABI includes arithmetic and compare helpers, UTF-16 `String`
operations, and an opaque dynamically growing `StringBuilder` state with
explicit init, destroy, snapshot, and Java-object materialization boundaries.

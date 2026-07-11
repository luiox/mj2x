# mj2x-crt

`mj2x-crt` is the native-side runtime source pack copied into projects generated
by `mj2x-cli --generate-xmake`. Its public ABI is C99/JNI; source packs may add
C++ implementation files behind that ABI without changing generated C.

It is intentionally separate from `mj2x-runtime` and has no `libca` dependency.
This layer is the sole distributable runtime entry for generated RegC code and
generated native build templates. Higher-level runtime features can be connected
later without making generated projects depend on the in-repo runtime layout.

The current ABI includes arithmetic and compare helpers, UTF-16 `String`
operations, and an opaque dynamically growing `StringBuilder` state with
explicit init, destroy, snapshot, and Java-object materialization boundaries.

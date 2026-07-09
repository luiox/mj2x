# Java obfuscator style benchmark fixture

This directory contains source-only benchmark inputs for `mj2x/benchmark`.
The benchmark script compiles these Java sources into `test/` at run time and
does not require committed binary JARs.

The fixture intentionally touches the RegC full-native hot paths that have been
optimized recently:

- wrapper compare and `toString` intrinsics;
- `String` read-only intrinsics;
- non-escaping `StringBuilder` append chains, including `char[]`;
- repeated multi-array descriptors for JNI class-cache measurements.

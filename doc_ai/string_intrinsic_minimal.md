# String/StringBuilder Intrinsic Minimal Slice

## Purpose

Issue #440 is larger than the current RegC/R3 live-validation window because
full Java `String` and `StringBuilder` support needs UTF-16 ownership, object
materialization, escape analysis, and GC/writeback rules.  The v2.1 first step
therefore fixes the runtime boundary rather than pretending the full intrinsic
set is ready.

## Runtime ABI

`mj2x-runtime/src/intrinsic/intrinsic_string.hpp` defines:

- `mj2x_utf16_view`: runtime-owned UTF-16 code-unit view.
- `mj2x_string_status`: explicit fail-closed status codes.
- `mj2x_string_length`: minimal `String.length()` semantics over a view.
- `mj2x_string_char_at`: minimal `String.charAt(int)` semantics over a view.
- `mj2x_stringbuilder_unsupported_status`: current `StringBuilder` /
  `StringBuffer` fail-closed marker.

This ABI deliberately does not accept `jstring` or return `jobject`.  JNI string
materialization remains in emitter-local helpers such as
`_mj2x_intrinsic_string_literal` and `_mj2x_intrinsic_parse_int`.

## Emitter Policy

RegC must only emit these runtime calls when all conditions are true:

- the UTF-16 storage is owned by generated/runtime code and outlives the call;
- the value does not escape as `jobject`, field, array element, monitor target,
  unknown call argument, or cross-CFG mutable state;
- unsupported operations fail closed with a clear reason instead of falling
  through to imprecise native behavior.

Until escape-aware fallback exists, full `String.concat`, `substring`,
`equals`, `indexOf`, `valueOf`, and all `StringBuilder` mutation chains remain
future work.

## Validation

`mj2x_runtime_test` covers the minimal shim through `IntrinsicStringTest`:

- UTF-16 length and `charAt`.
- empty view with null data.
- null non-empty view fail-closed.
- out-of-bounds fail-closed.
- explicit StringBuilder unsupported status.

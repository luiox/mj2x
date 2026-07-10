# String/StringBuilder Intrinsic Minimal Slice

## Purpose

Issue #440 is larger than the current RegC/R3 live-validation window because
full Java `String` and `StringBuilder` support needs UTF-16 ownership, object
materialization, escape analysis, and GC/writeback rules.  The v2.1 first step
therefore fixes the runtime boundary rather than pretending the full intrinsic
set is ready.

## Existing Runtime Slice

`mj2x-runtime/src/intrinsic/intrinsic_string.hpp` defines:

- `mj2x_utf16_view`: runtime-owned UTF-16 code-unit view.
- `mj2x_string_status`: explicit fail-closed status codes.
- `mj2x_string_length`: minimal `String.length()` semantics over a view.
- `mj2x_string_char_at`: minimal `String.charAt(int)` semantics over a view.
- `mj2x_stringbuilder_unsupported_status`: current `StringBuilder` /
  `StringBuffer` fail-closed marker.

This in-tree runtime slice deliberately does not accept `jstring` or return `jobject`. JNI string
materialization remains in emitter-local helpers such as
`_mj2x_intrinsic_string_literal` and `_mj2x_intrinsic_parse_int`.

## Distributable StringBuilder C ABI

`mj2x-crt/src/regc/mj2x_regc_crt.h` is the public ABI for the next
StringBuilder lowering phase. It keeps `mj2x_string_builder` opaque and exposes
only C99/JNI operations:

- explicit `init`, `init_jstring`, and `destroy` lifecycle;
- UTF-16 code-unit append plus `jstring`, object, primitive, and `char[]`
  append operations;
- `to_jstring` for a local `String` snapshot and `materialize` for a local
  `StringBuilder` escape bridge;
- explicit `mj2x_regc_status` results and `mj2x_regc_report_status` for the
  Java exception bridge.

`PENDING_EXCEPTION` means JNI has already established the Java exception and
must not be replaced. Other failed statuses map to a stable Java exception
class through the RegC context callback. The current implementation is C99 and
stores UTF-16 with explicit capacity growth; a later `.cpp` implementation may
replace it without changing generated C or the source-pack manifest contract.

## Emitter Policy

RegC must only emit these runtime calls when all conditions are true:

- the UTF-16 storage is owned by generated/runtime code and outlives the call;
- the value does not escape as `jobject`, field, array element, monitor target,
  unknown call argument, or cross-CFG mutable state;
- unsupported operations fail closed with a clear reason instead of falling
  through to imprecise native behavior.

Current RegC first-slice StringBuilder lowering is intentionally narrower than
the full runtime ABI target.  It supports non-escaping `new`/`<init>` plus
`append(String|Object|CharSequence|int|long|boolean|char|char[]|float|double)`
and `toString()` with a fixed UTF-8-friendly buffer.  `append(char[])` reads
JVM `char[]` elements as UTF-16 code units and preserves the JDK null NPE
boundary, but it still materializes into the temporary first-slice buffer.

The emitter currently retains its fixed-buffer
`mj2x_regc_local_string_builder` lowering. Moving it to opaque CRT state needs
cleanup on every return and exception path, so the new ABI is defined and tested
before generated code begins to allocate it. Until that lifecycle work and
escape-aware fallback exist, full `String.concat`, `substring`,
additional `String.valueOf` overloads, `StringBuilder` range overloads,
capacity semantics, and escaping mutation chains remain future work.

## Implementation Boundary

The next StringBuilder/UTF-16 upgrade belongs in `mj2x-crt`, behind its C ABI,
rather than more generated-C helper bodies. The CRT has no direct or transitive
`libca` dependency: a generated native project must remain a self-contained
source pack. `libca` may supply semantic references, conformance test vectors,
or independently ported algorithms, but CRT must own its allocators and
implementation dependencies. mj2x needs:

- owning UTF-16 string and builder buffers with explicit capacity growth;
- conversion helpers for JNI modified UTF-8, UTF-16 views, and Java `char[]`;
- surrogate and non-ASCII behavior that matches JDK `String`/`StringBuilder`;
- a C ABI wrapper that can materialize native builder state back to `jobject`;
- escape-aware ownership hooks so RegC can fallback or materialize instead of
  fail-closing when a shadow object crosses a native/JNI boundary;
- a future synchronized wrapper point for `StringBuffer` semantics.

## Validation

`mj2x_runtime_test` covers the existing minimal shim through `IntrinsicStringTest`:

- UTF-16 length and `charAt`.
- empty view with null data.
- null non-empty view fail-closed.
- out-of-bounds fail-closed.
- explicit StringBuilder unsupported status.

`mj2x_crt_test` covers the CRT StringBuilder ABI directly, including UTF-16
append/length, allocation failure reporting, and the status-to-exception bridge.

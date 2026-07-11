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

RegC emits these runtime calls under the following ownership rules:

- the UTF-16 storage is owned by generated/runtime code and outlives the call;
- an escaping value is materialized before use as a `jobject`, field, array
  element, or unknown-call argument; cross-CFG aliases retain the opaque handle;
- a chain that mutates after materialization falls back to Java from its
  allocation point;
- unsupported operations fail closed with a clear reason instead of falling
  through to imprecise native behavior.

Current RegC StringBuilder lowering uses the opaque CRT state directly. It
supports `new`/`<init>` plus
`append(String|Object|CharSequence|int|long|boolean|char|char[]|float|double)`
and `toString()` with dynamically growing UTF-16 storage. Every generated
function owns a cleanup path that destroys its native builders on normal return,
throw, unreachable, and uncaught-exception exits.

Return, unknown-call, field, array, and CFG escape points lazily materialize a
real Java `StringBuilder`. Repeated escapes reuse the same materialized object.
If a chain mutates after escape, analysis falls back from its allocation point
to Java operations so native and Java state cannot diverge. The CRT also owns
`String.concat`, `substring`, `compareTo`, `indexOf` overloads,
`replace(char,char)`, and `trim`. Remaining scope is StringBuilder range and
capacity APIs, additional overloads, synchronized StringBuffer, and
`String.split`.

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

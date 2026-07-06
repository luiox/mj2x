# Runtime Kernel Design

## Purpose

`runtime/src/kernel/` is the small shared runtime layer for key derivation,
resource integrity, and secret memory hygiene.  Other runtime modules should use
it instead of each inventing local key schedules.

## Design

The kernel uses HKDF-SHA256 with explicit salt, input key material, and info
bytes.  That gives RegC, string pools, and future protected resources a common
domain-separation pattern while keeping tests deterministic.  HMAC verification
uses the crypto module's constant-time comparison.  Temporary secret buffers are
cleared through volatile byte stores.

## Boundaries

The kernel does not own a global runtime context yet.  Generated code or a
future JNI adapter must supply sample seeds and labels explicitly.  This keeps
the first R3 runtime usable for unit tests and protected sample generation before
the final loader ABI is fixed.

## Validation

`mj2x_runtime_test` checks HKDF-SHA256 against RFC 5869 test case 1, verifies
HMAC rejection on modified data, and confirms secure wipe clears byte buffers.

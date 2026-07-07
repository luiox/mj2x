# Runtime Crypto Design

## Purpose

`runtime/src/crypto/` provides the portable cryptographic floor for R3.  It is
kept inside mj2x so protected samples do not inherit OpenSSL, Windows CNG, or
JDK provider dependencies.

## Design

The first slice implements SHA-256, HMAC-SHA256, AES-128 block encryption,
AES-128 CTR XOR, and constant-time comparison.  SHA/HMAC feed the runtime
kernel's HKDF and resource verification.  AES-CTR gives future resource and
string-pool formats a dependency-free stream primitive.  The module is
value-oriented: callers pass byte buffers and get fixed-size outputs back, while
ownership and wiping of secret material stay outside in the kernel module.

## Boundaries

This module does not decide protection policy and does not store process-global
keys.  Higher layers choose labels, counters, and fail-closed behavior.

## Validation

`mj2x_runtime_test` checks SHA-256, HMAC-SHA256, and AES-128 block encryption
against public known-answer vectors, verifies AES-CTR round-trip behavior, and
checks constant-time equality behavior.

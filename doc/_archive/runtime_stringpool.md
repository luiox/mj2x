# Runtime String Pool Design

## Purpose

`mj2x-runtime/src/stringpool/` provides the first R3 string-pool decoder for RegC and
protected samples.  Its job is to remove clear text and static XOR keys from
generated native code.

## Design

Each string entry has encrypted bytes and a label.  At runtime the module derives
a per-entry keystream with HKDF-SHA256 using a runtime root key and the entry
label, then XORs that stream with the payload.  The same transform is symmetric,
so tests and fixture generation can create deterministic encrypted entries.

## Boundaries

The current format is intentionally small: no compression, no record table ABI,
and no authenticated metadata.  Future sample formats can add those pieces while
keeping the same label-based key separation.

## Validation

`mj2x_runtime_test` verifies round-trip decode and confirms different labels
produce different ciphertext for the same plaintext and root key.

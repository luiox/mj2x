# mj2x JSON config

`mj2x-cli --config <file>` loads command-line defaults from a JSON object.
Explicit command-line arguments override values from the config file.

Supported keys:

| key | type | CLI equivalent |
|---|---|---|
| `input` | string | `-i` |
| `output` | string | `-o` |
| `emit_c` | string | `--emit-c` |
| `backend` | string: `stackc`, `regc`, `cbackend`, or `c` | `--backend` |
| `native_annotation` | string | `--native-annotation` |
| `native_method_prefixes` | string array | repeated `--native-method-prefix` |
| `compile` | boolean | `--compile` |
| `c_compiler` | string: `msvc`, `gcc`, or `clang` | `--c-compiler` |
| `generate_xmake` | string | `--generate-xmake` |
| `generate_cmake` | string | `--generate-cmake` |
| `dump_mir` | boolean | `--dump-mir` |

Example:

```json
{
  "input": "Hello.class",
  "output": "Hello_native.class",
  "emit_c": "Hello.c",
  "backend": "regc",
  "dump_mir": true
}
```

Unknown keys and type mismatches are rejected. Normal CLI validation still
applies after config loading, including mutually exclusive options such as
`native_annotation` versus `native_method_prefixes`, and `generate_cmake`
versus `generate_xmake`.

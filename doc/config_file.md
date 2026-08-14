# mj2x JSON config（现状）与配置系统设计指向

## 现状（v2.2）

`mj2x-cli --config <file>` loads command-line defaults from a JSON object.
Explicit command-line arguments override values from the config file.

Supported keys（**现状仅覆盖以下平铺键**；完整参数面见 §2 缺口）：

| key | type | CLI equivalent |
|---|---|---|
| `input` | string | `-i` |
| `output` | string | `-o` |
| `emit_c` | string | `--emit-c` |
| `emit_cpp` | string | `--emit-cpp` |
| `emit_llvm` | string | `--emit-llvm` |
| `backend` | string: `stackc`, `regc`, `cbackend`, or `c` | `--backend` |
| `native_annotation` | string | `--native-annotation` |
| `native_method_prefixes` | string array | repeated `--native-method-prefix` |
| `compile` | boolean | `--compile` |
| `compile_mode` | string: `file` or `pipe` | `--compile-mode` |
| `c_compiler` | string: `msvc`, `gcc`, `clang`, `zig`, or `xmake` | `--c-compiler` |
| `llvm_tool` | string: `clang` or `llc` | `--llvm-tool` |
| `generate_xmake` | string | `--generate-xmake` |
| `generate_cmake` | string | `--generate-cmake` |
| `dump_mir` | boolean | `--dump-mir` |

Example:

```json
{
  "input": "Hello.class",
  "output": "Hello_native.class",
  "emit_cpp": "Hello.cpp",
  "backend": "regc",
  "dump_mir": true
}
```

Unknown keys and type mismatches are rejected. Normal CLI validation still
applies after config loading, including mutually exclusive options such as
`native_annotation` versus `native_method_prefixes`, and `generate_cmake`
versus `generate_xmake`. `c_compiler: "xmake"` selects the xmake build driver rather than a C
compiler and creates a cleaned temporary project unless `generate_xmake` supplies a durable path.

## 缺口与设计

现状 `--config` 只覆盖 15 个字段；`--regc-*`、`--vmc4-*`、`--runtime-protection*`、
`--client-id`、`--native-lib-key`、`--native-lib-pack-hook`、`--intrinsic-*`、
`--emit-native-dir` 等 **40+ 参数不能进配置文件**，且无 schema 导出（GUI 配置
工具无从接入）。

目标形态见 **`doc_ai/config_system_design.md`**（设计稿 v1）：

- 参数注册表单源定义（类型/默认值/描述/枚举/范围），统一驱动 CLI 解析、
  配置文件解析、`--help` 与 `--dump-config-schema`（JSON Schema draft-07）。
- 配置 JSON 按域嵌套分组（global/selection/compile/stackc/regc/vmc4/runtime/
  nativeLib/client/intrinsic/log），规范 ID 为 `域.键`。
- 合并语义不变：默认值 < 配置文件 < CLI 参数；全默认产物逐字节一致。
- 竞品调研依据：`doc_ai/competitor_config_reference.md`（jvmtp / skidoni）。

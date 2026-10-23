# mj2x JSON config（配置系统）

## 概览

`mj2x-cli --config <file>` 从 JSON 配置读取默认参数，命令行显式参数覆盖配置。
**全量 45+ 参数均可配置**（参数注册表单源驱动）。
未知键与类型错误被拒绝，报错定位到键名。

```json
{
  "input": "app.jar",
  "output": "app_protected.jar",
  "backend": "regc",
  "regc_obfuscate": true,
  "regc_hide_constants": true,
  "regc_indirect_calls": true,
  "regc_transform_seed": 42,
  "client_id": 7,
  "runtime_protection": "fake",
  "native_lib_key": "1a2b3c4d"
}
```

## 键表

键名 = snake_case 平铺（规范 ID `域.键` 的 `.` → `_`）。完整清单与默认值：

| 域 | 键 | 类型/枚举 | 默认 |
|---|---|---|---|
| global | `input` / `output` | string | — |
| global | `backend` | `stackc` \| `regc` \| `llvm` \| `vmc4`（`with_mj2x_llvm=n` 构建无 `llvm`） | `stackc` |
| global | `emit_c` / `emit_cpp` / `emit_llvm` / `emit_native_dir` | string | — |
| global | `dump_mir` | boolean | `false` |
| selection | `native_annotation` | string（JVM desc） | — |
| selection | `native_method_prefixes` | string[] | — |
| selection | `native_include` / `native_exclude` | string[]（类名 glob；`*` 不跨段、`**` 跨段、`/` 与 `.` 等价；exclude 优先；空片段拒绝） | — |
| compile | `compile` | boolean | `false` |
| compile | `compile_mode` | `file` \| `pipe` | `file` |
| compile | `c_compiler` | `auto` \| `msvc` \| `gcc` \| `clang` \| `zig` \| `xmake` | `auto` |
| compile | `native_opt_level` | `0` \| `1` \| `2` \| `3` \| `s` | `2` |
| compile | `native_timeout` | integer ≥ 0（秒；`0` = 不限时；每次外呼独立计时，超时整树终止） | `900` |
| compile | `llvm_tool` | `clang` \| `llc` | `clang` |
| compile | `generate_cmake` / `generate_xmake` | string | — |
| stackc | `stackc_exception_check` | `full` \| `lite` | `full` |
| obf_scope | `obf_scope_include` / `obf_scope_exclude` | string array（类级 glob，与 `native_include` 同语义） | `[]` |
| regc | `regc_trace_calls` / `regc_direct_calls` / `regc_safe_env` / `regc_safe_env_debug` / `regc_no_inline` / `regc_obfuscate` / `regc_cff_state_encrypt` / `regc_mba_deep` / `regc_opaque_zero` / `regc_hide_constants` / `regc_indirect_calls` | boolean | `false` |
| regc | `regc_comment_level` | `none` \| `summary` \| `detailed` | `summary` |
| regc | `regc_cff_dead` | integer ≥ 0 | `0` |
| regc | `regc_transform_seed` | integer ≥ 0 | `0` |
| vmc4 | `vmc4_dispatcher` | `switch_table` \| `funcptr_table` \| `computed_goto` \| `if_nest` \| `state_dispatcher` | `switch_table` |
| vmc4 | `vmc4_seed` | hex string / number | `0` |
| vmc4 | `vmc4_granularity` | `class` \| `method` \| `program` | `class` |
| vmc4 | `vmc4_alias`（0..3）/ `vmc4_bogus`（0..4）/ `vmc4_drift`（0..4）/ `vmc4_drift_period`（≥1） | integer | `1` / `1` / `0` / `16` |
| vmc4 | `vmc4_trace_surface` | string | — |
| vmc4 | `vmc4_encrypt_program` / `vmc4_lazy_decrypt` / `vmc4_block_lazy_decrypt` / `vmc4_shuffle_blocks` | boolean（两个懒解密档要求 `vmc4_encrypt_program` 且互斥） | `false` |
| vmc4 | `vmc4_super_ops` / `vmc4_fragment` | boolean（相邻指令融合 / 指令碎片化，可组合；fragment 指令数约 2~3 倍） | `false` |
| strings | `strings_encrypt` | boolean | `false` |
| runtime | `runtime_protection` | `none` \| `log` \| `delay` \| `fake` \| `terminate` | `none` |
| runtime | `runtime_protection_step`（≥1）/ `runtime_protection_delay_ms`（≥0） | integer | `16` / `500` |
| nativeLib | `native_lib_key` | hex string / number | — |
| nativeLib | `native_lib_pack_hook` | string | — |
| client | `client_id` | integer ≥ 0 | `0` |
| intrinsic | `intrinsic_config` | string（外部 intrinsic JSON 路径） | — |
| intrinsic | `intrinsic_src` | string[] | — |
| log | `log_quiet` / `log_verbose` | boolean | `false` |

## Schema 与模板

```sh
mj2x-cli --dump-config-schema[=<file>]      # JSON Schema draft-07（裸=stdout，内联 =file 写文件）
mj2x-cli --dump-config-template[=<file>]    # 全量键 + 默认值 + 注释模板（JSONC）
```

> 文件目标仅支持内联 `=` 形态（`--dump-config-schema=out.json`）；空格形态
> 不再消费后继 token，避免与位置参数的歧义。二者互斥，同给报错。

- **schema** 供 GUI 配置工具 / VS Code（`json.schemas`）/ ajv 校验直接消费；
  键含 `description`/`default`/`enum`/`minimum`/`maximum`/`x-requires`。
- **模板** 是带注释的手写起点（删除注释即得合法配置）。

## 语义

- 合并顺序：默认值 < 配置文件 < CLI 参数（CLI 显式给出即覆盖）。
- 未知键/类型错误/枚举越界/范围越界 → 报错含键名（如
  `unknown config key: foo_bar`、`config key must be an integer: regc_cff_dead`）。
- 后端相关性（regc/vmc4/runtime 族）沿用 main.cpp 交叉校验，schema 以
  `x-requires` 标注。
- 用户 intrinsic JSON（`intrinsic_config` 指向的文件）格式独立，见
  [user_intrinsic.md](user_intrinsic.md)。

# mj2x JSON config（配置系统）

## 概览

`mj2x-cli --config <file>` 从 JSON 配置读取默认参数，命令行显式参数覆盖配置。
**全量 45+ 参数均可配置**（参数注册表单源驱动）。
未知键与类型错误被拒绝，报错定位到键名。

参数数量已到 59 个（regc 域独占 14 个），命令行越来越难管理。除配置文件外另提供
两组能力：**preset 默认配置档**（一次设一组）与**生效配置回显**（排查"到底开了啥"）。

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
| regc | `regc_trace_calls` / `regc_safe_env` / `regc_safe_env_debug` / `regc_no_inline` / `regc_obfuscate` / `regc_cff_state_encrypt` / `regc_mba_deep` / `regc_opaque_zero` / `regc_hide_constants` / `regc_indirect_calls` / `regc_pod_struct` / `regc_computed_jumps` | boolean | `false` |
| regc | `regc_direct_calls` | `off` \| `intra-class` \| `all`（RegC + LLVM 同一门控） | `off` |
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

## preset（默认配置档）

`mj2x-cli --preset <name>` 一次设一组参数。

| 档 | 定位 | 内容 |
|---|---|---|
| `none` | 零混淆，最快路径（**默认**） | 与不加 `--preset` 完全一致 |
| `perf` | 性能优先，零混淆 | `regc_direct_calls` 开、`regc_no_inline` 关（保留内联）、`regc_safe_env` 关、`native_opt_level` = `3`、`regc_comment_level` = `none` |
| `balanced` | 常规保护，代价可控 | `regc_obfuscate` 开 + `regc_cff_state_encrypt`（编译期随机化，零运行时代价）+ `strings_encrypt` |
| `max` | 最强，代价不设限 | 基础三项 + `regc_mba_deep` / `regc_opaque_zero` / `regc_cff_state_encrypt` / `regc_cff_dead=3` + 两项重型（`regc_hide_constants` / `regc_indirect_calls`）+ `strings_encrypt` + `runtime_protection=fake` |

preset 不是第四套配置语法——它是**内置的 JSON 对象**，键名与配置文件完全相同，
走同一套 `parseJson` 校验。档位只列"与默认值不同"的键，是建议值不是硬编码全量：
在 preset 之上仍可用配置文件或 CLI 逐项改。

`perf` 档的主要收益来自直调：实测（jvm-obf-tester，7 次取中位数）把 Inheritance
probe 从 12.858ms 降到 2.542ms（5.06x），合计追平未混淆的 JVM 基线。代价是
#814 的栈观察语义失真，故只在明确性能优先的档位里开。

## 生效配置回显

```sh
mj2x-cli --dump-effective-config[=<file>]      # 四层合并结果（裸=stdout，内联 =file 写文件）
```

59 个开关 + 四层合并 + 联动规则（增强项隐含开基础项），出错时没有可核对的输出
就只能靠猜。回显形如：

```json
{
  "preset": "max",
  "config": "app.json",
  "layers": ["default", "preset", "config", "cli"],
  "effective": {
    "regc_obfuscate": { "value": false, "source": "config" }
  }
}
```

`source` 是最终写入该键的那一层。`--dump-effective-config` 与
`--dump-config-schema` / `--dump-config-template` 互斥，且都不要求 `-i/-o`。

## 语义

- 合并顺序：默认值 < preset < 配置文件 < CLI 参数（每层显式给出即覆盖上一层）。
- 未知键/类型错误/枚举越界/范围越界 → 报错含键名（如
  `unknown config key: foo_bar`、`config key must be an integer: regc_cff_dead`）。
- 后端相关性（regc/vmc4/runtime 族）沿用 main.cpp 交叉校验，schema 以
  `x-requires` 标注。
- 用户 intrinsic JSON（`intrinsic_config` 指向的文件）格式独立，见
  [user_intrinsic.md](user_intrinsic.md)。

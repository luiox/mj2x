# mj2x 配置系统设计（config system design）

> 状态：**P0+P1 已实现**（2026-08-15，PR 见 git log；本文档同步为实现态）。
> 驱动源：`competitor_config_reference.md`（jvmtp / skidoni 竞品调研）。
> 关联：`doc/config_file.md`（配置键表）、`doc/user_guide.md` §4（参数参考）。

## 1. 背景与目标

mj2x 参数面已膨胀到 **50+ 个 CLI 参数**（三后端 + 混淆 + 反侦测 + native 打包 +
per-client ISA），但 `--config` 只覆盖其中 13 个平铺字段，且无类型校验元数据、
无分组、无 schema。命令行只适合 CI 与探索；**真实项目保护需要可复用、可评审、
可版本化的配置文件，并导出 schema 以支撑可视化 GUI 配置工具**。

目标：

1. **全量参数可配置**：CLI 参数 ↔ 配置键 1:1 映射，无遗漏。
2. **单源定义**：参数的类型/默认值/描述/枚举/范围只声明一次，驱动 CLI 解析、
   配置文件解析、`--help`、schema 导出四处，杜绝现状的「help 与 config 文档漂移」。
3. **Schema 导出**：`mj2x-cli --dump-config-schema` 输出 JSON Schema，GUI 配置
   工具与编辑器（VS Code 补全/校验）直接消费。
4. **合并语义明确**：默认值 < 配置文件 < CLI 参数；行为与现状逐字节一致
   （配置化不改变任何默认产物）。
5. **精确报错**：未知键、类型错误、枚举/范围越界、互斥冲突报错定位到 JSON 路径。

非目标：不做 YAML/TOML 多格式（JSON Schema 生态是 GUI 最短路径）；不做运行时
热重载；不做环境变量展开；不做配置加密（配置不是机密，密钥走 `--native-lib-key`
等 CLI/环境形态另行讨论）。

## 2. 现状与缺口

现状（`mj2x-cli/src/main.cpp` + `config_file.{hpp,cpp}`）：

- `CliOptions` 手写结构体（50+ 字段），CLI 解析为手写 `if/else` 链（~200 行），
  `printUsage()` 手写 help 文本（~90 行），三处各自维护。
- `ConfigFileOptions` 仅 13 字段：`input/output/emit_c/emit_cpp/emit_llvm/backend/
  native_annotation/native_method_prefixes/compile/compile_mode/c_compiler/
  llvm_tool/generate_xmake/generate_cmake/dump_mir`（另有两处文档不一致：
  `config_file.md` 还列了 `intrinsic_config`、`vmc4_granularity`，实现未见）。
- 已具备：配置先加载、CLI 后覆盖的顺序；未知键/类型错误拒绝。

缺口清单（不能进配置的现有参数）：`--regc-*` 全部 12 个、`--vmc4-*` 全部 8 个、
`--client-id`、`--stackc-exception-check`、`--runtime-protection*` 3 个、
`--native-lib-key`、`--native-lib-pack-hook`、`--intrinsic-config`、
`--intrinsic-src`、`--emit-native-dir`、`--native-method-prefix`（已支持）、
`--llvm-tool`（未支持）。

## 3. 设计决策

| 决策点 | 结论 | 理由 |
|---|---|---|
| 格式 | **JSON（平铺 snake_case 键）** | 现有基础（libca_json + `--config` 已实现）；**向后兼容旧 15 键**（input/output/emit_c/... 同名保留）；JSON Schema 生态（draft-07/2020-12）成熟：VS Code 原生、vue-json-schema-form / react-jsonschema-form / ajv 直接可用，GUI 工具零成本接入 |
| 键风格 | 平铺 snake_case（jvmtp 风格域前缀平铺视角）；规范 ID 为 `域.键` | 配置可读性（jvmtp 同款）+ 单源注册表用 `域.键` 定位；schema 的 `x-requires`/description 表达分组与依赖 |
| 单源 | **参数注册表**（声明式 ParamDef 数组） | 消灭 help/config/schema 三处漂移（已实现：`param_registry.{hpp,cpp}`） |
| 默认值语义 | 缺省 = 注册表默认值；所有「默认关闭」开关保持默认 false | 与 skidoni「注释即关闭」等价，且更安全（未知键即报错） |
| 方法与类选择 | 沿用现有 `native_annotation` / `native_method_prefixes` | jvmtp 的 `native.direction` include/exclude 与 obfuscation 作用域过滤（§4.4）作为后续扩展点，本次不引入 |
| schema 版本 | JSON Schema **draft-07** | 工具兼容面最广（VS Code、form 库、ajv 均支持）；2020-12 后续可选 |
| intrinsic 配置 | **保持独立文件**（`--intrinsic-config` 用户 JSON），主配置只存路径 | 格式与语义不同（intrinsic 描述表），混入主配置会破坏 schema 纯净性 |

## 4. 参数注册表（核心）

### 4.1 ParamDef 声明

```cpp
// mj2x-cli/src/param_registry.hpp（P1 实现）
enum class ParamType { Bool, Int, UInt64, Hex, Enum, String, StringList };

struct ParamDef {
    const char* id;          // 规范 ID："regc.cff_dead"
    const char* cliFlag;     // "--regc-cff-dead"（nullptr = 仅配置键，保留扩展）
    const char* jsonPath;    // "regc_cff_dead"（snake_case 平铺 JSON 键，向后兼容）
    ParamType   type;
    const char* enumValues;  // 逗号分隔（Enum 用）；Int/UInt64 用 min/max
    i64         minValue = 0, maxValue = 0;   // 0 = 无约束（Int/UInt64）
    bool        defaultBool = false;
    i64         defaultInt  = 0;
    const char* defaultStr;  // String/Hex/Enum/StringList 默认
    const char* description; // help 与 schema description 同源（中文，现状 help 文案迁移）
    const char* group;       // "global" / "selection" / "compile" / "stackc" /
                             // "regc" / "vmc4" / "runtime" / "nativeLib" / "client" / "intrinsic" / "log"
    const char* requires;    // 依赖条件："regc"（仅 backend=regc 时有效），空 = 无
};
```

要点：

- **`id` 即唯一键**。CLI flag、JSON path、默认值、描述全部从一条声明派生。
- 枚举参数的合法值写在 `enumValues`，CLI 与 JSON 共用同一校验。
- `requires` 表达后端相关性（如 `regc.*` 需 `backend=regc`、`vmc4.*` 需
  `backend=vmc4`、`runtime.*` 需 regc/vmc4），校验阶段统一检查；互斥组
  （emit-c/emit-cpp/emit-llvm、generate-cmake/generate-xmake、annotation/prefix）
  由独立的互斥表表达。
- 注册表是 `constexpr`/`static const` 数组，`paramCount()` 编译期可知。

### 4.2 完整参数映射表（已实现全量）

> JSON 键 = snake_case 平铺（= 规范 ID 的 `.` → `_`）；旧 15 键（input/output/
> emit_c/...）同名保留，向后兼容。全表在 `param_registry.cpp` 的 `kParamTable`。

| JSON 键 | 规范 ID | CLI | 类型/枚举/范围 | 默认 |
|---|---|---|---|---|
| `input` | global.input | `-i` | String | — |
| `output` | global.output | `-o` | String | — |
| `backend` | global.backend | `--backend` | Enum stackc/regc/vmc4（LLVM 构建含 llvm） | stackc |
| `emit_c` | global.emit_c | `--emit-c` | String | — |
| `emit_cpp` | global.emit_cpp | `--emit-cpp` | String | — |
| `emit_llvm` | global.emit_llvm | `--emit-llvm` | String | — |
| `emit_native_dir` | global.emit_native_dir | `--emit-native-dir` | String | — |
| `dump_mir` | global.dump_mir | `--dump-mir` | Bool | false |
| `native_annotation` | selection.native_annotation | `--native-annotation` | String（JVM desc） | — |
| `native_method_prefixes` | selection.native_method_prefixes | `--native-method-prefix`（可重复） | StringList | — |
| `compile` | compile.enabled | `--compile` | Bool | false |
| `compile_mode` | compile.mode | `--compile-mode` | Enum file/pipe | file |
| `c_compiler` | compile.c_compiler | `--c-compiler` | Enum msvc/gcc/clang/zig/xmake | auto |
| `llvm_tool` | compile.llvm_tool | `--llvm-tool` | Enum clang/llc | clang |
| `generate_cmake` | compile.generate_cmake | `--generate-cmake` | String | — |
| `generate_xmake` | compile.generate_xmake | `--generate-xmake` | String | — |
| `stackc_exception_check` | stackc.exception_check | `--stackc-exception-check` | Enum full/lite | full |
| `regc_trace_calls` | regc.trace_calls | `--regc-trace-calls` | Bool | false |
| `regc_direct_calls` | regc.direct_calls | `--regc-direct-calls` | Bool | false |
| `regc_comment_level` | regc.comment_level | `--regc-comment-level` | Enum none/summary/detailed | summary |
| `regc_safe_env` | regc.safe_env | `--regc-safe-env` | Bool | false |
| `regc_safe_env_debug` | regc.safe_env_debug | `--regc-safe-env-debug` | Bool | false（隐含 safe_env） |
| `regc_no_inline` | regc.no_inline | `--regc-no-inline` | Bool | false（内联默认开） |
| `regc_obfuscate` | regc.obfuscate | `--regc-obfuscate` | Bool | false |
| `regc_cff_dead` | regc.cff_dead | `--regc-cff-dead` | Int ≥0 | 0 |
| `regc_cff_state_encrypt` | regc.cff_state_encrypt | `--regc-cff-state-encrypt` | Bool | false |
| `regc_mba_deep` | regc.mba_deep | `--regc-mba-deep` | Bool | false |
| `regc_opaque_zero` | regc.opaque_zero | `--regc-opaque-zero` | Bool | false |
| `regc_transform_seed` | regc.transform_seed | `--regc-transform-seed` | UInt64 | 0 |
| `vmc4_dispatcher` | vmc4.dispatcher | `--vmc4-dispatcher` | Enum switch_table/funcptr_table/computed_goto/if_nest/state_dispatcher | switch_table |
| `vmc4_seed` | vmc4.seed | `--vmc4-seed` | Hex | 0 |
| `vmc4_granularity` | vmc4.granularity | `--vmc4-granularity` | Enum class/method/program | class |
| `vmc4_alias` | vmc4.alias | `--vmc4-alias` | Int 0..3 | 1 |
| `vmc4_bogus` | vmc4.bogus | `--vmc4-bogus` | Int 0..4 | 1 |
| `vmc4_drift` | vmc4.drift | `--vmc4-drift` | Int 0..4 | 0 |
| `vmc4_drift_period` | vmc4.drift_period | `--vmc4-drift-period` | Int ≥1 | 16 |
| `vmc4_trace_surface` | vmc4.trace_surface | `--vmc4-trace-surface` | String | — |
| `runtime_protection` | runtime.protection | `--runtime-protection` | Enum none/log/delay/fake/terminate | none |
| `runtime_protection_step` | runtime.protection_step | `--runtime-protection-step` | Int ≥1 | 16 |
| `runtime_protection_delay_ms` | runtime.protection_delay_ms | `--runtime-protection-delay` | Int ≥0 | 500 |
| `native_lib_key` | native_lib.key | `--native-lib-key` | Hex | — |
| `native_lib_pack_hook` | native_lib.pack_hook | `--native-lib-pack-hook` | String | — |
| `client_id` | client.id | `--client-id` | UInt64 | 0 |
| `intrinsic_config` | intrinsic.config | `--intrinsic-config` | String（外部 JSON 路径） | — |
| `intrinsic_src` | intrinsic.sources | `--intrinsic-src`（可重复） | StringList | — |
| `log_quiet` | log.quiet | `--quiet` | Bool | false |
| `log_verbose` | log.verbose | `--verbose` | Bool | false |

> 注：`emit_llvm` / `llvm_tool` 在显式关闭 LLVM 的构建（`with_mj2x_llvm=n`）下
> 不存在——注册表按编译宏裁剪（与 help 行为一致）；默认构建（v2.2 起）包含。

### 4.3 生成物

注册表驱动四个消费者：

1. **CLI 解析**：`--flag` 查表得 ParamDef → 类型化解析 → 写入 `CliOptions`
   （通过每个 ParamDef 关联的成员偏移/访问器）。手写 if/else 链删除。
2. **配置解析**：JSON 对象按 `jsonPath` 逐键写入（缺省不动 = 保持默认值），
   未知键/类型错误报 `config.json: <path>: <原因>`。
3. **`--help`**：按 `group` 分组输出，描述文本与 schema 同源。
4. **`--dump-config-schema <file>`**：见 §5。

### 4.4 后续扩展点（本次不实现）

- obfuscation 作用域过滤（jvmtp `<module>.include/exclude` 两级 glob）：RegC
  现有 `--regc-*` 都是全局开关，方法级作用域需要 transform pass 支持 per-site
  过滤——列为独立主线，注册表已预留 StringList 形态。
- `native.direction` include/exclude 语义、`rename.*` 命名 scheme：同上，记录不实施。

## 5. Schema 导出（GUI 工具路径）

### 5.1 CLI 形态

```
mj2x-cli --dump-config-schema [<file>]   # 缺省输出到 stdout；<file> 写 JSON 文件
```

### 5.2 输出（JSON Schema draft-07）

```jsonc
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://morpher.dev/schema/mj2x-config.schema.json",
  "title": "mj2x config",
  "description": "mj2x native protection configuration",
  "type": "object",
  "properties": {
    "backend": { "type": "string", "enum": ["stackc","regc","vmc4"],
                 "default": "stackc", "description": "选择 native 发射后端",
                 "x-requires": "" },
    "regc_cff_dead": { "type": "integer", "minimum": 0, "default": 0,
                       "description": "RegC: CFF 恒假死块密度（0 关闭）",
                       "x-requires": "regc" }
    // ... 全部平铺键（45 个）
  },
  "additionalProperties": false
}
```

要点：

- `description`/`default`/`enum`/`minimum`/`maximum` 全部来自注册表，与 `--help`
  同源，永不漂移。
- `additionalProperties: false`：编辑器/GUI 直接拒绝未知键（与 CLI 行为一致）。
- 后端相关性以 **`x-requires` 元数据**标注（`regc`/`vmc4`/`regc|vmc4`/`stackc`），
  运行时校验沿用 main.cpp 交叉校验链；if-then 条件子 schema 列为 P2 增强
  （draft-07 支持 `if: {properties: {backend: {const: "regc"}}}` 形式）。
- 生成实现：注册表遍历 → `nlohmann::json` 构建 → 写出（格式化 2 空格，键字母序）。

### 5.3 GUI 工具消费路径

| 消费者 | 接入方式 |
|---|---|
| VS Code | `json.schemas` 指向 schema URL/文件 → 补全、校验、悬停文档零成本 |
| vue-json-schema-form / react-jsonschema-form | 直接渲染表单（下拉=enum、数字=minimum/maximum、开关=boolean、描述=label） |
| ajv 等校验器 | CI 里校验用户的配置文件 |
| AI/CLI 包装 | schema 喂给代码生成器产出命令行或配置模板 |

GUI 配置工具本体（独立仓库或 `mj2x/tools/config-gui`）是 P2：输入 = schema 文件，
输出 = 配置文件，`mj2x-cli --config` 消费——**协议即 schema，两端零耦合**。

## 6. 合并与校验语义

1. **顺序**：注册表默认值 → 配置文件（`--config`，可多次？现状只一次，P1 保持
   一次）→ CLI 参数。CLI 显式给出即覆盖；`--flag` 布尔开关与 JSON `true` 等价。
2. **校验时序**：配置文件解析期校验类型/枚举/范围/未知键；CLI 解析后执行现有
   交叉校验链（互斥、requires、backend 依赖），报错格式含参数名与 JSON 路径。
3. **行为不变约束**：全部参数缺省时，产物与现状逐字节一致（现有
   `cli_test.cpp` 的产物一致性用例即为回归护栏；`regc-transform-seed=0`、
   `client-id=0` 等默认值不得改动）。
4. **模板输出**（低成本附加，P1 可选）：`--dump-config-template` 输出全量键 +
   默认值 + 注释的示例配置，供手写起点（对齐 jvmtp "full example" 惯例）。

## 7. 实现方案（P1，✅ 已落地）

| 文件 | 变更 | 状态 |
|---|---|---|
| `mj2x-cli/src/param_registry.hpp` | `ParamDef` + `CliOptions`（迁移）+ 查询/遍历 API + schema 构建声明 | ✅ |
| `mj2x-cli/src/param_registry.cpp` | `kParamTable`（45 键）+ CLI/JSON 解析回调 + schema/help/template 构建 | ✅ |
| `mj2x-cli/src/config_file.hpp/cpp` | 重写：注册表驱动解析，删除手写 13 字段结构 | ✅ |
| `mj2x-cli/src/main.cpp` | 手写解析链/help 删除 → 查表 + 分组 help；新增 `--dump-config-schema` / `--dump-config-template` | ✅ |
| `mj2x/unittest/src/cli/config_registry_test.cpp` | schema 键全覆盖/注释一致性、模板全键、未知键/类型/枚举错误定位、config backend 驱动校验、CLI 覆盖 config、多族参数一次配置 | ✅ |

测试矩阵：`mj2x_unittest` 全量（含 ConfigRegistryTest 10 项）+ `mtest_set_unittest`
（回归护栏）；CLI 侧行为不变由既有 `cli_test.cpp` 全量保障。

## 8. 验收标准（P1 达成状态）

1. ✅ §4.2 表中每个 CLI 参数都能通过配置文件等价设置（`--config` 自身与
   `--help`/`--quiet`/`--verbose` 日志键除外，见 §2 缺口说明）。
2. ✅ `--dump-config-schema` 输出 draft-07 JSON Schema（additionalProperties
   false），键/默认值/枚举/范围/描述与注册表一致（ConfigRegistryTest 断言）。
3. ✅ 全默认配置产物不变：既有 cli_test 产物一致性用例保持绿（全量回归）。
4. ✅ 错误信息定位到键名（如 `unknown config key: foo_bar`、
   `config key must be an integer: regc_cff_dead`）。
5. ✅ `--help` 与 schema description 同源（ConfigRegistryTest 描述计数断言）。
6. ✅ 全量回归：`mj2x_unittest` + `mtest_set_unittest` 绿。

## 9. 分期

- **P1（本设计评审后实现）**：✅ 已完成（§4/§5/§6/§7 全部；纯 CLI 侧重构 +
  schema 导出 + 测试，无产物变化）。
- **P2（独立）**：GUI 配置工具（消费 schema）；if-then 条件子 schema；
  obfuscation 方法级作用域（§4.4）落注册表扩展。
- **P3（观察）**：多配置文件叠加、环境变量、YAML 前端（均需真实需求驱动）。

## 10. 边界

- 不引入第三方配置库（libca_json 已够）；schema 生成纯代码。
- 不改变现有 `--config` 键名（保持向后兼容；旧 13 键映射进注册表同名项）。
- 用户 intrinsic JSON（`--intrinsic-config` 指向的文件）格式不变。
- 不实现 GUI 本体（P2）；不实现 YAML。

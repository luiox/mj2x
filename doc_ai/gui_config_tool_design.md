# mj2x GUI 配置工具设计（vite + vue）

> 状态：**设计完成（P2）**，实现待排期。
> 前置：配置系统 P0+P1 已完成（`config_system_design.md`：参数注册表单源 +
> `--dump-config-schema` 导出 draft-07 JSON Schema）。
> 协议：**schema 即契约**——GUI 只消费 `mj2x-cli --dump-config-schema` 的输出，
> 与 CLI/配置解析零代码耦合；CLI 侧新增参数（如 LLVM 选项）GUI 自动出现。

## 1. 背景与目标

mj2x 参数面已到 50+（三后端 + 混淆 + 反侦测 + native 打包 + LLVM），真实项目
保护需要可复用、可评审、可版本化的配置编辑体验。CLI 适合 CI 与探索；**GUI 面向
"配置一遍、团队复用、评审安全"的场景**：按分组呈现参数、联动显隐、实时校验、
导出配置/命令行，并可一键调用 CLI 编译。

目标：

1. **schema 驱动**：表单完全由 `--dump-config-schema` 输出渲染，无手写表单定义；
   参数注册表新增/变更后 GUI 零改动。
2. **分组与联动**：按注册表 `group` 分页签；`x-requires` 驱动的依赖显隐/禁用与
   CLI 交叉校验链语义一致（如 `runtime-protection≠none` 时后端限 regc/vmc4）。
3. **实时校验**：ajv 校验 + 内置互斥/依赖规则，错误定位到参数卡片，先于 CLI 报错。
4. **双产物导出**：配置文件（`--config` 消费）+ 等价 CLI 命令行（复制即用）。
5. **可选执行**：本地 sidecar 调 `mj2x-cli` 跑编译，流式日志；纯前端模式不依赖
   sidecar 也能完成全部配置编辑。
6. **预设与模板**：内置常用模板 + 用户预设持久化，团队可分享配置文件。

非目标：不做云端/多用户后端；不做 IDE 插件（VS Code 的 schema 补全已由
`config_system_design.md` §5.3 覆盖）；不做 YAML/TOML 前端；不进入 xmake 默认
构建依赖（共享契约"不静默引入额外依赖"，GUI 是独立可选工具）。

## 2. 定位与边界

| 项 | 结论 |
|---|---|
| 项目位置 | `mj2x/tools/config-gui/`（独立 npm 工程，不进 xmake 构建） |
| 技术栈 | Vite 5 + Vue 3.4（script setup + TS）+ Pinia + Vue Router + ajv |
| 与 morpher-web 关系 | morpher-web 是 React 调试器/看板原型（mjdbg 面）；config-gui 是 Vue 配置工具（mj2x 面），互不依赖、不重叠 |
| 与 mj2x-cli 关系 | 无库依赖；可选 sidecar（Node 脚本）spawn `mj2x-cli` 执行编译 |
| 运行形态 | 纯静态 SPA（`pnpm dev` / `pnpm build` 产物可任意静态托管）；sidecar 仅本机开发/使用 |

## 3. 技术选型与理由

| 决策点 | 结论 | 理由 |
|---|---|---|
| 构建 | **Vite 5** | dev 秒级 HMR、ESM 优先、零配置 TS；仓库已有 vite 先例（morpher-web） |
| 框架 | **Vue 3.4 + `<script setup>` + TS** | 用户指定 vue；组合式 API 适合 schema 驱动渲染（渲染函数 + reactive 模型） |
| 状态 | Pinia | 表单模型/预设/执行会话三处共享状态，比 provide/inject 可测试 |
| 校验 | ajv（draft-07） | 与 schema 版本一致；`additionalProperties:false`、enum/range 直接复用 |
| 表单渲染 | **自研轻量 schema 驱动渲染器**（~15-20 个小组件），不用 vue-json-schema-form | mj2x 需要 x-requires 联动、分组 tab、枚举描述、禁用态 tooltip；通用 form 库支持不足且维护弱。渲染器只消费 schema 数据源，未来可换库 |
| 代码高亮/预览 | 自研 JSON 树/文本预览（v1）；Monaco 列为可选 | 配置体量小，`<pre>` + 折叠即可；避免重型依赖 |
| CLI 执行 | Node sidecar（`server/`，express 或原生 http）+ `child_process.spawn` | GUI 是浏览器页面，无法直接 spawn；sidecar 只做受控命令转发（白名单：mj2x-cli 路径 + 参数），日志 SSE 流式推送 |

## 4. 架构

### 4.1 数据流

```text
mj2x-cli --dump-config-schema <file>
        │ (一次，或由用户粘贴/上传)
        ▼
schema.json ──► SchemaLoader ──► ParamModel（注册表模型：按 group 分组的
                                      ParamDef[]，含 x-requires/互斥/默认值）
                                      │
                                      ▼
                              FormModel（reactive 值表 + 依赖状态机）
                                      │ 双向
                                      ▼
                          SchemaForm 渲染器（分组 tab + 参数卡片）
                                      │
                                      ▼
                         ajv.validate(schema, config) + 互斥/依赖校验
                                      │
                                      ▼
                         导出：config.json（下载/复制）｜ CLI 命令展开（复制）
                                      │ （可选）
                                      ▼
                          Sidecar：spawn mj2x-cli --config …（流式日志）
```

### 4.2 目录结构

```text
mj2x/tools/config-gui/
├── index.html
├── package.json / vite.config.ts / tsconfig.json
├── src/
│   ├── main.ts / App.vue
│   ├── router/                 # /editor /presets /about
│   ├── stores/                 # configStore.ts（模型+校验）、presetStore.ts、runStore.ts
│   ├── schema/                 # loadSchema.ts（fetch/上传/粘贴）、types.ts（Schema/ParamDef 类型）
│   ├── forms/                  # schema 驱动渲染器
│   │   ├── SchemaForm.vue      # 入口：分组 tabs + 参数卡片列表
│   │   ├── fields/             # StringField/NumberField/EnumField/BooleanField/ListField
│   │   └── rules.ts            # x-requires 依赖状态机 + 互斥表 + 后端条件表
│   ├── components/             # ConfigPreview.vue / CliCommandView.vue / RunPanel.vue / PresetList.vue
│   └── services/               # cli.ts（sidecar HTTP 客户端）、export.ts
└── server/                     # 可选 sidecar：index.mjs（express）、cli-runner.mjs
```

### 4.3 核心模型

```ts
// schema 类型（draft-07 子集，来自 --dump-config-schema）
interface ParamSchema {
  type: "string" | "boolean" | "integer";
  default?: string | number | boolean;
  enum?: string[];            // Enum 参数
  minimum?: number; maximum?: number;
  description?: string;       // 与 --help 同源
  "x-requires"?: string;      // "" | "regc" | "vmc4" | "regc|vmc4" | "llvm" | …
  "x-group"?: string;         // 分页签：global/selection/compile/regc/vmc4/llvm/runtime/…
}
```

- 依赖状态机：`enabled(param) = requires 满足 && 互斥未触发`。`requires` 语义与
  main.cpp 交叉校验链逐条对齐（后端条件、emit 互斥、annotation/prefix 互斥、
  `--compile-mode pipe` 与产物互斥等），规则集中在 `forms/rules.ts` 单文件，
  附来源行号注释（main.cpp 行号），CLI 侧规则变更时审计此处。
- 校验输出：`{ path, message }[]`，路径形如 `regc_cff_dead`，卡片红框 + 顶部
  汇总条；导出按钮在校验通过前禁用（可强制导出预览）。

### 4.4 Sidecar（可选执行）

```text
POST /api/run      { cliPath, configPath, workDir } → { runId }
GET  /api/run/:id  SSE 流：stdout/stderr 行 → 前端日志面板（ANSI 简化）
POST /api/run/:id/kill
```

安全约束：sidecar 仅本机监听（127.0.0.1），参数白名单（只接受 `--config` +
`--input/--output` 等 GUI 明确声明的键），不暴露任意 shell。

## 5. UI 设计

### 5.1 布局

```text
┌──────────┬──────────────────────────────┬──────────────────┐
│ 预设/项目  │ 参数分组 tabs                 │ 实时 JSON 预览     │
│ 列表      │ （全局｜选择｜编译｜RegC｜VMC4  │ （折叠/复制/下载）  │
│          │  ｜LLVM｜运行时｜原生库｜日志…）  │  校验错误汇总       │
│          │ ┌──────────────────────────┐ │  命令行展开        │
│          │ │ 参数卡片（联动显隐/禁用）   │ │  [编译并运行]（可选）│
│          │ └──────────────────────────┘ │                  │
└──────────┴──────────────────────────────┴──────────────────┘
```

### 5.2 参数卡片

- 标题 = 规范 ID（`regc.cff_dead`）+ CLI flag 徽标（`--regc-cff-dead`）。
- 控件按类型：布尔→开关；enum→下拉（description 作选项 hint）；整数→数字输入
  + min/max 提示；字符串→文本框（带占位默认值）；StringList→可增删列表。
- 未满足 `x-requires` 的参数：灰显 + "需要 backend=regc" 提示，值保留但不导出
  （与 CLI"参数无效"语义一致，避免用户改后端后丢配置）。
- 与默认值一致的值弱化显示（"= 默认"），diff 视角便于评审安全参数。

### 5.3 预设

内置模板（导出即用）：

1. 最小编译：`input/output` + `compile` + 默认 StackC。
2. RegC 保护默认：`backend=regc` + `regc_obfuscate` 系列默认组合。
3. VMC4 虚拟化：`backend=vmc4` + `vmc4_*` 推荐档。
4. LLVM 导出：`backend=llvm` + `emit_llvm` + `llvm_tool`。
5. 反侦测组合：`runtime_protection` + `regc_safe_env`。

用户预设：localStorage + 导出/导入 JSON 文件；预设文件即配置文件（同格式）。

## 6. 与后续特性的衔接（记录不实施）

- **整类下沉**（roadmap §2 表格行 + 待做）：GUI 预留"方法/类选择"页签扩展点
  （注册表 §4.4 方法级作用域落地后启用）：类树勾选下沉类、方法级 include/exclude
  glob。本轮只保证 schema 渲染器能承载 StringList 选择器，不实现类树。
- **LLVM 后端新参数**：本轮 LLVM 扩面（indy/intrinsic/保护）新增的 CLI 参数
  注册进 ParamDef 后自动出现在 LLVM 页签，GUI 无需改动。
- **mjdbg 闭环**：运行面板后续可接 mjdbg 报告展示（复用 morpher-web 思路，
  独立迭代，不在本设计内）。

## 7. 分期

| 期 | 内容 | 验收 |
|---|---|---|
| P1 | schema 加载 + 表单渲染 + 联动/校验 + JSON/CLI 导出 + 内置模板 | 用 `--dump-config-schema` 真实输出渲染 50+ 参数无遗漏；导出的 config 被 `mj2x-cli --config` 接受且与表单一致；全默认导出与 CLI 默认行为一致 |
| P2 | sidecar 执行面板（流式日志/中断）、用户预设管理、diff 视图 | 一键编译 E2E：GUI 配置 → 编译 → 日志可见 |
| P3 | 方法/类级选择器（整类下沉等）、多配置对比、国际化 | 见 §6 |

## 8. 验收标准（P1）

1. 输入 = `mj2x-cli --dump-config-schema` 输出，无任何手写参数表。
2. 参数全量可见：注册表每个键在对应分组出现，`--help` 描述与卡片说明一致
   （同源断言，可用 config_registry_test 的 schema 为基线做快照对比）。
3. 联动正确：backend 切换时 regc/vmc4/llvm 专属参数显隐/禁用正确；
   `runtime-protection` 与后端互斥提示与 CLI 一致。
4. 校验先行：枚举/范围/未知键在 GUI 内报错，导出与 CLI 报错集一致。
5. 产物一致：GUI 导出配置 → CLI 编译产物 == 手工等价 CLI 参数编译产物
   （复用 cli_test 产物一致性基线思路）。

## 9. 决策记录

- 不用 Qt/原生：web 方案（用户指定 vite+vue），无平台打包负担，schema 生态成熟。
- 不用 vue-json-schema-form：x-requires/分组/禁用态等 mj2x 特有语义需要自定义
  渲染器；渲染器仅依赖 schema 结构，可替换。
- sidecar 只做"受控执行"，不做任意命令：GUI 的安全边界 = CLI 参数白名单。
- 不进入 xmake 构建：`package.json` 独立管理；CI 可加 `pnpm build` 冒烟但不阻塞
  xmake 主线。

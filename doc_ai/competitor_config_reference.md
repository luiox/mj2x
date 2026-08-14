# 竞品配置系统参考笔记（jvmtp / skidoni）

> 状态：2026-08-15 采集自 jvmtp.dev/docs（1.6.1）与 skidonion.tech/docs 公开文档。
> 用途：mj2x 配置系统设计（`config_system_design.md`）的输入依据。
> 原始抓取材料已清理；本文是唯一留存的可追溯摘要。

## 1. jvmtp（properties 平铺分层）

格式：Java properties（`key = value`，`#` 注释）。**域前缀平铺键**，无嵌套。

### 1.1 域与键

| 域 | 键示例 | 说明 |
|---|---|---|
| `compiler.*` | `compiler.path` / `compiler.version` / `compiler.optimization`(0-3,size) / `compiler.aggressive-inline` / `compiler.lto` / `compiler.branch-hints` / `compiler.timeout` / `compiler.intrinsics` / `compiler.safe-env` / `compiler.batch-resolve` / `compiler.export-all-symbols` / `compiler.options.<platform>` / `compiler.vmp-*` | 编译器与构建（zig 路径/版本、优化、超时、平台私有参数） |
| `native.*` | `native.annotation`（注解简单名）/ `native.direction`(include/exclude) / `native.strip` | 方法选择 |
| `filter.*` | `filter.include` / `filter.exclude`（`com/example/**` glob） | 类级过滤，叠加在 `native.*` 之上 |
| `extract.*` | `extract.init` / `extract.clinit` | 构造器/静态初始化器体提取 |
| `debug.*` | `debug.strip` / `debug.map` / `debug.trace` | 调试符号与运行时 trace |
| `target.*` | `target.platforms`（triple 列表，必填） | 目标平台 |
| `android.*` | `android.ndk` / `android.api` | Android 构建 |
| `rename.*` | `rename.clinit` / `rename.init` / `rename.loader-package` / `rename.loader-class`；取值 scheme：`fixed:<name>` / `hash` / `alphabetic` / `random` / 字面量 | 注入符号命名方案 |
| `resources.*` | `resources.compress` | 打包形态（压缩 blob） |
| `intrinsics.*` | `intrinsics.enable` / `intrinsics.hotspot.field` / `intrinsics.hotspot.array` | intrinsic 开关 |
| `obfuscation.*` | 见 1.2 | 混淆模块（opt-in） |
| `antidebug.*` | `antidebug.enable` / `antidebug.native` / `antidebug.agent` / `antidebug.recheck-period` | 反调试（条件编译） |

### 1.2 obfuscation 域（最值得借鉴的部分）

- **独立 opt-in 模块**：`obfuscation.enable` 主开关 + 每模块独立开关（constants / constants.mba / strings / calls / calls.inline / calls.native / calls.merge / arithmetic / predicates / predicates.indirect / flatten / jumps），全部默认 false。
- **作用域过滤（两级）**：全局 `obfuscation.include/exclude` + 每模块 `<module>.include/exclude`；`*` 单段、`**` 跨段通配，可带 `#method` 后缀；函数名对 intrinsic 生效。判定规则：两级都接受才应用；exclude 优先于 include；空 include = 全部。
- **条件编译**：antidebug 各家族按开关条件编译，关闭时产物零代码。
- **性能代价明示**：文档明确标注各模块的编译时间/体积/运行期代价梯度（flatten 最重），引导按需开启。

### 1.3 文档形态

- 每键一行：`| Key | Default | Description |` 表格；默认值是文档的一部分（单源）。
- 全量示例 config 文件 + 平台参数示例 + 场景化配置片段（Android、VMP、作用域）。
- 注解（`@Native`）与过滤（`filter.*`）组合语义用文字明确（"runs on top of"）。

## 2. skidoni / 幻影盾X（YAML 分组块）

格式：YAML。**顶层属性 + 每 transformer 一个子块**；注释掉整个子块 = 关闭该 transformer。

### 2.1 顶层属性

| 键 | 说明 |
|---|---|
| `input` / `output` | 必填；JAR 路径（相对/绝对均可） |
| `dictionary` / `minimum_generated_name_length` | 重命名字典（字符串 / 块数组 / `spaces` / `keywords` 预设） |
| `creation_date` | Class 文件输出日期 |
| `random_seed` | 64bit 种子，驱动重命名与成员顺序 |
| `generate_phantom_classes` / `print_classes_as_directory` / `legacy_compile_mode` | 功能开关 |
| `input_mappings_file` | 增量混淆（读取上次映射输出） |
| `cpp_compiler` / `cpp_compiler_arguments` / `cpp_compiler_output` / `cpp_compiler_is_aarch64` | native 编译器（默认 zig 交叉编译） |
| `targets` / `libraries` | 目标平台 triple 列表 / 依赖库（构建完整继承关系） |
| `filters` | `-org.example.**` 排除规则 |

### 2.2 transformer 子块（注释即关闭）

`renamer`（print_mappings / prefix_name / repackage / adapt_resources / mixin 支持）、
`string_encryption`、`member_shuffler`、`debug_information_remover`
（remove_signatures / remove_source_file / remove_inner_class / remove_line_number /
remove_local_variable / remove_kotlin_reference）、`control_flow_obfuscation`（标注实验性）、
`invoke_wrapper_obfuscation`（package_mode: random_existed / inject_to_other_class）、
`native_obfuscation`（loader_package / hidden_stack_trace / null_safety / verification 子块）。

### 2.3 注解扩展

`@NativeObfuscation(virtualize=..., verificationLock="组")`、`@Inline` 等注解做方法级
细粒度控制——配置定全局、注解定局部。

## 3. 对 mj2x 的启示（结论）

| 竞品做法 | mj2x 借鉴判断 |
|---|---|
| jvmtp：域前缀平铺键 + 每键默认值表（文档=实现同源） | ✅ 参数注册表单源驱动（help/schema/文档同源） |
| jvmtp：obfuscation 模块化 opt-in + 两级 include/exclude 作用域 | ✅ 已是 mj2x RegC #829 的实际开关形态（`--regc-*` 默认关）；配置化时保留默认关语义 |
| jvmtp：antidebug 条件编译、代价梯度明示 | ✅ 与 mj2x runtime-protection / antidebug 现状一致 |
| skidoni：YAML 嵌套块、注释即关闭 | ⚠️ mj2x 采用 JSON（现有 `--config` 基础 + libca_json）；「注释即关闭」用「缺省=默认值」语义替代 |
| skidoni：注解做方法级细粒度控制 | ⚠️ mj2x 已有 `--native-annotation`/`--native-method-prefix` 选择面；方法级混淆注解不在本次范围 |
| 两者：必填/默认/枚举/取值范围在文档明示 | ✅ schema 导出后由 GUI/编辑器直接消费 |
| jvmtp：`rename.*` 命名 scheme 化（hash/random/fixed） | ℹ️ 观察项，非本次范围 |

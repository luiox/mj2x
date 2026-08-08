# C 代码生成规范：命名、注释与源码拼接

> 适用范围：StackC 与 RegC 两个后端生成 C 代码时**共用**的命名、注释规范与源码拼接工具。
> 位置：命名规则由各后端 emitter 实现；共用工具在 `mj2x-core/src/mj2x/codegen/`（`c_source.hpp`、`c_helpers.hpp`）。
> 修订规则：任何命名/注释/拼接行为变化 → **先改本文档**，再改代码。

## 1. 总则

- 生成代码的目标是「可读、可回溯」：每个符号都能反查到 Java 侧来源（类/方法/字节码偏移），每个块/寄存器/缓存槽都带生成期注释。
- 生成源码的文本组装一律走 **Utf8 字符串家族 + CSourceStream 三层模型**（见 §3），**禁止**手写 `std::ostringstream << "    ..."` 缩进，**禁止**用 `std::string`/`std::ostringstream` 作源码拼接容器（外部 API 边界除外，见 §3.4）。
- 注释语义约定（面向定位问题，参照 Simulink 生成代码风格）：
  - 每个 helper 函数头：`// RegC method: <owner>.<name><desc>`（Java 侧完整签名）。
  - 每个基本块：`// block_b<n>: <来源说明>`（如 `entry` / `exception handler of <type>` / 终结类型）。
  - 每类缓存槽声明：一行注释说明该槽保有的 Java 符号（owner/name/desc）。
  - 寄存器不做逐个注释（数量爆炸），由块注释 + 辅助名表达语义。

## 2. 标识符命名表

### 2.1 函数符号（`regc_*`）

规则实现：`regc_emitter.cpp` `functionName()` / `sanitizeIdent()`。

```
regc_<sanitize(owner)>_<sanitize(name)>_<sanitize(desc)>
```

`sanitizeIdent`：`[A-Za-z0-9]` 原样保留，其余任意字节 → `_`；首字符为数字时前缀 `_`。

示例：

| Java 方法 | 生成 C 函数 |
|---|---|
| `RegCCapture.addDelta(II)I` | `regc_RegCCapture_addDelta__II_I`（`(`/`)` → `_`） |
| `RegCBox.bump(Box)int` | `regc_RegCBox_bump__LRegCBox__I` |
| `RegCCapture.lambda$addDelta$0(II)I` | `regc_RegCCapture_lambda_addDelta_0__II_I`（`$` → `_`） |

### 2.2 JNI wrapper（`__mj2x_c_*`）

规则：`regc_backend.cpp` `nativeFunctionName()`。

```
__mj2x_c_<hex64(fnv1a64(className|methodName|methodDesc))>
```

每个 native 方法的 C 入口名不携带人类可读信息（防符号名长度/冲突）；与 JNI 导出名完全无关（RegisterNatives 动态绑定）。

### 2.3 RegisterNatives 注册函数（`_mj2x_register_*`）

规则：`regc_backend.cpp` `registerFunctionName()`。

```
_mj2x_register_<sanitize(className)>
```

类名中非字母数字字节映射为 `_` + 十六进制低 2 位（防碰撞），如 `a/b` → `a_b2f`。

### 2.4 缓存槽（`_mj2x_regc_*_cache_tu_N`）

规则：`regc_emitter.cpp` `RegCCachePool`，计数器按类别独立从 0 递增。

| 类别 | 前缀 |
|---|---|
| class | `_mj2x_regc_class_cache_tu_<N>` |
| 实例方法 | `_mj2x_regc_instance_cache_tu_<N>` |
| 静态方法 | `_mj2x_regc_cache_tu_<N>`（最简前缀，历史命名） |
| 实例字段 | `_mj2x_regc_instance_field_cache_tu_<N>` |
| 静态字段 | `_mj2x_regc_field_cache_tu_<N>` |

`<N>` 与声明顺序严格对应（同一类别的第 K 次 add 即 `<K>`），便于反查 `RegCCachePool` 的 poolCacheKey。

### 2.5 寄存器（`r<id>`）

规则：`regc_emitter.cpp` `regName()`。`r<N>`，N 为 `RegId::value`（0 起，与 RegIR 的 SSA 值号一致）。参数一定位于低编号（`fn.parameters` 顺序）。

### 2.6 基本块标签（`block_b<n>`）

规则：`regc_emitter.cpp` `emitFunction()`。`block_b<n>`，n 为 `Block::id.value`。块注释在规范 §1 约定。

### 2.7 辅助临时量

| 名称 | 模式 | 用途 |
|---|---|---|
| 边拷贝临时 | `edge_tmp_<n>` | phi 并行转移的 stage 临时（每块清 0 初始化） |
| 可变字符串持有 | `native_owner_r<n>` | StringBuilder/StringBuffer 原生持有句柄 |
| 可变字符串物化 | `native_materialized_r<n>` | 物化回 Java 引用的槽 |
| native 对象存储 | `native_r<n>` | 寄存器上的原生对象存储 |
| 状态检查临时 | `runtime_status_<n>` | 运行时状态码临时 |

## 3. 源码拼接规范（Utf8 字符串家族 + CSourceStream 三层模型）

生成源码是 UTF-8 文本流。三层工具职责固定，缺一不可：

### 3.1 三层分工

| 层 | 工具 | 职责 |
|---|---|---|
| 排版 | `codegen::CSourceStream`（`c_source.hpp`） | 缩进层级与换行：底层存储即 `Utf8StringBuilder`（三同一体）；`emitLine()` 一行一调用，嵌套块 `indent()/dedent()` 成对（可用 RAII `IndentGuard`）；多片段拼一条语句用 `appendLineStart()` + `append()` + `newLine()`；`emitRaw()` 仅用于外部已格式化的整块（runtime 模板段），禁止用它逃脱缩进统一 |
| 拼接 | `ca::str::Utf8StringBuilder`（可变 UTF-8 字符串容器） | 逐段/逐行构建源码文本；`append(const char*, usize)` 不校验字节，混淆样本的任意字节 owner/desc 可安全入流 |
| 驻留 | `ca::str::Utf8StringArena` | 产物文本 `intern_raw` 入 arena，翻译单元存活期内有效；结束 `clear()` 一次性释放全部 chunk（ref 全部失效） |

`CSourceStream` 的 `append` 重载与 `Utf8StringBuilder` 对齐（Utf8String/Utf8StringRef/`const char*`/字节对/`string_view`/`appendInt`），emitter 的多片段拼接（如 `out.append(operandExpr(...))`）可直接落到 CSourceStream，无需跨容器转换。

### 3.2 生命周期

- 一个 C 翻译单元 = 一个 arena。backend 组装 helper/cache/注册代码全部入 arena，交付边界一次性转出；转出后 `clear()` 回收。
- 行级临时拼段用 `Utf8StringBuilder`（栈上），不 intern；只有产物块才落 arena。

### 3.3 禁止

- `std::ostringstream` / `std::string` 作为源码拼接容器（唯一例外见 §3.4）。
- 字符串字面量里手数空格模拟缩进。
- 存量 `std::ostringstream` 发射点是已知技术债：新增/修改代码一律走本规范，存量渐进迁移。

### 3.4 外部 API 边界

`RegCEmitResult.source`、`RegCBackend` 产物文本、测试断言的 `std::string` 是合法交付边界：arena/builder 文本在交付点一次 `to_std_string()`。边界内不再出现 `std::string` 拼接。

### 3.5 `<init>` / `<clinit>` 特殊 bridge 方法

- 两者**不进入** native 化路径（backend 保留字节码；`<init>` 的缺失化调用走 intrinsic 的 `<init>` 条目，不产生 `regc_*` helper 函数体）。
- 需要把 `<init>`/`<clinit>` 映射为 C 标识符时（JNI 导出名）按 `mangleMethod`：`<init>` → `init_`，`<clinit>` → `clinit_`（`c_helpers.hpp` 已实现）。
- 若 `functionName()` 意外收到含 `<>` 的名字：`<init>` sanitize 后为 `_init_`（如 `regc_<owner>__init__<desc>`），属于**不稳定的历史产物，禁止依赖**；引用该类符号必须用上面的 `init_`/`clinit_` 映射。
- 生成注释里引用 `<init>`/`<clinit>` 时保留 JVM 原名（`<init>`），按 §4 走 `escapeCommentIdent`（`<`/`>` 属可打印 ASCII 原样保留）。

## 4. 生成注释规范（Simul 风格对照）

每类产物在生成时必须携带以下溯源信息：

| 产物 | 注释 | 示例 |
|---|---|---|
| helper 函数 | Java 侧完整签名 | `// RegC method: com/x/Foo.compute(ILjava/lang/String;)I` |
| `block_b<n>` | 来源（入口/handler/终结类型） | `// block_b2: exception handler of Lcom/x/Foo;` |
| 缓存槽声明 | 对应 Java 符号（有注释行随声明一起输出） | `// cache: static com/x/Foo.value I` |
| 帧/清理区 | 用途短语 | `// R6 ref frame: PushLocalFrame(16)` |

注释只表达"这段代码是什么"的溯源事实；不允许在生成代码里写设计决策长文（设计理由留在 `doc_ai/`）。生成注释不影响产物符号与 E2E 基线断言（现有断言均匹配 `regc_*`/`_mj2x_*` 标识符，注释共存）。

### 4.1 注释内 Java 符号转义（混淆样本安全）

混淆/二次 native 化样本的类名、方法名、字段名、desc 可能携带任意字节（非 ASCII、控制符）。它们写进 C 注释前必须过 `escapeCommentIdent`：

- ASCII 可打印（`0x20..0x7E`）原样保留。
- 其余字节按 UTF-8 解码为 Unicode 码点后输出转义：BMP 用 `\uXXXX`（4 位大写 hex），增补平面用 `\UXXXXXXXX`（8 位大写 hex）。
- 解码失败的畸形字节按单字节输出 `\uXX`，防止字节序列被注入 `*/`、`\r` 等破坏注释流；`//` 行注释天然免疫换行，但统一转义保证任意字节都不会破坏生成 C 的文本完整性。

所有携带 Java 符号的注释（函数头、缓存槽、块来源）一律走本转义；标识符构造路径本身（§2 各表）不需要——那里有独立的绝对安全构造（`sanitizeIdent`/hex64）。

### 4.2 注释分级体系

生成注释按详细程度分三级，由 `RegCCommentLevel`（`regc_emitter.hpp`）控制，`RegCBackend::setCommentLevel` 在 compileMethod 前透传给 `RegCCachePool` 与 `RegCEmitter`：

| 级别 | 产出 | 用途 |
|---|---|---|
| `None` | 不生成任何注释 | 最小输出基线、产物体积敏感场景 |
| `Summary`（默认） | 函数头 `// RegC method:` + 基本块来源 `// block_b<n>:` + 缓存槽 `// cache:` + 清理区用途 | 定位问题：每个符号可反查 Java 侧来源 |
| `Detailed` | Summary 全部 + 每条 RegIR 指令/终结器前一行 IR dump 注释 | 逐字节排查生成代码与 IR 的一致性 |

Detailed 级的逐指令注释直接复用 `reg_dump` 的行渲染（`regInstructionLine(ins)` / `regTerminatorLines(block)`，见 `reg_dump.hpp`）：每行以 `// ` 前缀注入，JVM 符号已过 §4.1 转义，故任意字节样本的 Detailed 注释同样文本安全。

判定规则：
- 基本块来源：入口块 → `entry`；异常 handler 块（命中某 `RegExceptionRegion.handler`）→ `exception handler of <转义 caughtType>`；其余 → `block`。
- 注释只表达溯源事实，不写设计决策长文；注释不影响产物符号与 E2E 基线断言（断言匹配 `regc_*`/`_mj2x_*` 标识符，注释共存）。

## 5. 变更规程

- 新增命名类别 → 本文档 §2 加行，再动代码。
- 删改命名 → 先改本文档 + 改 E2E/单测里硬编码的断言字符串。
- 违反本规范（手拼 ostream 缩进）的代码 → 回归时 `review` 网关拦截。

## 6. 相关文档

- `c_source.hpp`：工具实现。
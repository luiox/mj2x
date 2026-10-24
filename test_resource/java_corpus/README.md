# java_corpus — mj2x 自建 Java 语料集（dev_plan B 项）

系统性 Java 特性语料 + 差分对拍，是全仓的**核心正确性验证面**：用大量真实
javac 产物（而非手工字节码）压后端全链路，挖语义回归与能力缺口。

## 形态

- **用例 = 自包含 .java**（自带 `main`、输出确定性），顶层目录 = 类别：
  - `language/` — Java 语言特性面（javac 字节码覆盖矩阵）
  - `typical/` — 现实小程型（状态机 / 解析器 / 排序 / 图遍历…）
  - `edge/` — 边界压力（wide 指令 / 大 switch / 超长表达式链 / 参数数边界…）
  - `perf/` — 性能档（只打确定性校验和，耗时由 runner 测量）
- **判定 = 差分对拍**：baseline JVM 输出 vs 保护后 JVM 输出逐字节一致。没有
  expected 文件要维护——baseline 输出即金标准。
- **矩阵 = case × 后端**（stackc / regc / vmc4），runner 在
  `unittest/src/integration/java_corpus_e2e_test.cpp`（mj2x_integration_test）。
- **exclusions.txt** — 已知缺口的显式记账（`<case> <backend> <原因>`）：
  失败先分诊，能修就修；确属能力缺口才进表，修复后删行。

## 档位

| 档 | 开关 | 内容 |
|---|---|---|
| plain（默认） | — | 每后端最小开关 |
| obf | `MJ2X_CORPUS_TIER=obf` | regc: `--regc-obfuscate --regc-computed-jumps --regc-hide-constants`；vmc4: `--vmc4-super-ops --vmc4-fragment --vmc4-regfile 8 --vmc4-shuffle-blocks` —— 混淆组合吃满语料 |
| perf | `MJ2X_CORPUS_PERF=1` | perf/ 语料 × 列（baseline + 三后端），5 轮取中位数打表；对拍照常 |

## 写新用例的规则

1. 自带 `main`，输出**确定性**：禁用 identity hashCode / 多线程顺序 / 当前时间 /
   栈帧形态（`getStackTrace` 在保护后不同）。
2. 一个文件压一个主题，文件名 = 顶层类名（stem 必须与类名一致）。
3. 运行退出码 0、stdout 非空。
4. 提交前本地双跑 diff 确认确定性。
5. 中文注释保存为 UTF-8（runner javac 带 `-encoding UTF-8`）。

## 覆盖清单（当前 36 用例）

- **language (16)**：算术边界、浮点语义、switch 全形态、分支与循环、方法形态、
  对象分派、数组全类型、异常流、字符串与装箱、枚举、lambda 捕获、内部类与
  监视器、泛型与集合、实例生命周期、双槽交错、intrinsic 命中面
- **typical (8)**：词频统计、矩阵运算、迷你 JSON、哈希与位操作、排序查找、
  状态机、迷你大整数、图遍历
- **edge (8)**：wide 局部变量、大 switch、超长表达式链、参数数边界、finally
  返回值矩阵、异常×回边×活性、Unicode 文本、静态初始化网络
- **perf (4)**：算术、字符串、调用、字段与集合

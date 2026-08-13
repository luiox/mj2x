# jvm-obf-tester 13-probe 对比（2026-08-13）

- 环境：JDK openjdk version "21.0.4" 2024-07-16 LTS
- 取样：预热 1 轮 + 3 轮取平均
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比）

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms）

| 项 | RegC | os30 | ratio (RegC/os30) |
|---|---|---|---|
| Total | 157.7 | 101.3 | 1.56 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.215 | 1.257 | 1.76 |
| Class loading | 0.178 | 0.174 | 1.02 |
| Collections and generics | 10.529 | 7.576 | 1.39 |
| Control flow | 0.212 | 0.145 | 1.47 |
| Crypto APIs | 43.686 | 28.847 | 1.51 |
| Enums | 22.588 | 14.528 | 1.55 |
| Exception handling | 1.512 | 0.517 | 2.92 |
| Inheritance and interfaces | 22.637 | 9.477 | 2.39 |
| Inner classes | 7.61 | 4.274 | 1.78 |
| Reflection and annotations | 27.95 | 19.883 | 1.41 |
| Resource loading | 0.823 | 2.124 | 0.39 |
| String and Unicode handling | 8.862 | 4.17 | 2.13 |
| Visitor pattern | 8.067 | 8.424 | 0.96 |

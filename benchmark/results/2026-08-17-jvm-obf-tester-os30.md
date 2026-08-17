# jvm-obf-tester 13-probe 对比（2026-08-17）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 1 轮 + 3 轮取平均
- RegC 产物：D:\WorkSpace\ai2\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比）

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms）

| 项 | RegC | os30 | ratio (RegC/os30) |
|---|---|---|---|
| Total | 175.3 | 194 | 0.9 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.118 | 1.817 | 1.17 |
| Class loading | 0.292 | 0.349 | 0.84 |
| Collections and generics | 11.361 | 13.967 | 0.81 |
| Control flow | 0.267 | 0.224 | 1.19 |
| Crypto APIs | 72.339 | 62.085 | 1.17 |
| Enums | 11.548 | 24.458 | 0.47 |
| Exception handling | 1.182 | 1.357 | 0.87 |
| Inheritance and interfaces | 16.419 | 16.441 | 1 |
| Inner classes | 8.126 | 7.123 | 1.14 |
| Reflection and annotations | 31.009 | 34.487 | 0.9 |
| Resource loading | 1.436 | 8.665 | 0.17 |
| String and Unicode handling | 7.781 | 8.147 | 0.95 |
| Visitor pattern | 9.88 | 13.781 | 0.72 |

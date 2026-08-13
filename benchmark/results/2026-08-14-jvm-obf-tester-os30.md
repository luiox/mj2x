# jvm-obf-tester 13-probe 对比（2026-08-14）

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
| Total | 140.3 | 105.3 | 1.33 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.336 | 1.303 | 1.79 |
| Class loading | 0.253 | 0.17 | 1.49 |
| Collections and generics | 7.833 | 7.467 | 1.05 |
| Control flow | 0.205 | 0.141 | 1.46 |
| Crypto APIs | 42.221 | 31.828 | 1.33 |
| Enums | 18.239 | 15.183 | 1.2 |
| Exception handling | 0.844 | 0.519 | 1.63 |
| Inheritance and interfaces | 17.386 | 9.22 | 1.89 |
| Inner classes | 6.136 | 4.072 | 1.51 |
| Reflection and annotations | 22.83 | 19.486 | 1.17 |
| Resource loading | 0.673 | 2.085 | 0.32 |
| String and Unicode handling | 13.654 | 4.499 | 3.03 |
| Visitor pattern | 6.484 | 9.224 | 0.7 |

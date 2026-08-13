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
| Total | 154.7 | 167.3 | 0.92 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 1.985 | 1.975 | 1.01 |
| Class loading | 0.296 | 0.284 | 1.04 |
| Collections and generics | 10.38 | 11.925 | 0.87 |
| Control flow | 0.199 | 0.203 | 0.98 |
| Crypto APIs | 50.392 | 37.805 | 1.33 |
| Enums | 22.176 | 23.837 | 0.93 |
| Exception handling | 1.149 | 0.787 | 1.46 |
| Inheritance and interfaces | 17.145 | 15.946 | 1.08 |
| Inner classes | 7.61 | 5.351 | 1.42 |
| Reflection and annotations | 25.957 | 34.219 | 0.76 |
| Resource loading | 0.81 | 3.49 | 0.23 |
| String and Unicode handling | 6.679 | 18.335 | 0.36 |
| Visitor pattern | 9.223 | 12.335 | 0.75 |

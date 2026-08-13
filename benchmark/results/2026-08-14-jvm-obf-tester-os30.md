# jvm-obf-tester 13-probe 对比（2026-08-14）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
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
| Total | 139 | 116 | 1.2 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.099 | 1.244 | 1.69 |
| Class loading | 0.225 | 0.183 | 1.23 |
| Collections and generics | 9.271 | 7.906 | 1.17 |
| Control flow | 0.2 | 0.152 | 1.32 |
| Crypto APIs | 43.636 | 33.232 | 1.31 |
| Enums | 20.943 | 17.4 | 1.2 |
| Exception handling | 1.063 | 0.765 | 1.39 |
| Inheritance and interfaces | 16.127 | 10.48 | 1.54 |
| Inner classes | 6.504 | 5.038 | 1.29 |
| Reflection and annotations | 23.866 | 20.434 | 1.17 |
| Resource loading | 0.813 | 4.158 | 0.2 |
| String and Unicode handling | 5.596 | 4.899 | 1.14 |
| Visitor pattern | 7.762 | 9.69 | 0.8 |

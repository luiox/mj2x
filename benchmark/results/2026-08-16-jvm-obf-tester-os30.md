# jvm-obf-tester 13-probe 对比（2026-08-16）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 1 轮 + 5 轮取平均
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
| Total | 131 | 114.6 | 1.14 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 1.619 | 1.113 | 1.46 |
| Class loading | 0.201 | 0.19 | 1.06 |
| Collections and generics | 7.787 | 7.947 | 0.98 |
| Control flow | 0.185 | 0.193 | 0.96 |
| Crypto APIs | 37.197 | 33.161 | 1.12 |
| Enums | 19.272 | 16.28 | 1.18 |
| Exception handling | 1.663 | 0.673 | 2.47 |
| Inheritance and interfaces | 18.126 | 10.927 | 1.66 |
| Inner classes | 6.76 | 4.78 | 1.41 |
| Reflection and annotations | 18.04 | 18.918 | 0.95 |
| Resource loading | 0.61 | 4.24 | 0.14 |
| String and Unicode handling | 5.382 | 5.927 | 0.91 |
| Visitor pattern | 13.321 | 9.706 | 1.37 |

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
| Total | 116 | 122.3 | 0.95 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 1.563 | 1.435 | 1.09 |
| Class loading | 0.17 | 0.2 | 0.85 |
| Collections and generics | 8.368 | 9.216 | 0.91 |
| Control flow | 0.196 | 0.146 | 1.34 |
| Crypto APIs | 38.902 | 37.246 | 1.04 |
| Enums | 9.289 | 20.56 | 0.45 |
| Exception handling | 0.997 | 0.729 | 1.37 |
| Inheritance and interfaces | 13.13 | 8.483 | 1.55 |
| Inner classes | 5.187 | 6.098 | 0.85 |
| Reflection and annotations | 21.818 | 17.347 | 1.26 |
| Resource loading | 0.653 | 4.495 | 0.15 |
| String and Unicode handling | 6.478 | 5.782 | 1.12 |
| Visitor pattern | 8.722 | 10.157 | 0.86 |

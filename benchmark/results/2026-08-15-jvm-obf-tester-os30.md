# jvm-obf-tester 13-probe 对比（2026-08-15）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 1 轮 + 1 轮取平均
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- 模式：性能回归（#820 步骤 5，基线 D:\WorkSpace\ai\morpher\mj2x\benchmark\baseline\regc-perf-baseline.json，MaxTotalRatio=0.1，MinCoverageRatio=0.9）

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |

## 总耗时（ms）

| 项 | RegC |
|---|---|
| Total | 134 |

## 各探针耗时（ms，平均）

| Probe | RegC |
|---|---|
| Arithmetic operations | 1.854 |
| Class loading | 0.214 |
| Collections and generics | 7.666 |
| Control flow | 0.168 |
| Crypto APIs | 37.428 |
| Enums | 20.334 |
| Exception handling | 1.917 |
| Inheritance and interfaces | 18.127 |
| Inner classes | 6.823 |
| Reflection and annotations | 19.838 |
| Resource loading | 0.622 |
| String and Unicode handling | 4.924 |
| Visitor pattern | 13.412 |

# jvm-obf-tester 13-probe 对比（2026-08-14）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 1 轮 + 5 轮取平均
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
| Total | 361.4 | 241.4 | 1.5 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 3.519 | 2.332 | 1.51 |
| Class loading | 0.416 | 0.303 | 1.37 |
| Collections and generics | 19.609 | 17.66 | 1.11 |
| Control flow | 0.38 | 0.247 | 1.54 |
| Crypto APIs | 95.82 | 74.27 | 1.29 |
| Enums | 62.705 | 33.169 | 1.89 |
| Exception handling | 5.363 | 1.82 | 2.95 |
| Inheritance and interfaces | 47.38 | 18.12 | 2.61 |
| Inner classes | 21.077 | 13.166 | 1.6 |
| Reflection and annotations | 50.989 | 40.645 | 1.25 |
| Resource loading | 2.328 | 9.861 | 0.24 |
| String and Unicode handling | 11.266 | 9.091 | 1.24 |
| Visitor pattern | 37.746 | 19.441 | 1.94 |

## 同环境内联对照（2026-08-14，JDK 17，预热 1 + 5 轮平均）

| 项 | RegC inline（默认） | RegC no-inline（--regc-no-inline） | 差异 |
|---|---|---|---|
| Total（ms） | 361.4 | 366.2 | -1.3% |

说明：13-probe 工作负载的热点集中在探针内部循环（自定义方法多为 JDK 调用包装、方法大或异常区/多态，
不满足内联门控），内联收益被展开体代码膨胀抵消，当前负载下无显著提升；内联作为性能基础设施保留，
对热循环中满足门控的小方法（如 padRight/displayWidth 链）已消除调用帧，E2E 全探针通过（13/13）。
"## 覆盖率审计"中的方法数为转换后 native 化统计，内联不改写方法级 native 形态，覆盖率不变。
"## 各探针耗时"为 inline 版（默认开启）数据。

## 回归验证（2026-08-14）

- mj2x_unittest：541 passed + 2 skipped
- mtest_set_unittest：25 passed
- jvm-obf-tester E2E：13/13 Passed，无 lowering skip
- 可复现构建：同 seed 同 jar；native.c 生成确定性（3 次转换哈希一致）

# RegC 优化专项审计

> 日期：2026-07-23
>
> 本文记录原 RegC 优化专项的当前证据。后续 intrinsic 或 cache 改动必须先有新的
> profile、benchmark 或正确性报告，才能进入计划。

## 范围

定义原专项的三个 issue 均已关闭：

- #467：translation-unit JNI ID 与 class-cache CSE。
- #468：JDK String 热路径 intrinsic。
- #471：native 工程嵌入资源。

固定 benchmark 样本为
`mj2x/test_resource/protected_samples/regc/RegCIntrinsicBenchmarkSample.java`，覆盖 String
read hot path、可变字符串构建、parse/compare 以及多维数组分配。

## 证据

`xmake run mj2x_unittest --gtest_filter=RegCTraceSamples.GenerateMjdbgFixtures` 已在 Windows
x64、JDK 17 通过，生成样本的指标为：

| 指标 | 值 |
|---|---:|
| generated C bytes | 99,478 |
| `_mj2x_regc_call_` symbol occurrences | 36 |
| TU class cache declarations | 8 |
| TU static method cache declarations | 4 |
| TU instance method cache declarations | 1 |
| TU static field cache declarations | 0 |
| TU instance field cache declarations | 0 |

样本的结构断言确认 String read、StringBuilder character-array append、parse/compare 与
cached multi-array allocation 均已直降。通用 helper 计数反映生成 C 的 runtime surface，
不代表这些 matched call site 遗漏了 intrinsic。

## 结论

本轮不新增 intrinsic。代表性热路径样本没有未覆盖目标；没有实测调用点就增加 intrinsic
会扩张 native 语义，却没有可证明收益。新的优化必须提供可复跑 workload、生成源码结构断言、
native E2E、Java baseline 以及变更前后的指标。

# jvm-obf-tester 13-probe 对比（2026-08-17）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 对比取样：预热 1 轮 + 5 轮取平均（下表对比数据）；回归门另跑 3 轮（RegC total 126.7ms ≤ 253.4ms 阈值、覆盖率 74.7%、13-probe 全过，基线 JSON 已随本轮刷新）
- RegC 产物：D:\WorkSpace\ai2\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比）
- 工具链注记：本轮 RegC native DLL 由 xmake mingw-gcc 工具链编译（基准 shell 环境的 PATH/MSYSTEM 污染导致平台检测为 mingw）；基线与本轮同工具链，对比结论不受影响，MSVC 口径待 CI 复测。

## 本轮优化（perf/regc-probe-optimization 分支）

基线（同日同机，优化前）：Total 131.0ms / ratio 1.14。三项优化后 Total ratio 0.98（118.8ms vs 121.6ms），7/13 探针反超：

1. **枚举 intrinsic**（Enums 19.3→9.1ms）：子类 `name()/ordinal()` 直连
   字段读取（Enum final 语义等价，每调用点 2 JNI→1）；`values()` 验证 javac
   合成形态（getstatic $VALUES + 数组 clone + checkcast + areturn）后内联为
   cached getstatic + 站点缓存 clone（~5 JNI→2）；`valueOf(String)` 直调
   `Enum.valueOf`（~5 JNI→1 + JDK enumConstantDirectory 映射查找）。
2. **StringMethod 亏损 intrinsic 回退**（hashCode/charAt/equals）：多 JNI
   数据管道（3-6 JNI）不如缓存 methodID 的 1-JNI CallXxxMethodA（HotSpot
   对三者有 VM intrinsic + hashCode memoization）。
3. **确定性虚调用免检直调**：final 方法/final owner/编译输入无覆写时跳过
   运行时 IsInstanceOf（resolved 命中后本就直调 fn，栈观察语义一致；NULL
   receiver 显式守卫）。配套修正：owner 自声明非 final 方法也做覆写分析
   （新版 javac 不给匿名类设 ACC_FINAL）；convertedFunctions 按声明类回查。

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms）

| 项 | RegC | os30 | ratio (RegC/os30) |
|---|---|---|---|
| Total | 118.8 | 121.6 | 0.98 |

## 各探针耗时（ms，平均）

| Probe | RegC | os30 | ratio | 基线 RegC |
|---|---|---|---|---|
| Arithmetic operations | 1.542 | 1.32 | 1.17 | 1.619 |
| Class loading | 0.136 | 0.203 | 0.67 | 0.201 |
| Collections and generics | 8.856 | 8.993 | 0.98 | 7.787 |
| Control flow | 0.193 | 0.18 | 1.07 | 0.185 |
| Crypto APIs | 42.407 | 35.059 | 1.21 | 37.197 |
| Enums | 9.138 | 17.34 | 0.53 | 19.272 |
| Exception handling | 1.036 | 0.817 | 1.27 | 1.663 |
| Inheritance and interfaces | 14.888 | 10.596 | 1.41 | 18.126 |
| Inner classes | 5.365 | 5.372 | 1 | 6.76 |
| Reflection and annotations | 19.246 | 21.172 | 0.91 | 18.04 |
| Resource loading | 0.691 | 4.699 | 0.15 | 0.61 |
| String and Unicode handling | 6.695 | 5.302 | 1.26 | 5.382 |
| Visitor pattern | 7.761 | 10.085 | 0.77 | 13.321 |

## 遗留差距与后续方向

- Inheritance（1.41x）：剩余 ~10 JNI/迭代——`((Operation) w).apply` 的
  checkcast + 接口分派双检查、`getClass().getName().hashCode()` 链（3 JNI）。
  候选：receiver 对象身份键控的类名 hash 站点缓存（双槽，IsSameObject 命中
  可省整链 3 JNI 中的 2 个）。
- Crypto（1.21x）：JCE 单轮调用（getInstance/doFinal）为主，JDK 侧开销
  占主导，RegC 侧仅字符串 plumbing 可再省。
- 波动性：本轮 os30 轮间方差 ±20%（95-147ms）大于 RegC（109-139ms）；
  13-probe 单轮取样的 JIT 编译竞争是共同噪声源，跨轮对比以 ratio 为准。
  早前观察到的 RegC 上漂来自 GetStringUTFChars 快路径（已回退）。

## 回归验证（2026-08-17）

- mj2x_unittest：619 例 = 616 passed + 3 skipped（文档化环境门控）+ 0 failed
  （CliTest 3 例的失败为 shell 环境 MSYSTEM/PATH 污染 xmake 平台检测，
  `env -u MSYSTEM` 后全过）
- mtest_set_unittest：25/25 passed
- jvm-obf-tester E2E：13/13 Passed，无 lowering skip
- 性能回归门：total 126.7ms ≤ 253.4ms、覆盖率 74.7% ≥ 67.2%，通过

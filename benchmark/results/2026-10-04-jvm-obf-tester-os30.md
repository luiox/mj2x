# jvm-obf-tester 13-probe 对比（2026-10-04）

- 环境：JDK jdk21 [openjdk version "21.0.11" 2026-04-21 LTS]、jdk25 [openjdk version "25.0.3" 2026-04-21 LTS]（2 个 JDK，同一会话顺序计量）
- 取样：预热 3 轮（不计入统计）+ 9 轮计量；主指标=中位数，附 min~max/p95 与 CV%
- 高方差判定：CV% > 15% 标记 ⚠ 并汇总为存疑项（#887 方法论）
- RegC 产物：C:\sw\ai_worktrees\morpher-b3-kbox\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入；直调：开（--regc-direct-calls，native-to-native 直调口径））
- os30 产物：C:\Users\l1502\Downloads\jvm-obf-tester_final.jar（JNIC 竞品，黑盒对比；977338 字节，2026-08-17 16:11）
- 归因日志：C:\sw\ai_worktrees\morpher-b3-kbox\test\jvm-obf-tester\attribution（GC/类加载/JIT，各 JDK×产物冷启动 1 轮；default-gc.txt 记录各 JDK 默认收集器）

> **⚠ 高方差警告：结果存疑** — 以下样本 CV% 超过 15% 阈值，
> 不应作为打平/领先结论依据，建议提高 -Runs 或排查环境负载后重测：
> - jdk21 RegC total (CV=28.8%)
> - jdk21 RegC Arithmetic operations (CV=223.8%)
> - jdk21 RegC Crypto APIs (CV=25.5%)
> - jdk21 RegC Enums (CV=57.7%)
> - jdk21 RegC Exception handling (CV=19.1%)
> - jdk21 RegC Inheritance and interfaces (CV=141.7%)
> - jdk21 RegC Inner classes (CV=17.6%)
> - jdk21 RegC Reflection and annotations (CV=28.6%)
> - jdk21 RegC Resource loading (CV=15.7%)
> - jdk21 RegC String and Unicode handling (CV=24.2%)
> - jdk21 RegC Visitor pattern (CV=20%)
> - jdk21 os30 Arithmetic operations (CV=16.4%)
> - jdk21 os30 Class loading (CV=24.8%)
> - jdk21 os30 Enums (CV=16.1%)
> - jdk21 os30 Exception handling (CV=21.9%)
> - jdk21 os30 Inner classes (CV=20.9%)
> - jdk21 os30 Resource loading (CV=15.6%)
> - jdk21 os30 String and Unicode handling (CV=19.6%)
> - jdk25 RegC Exception handling (CV=22.5%)
> - jdk25 RegC Inheritance and interfaces (CV=17.6%)
> - jdk25 RegC Resource loading (CV=16.5%)
> - jdk25 RegC String and Unicode handling (CV=16.3%)
> - jdk25 os30 Class loading (CV=27.1%)
> - jdk25 os30 Inner classes (CV=23.7%)
> - jdk25 os30 String and Unicode handling (CV=23.1%)

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 跨 JDK 总耗时对比（ms，主指标=中位数）

| 项 | jdk21 RegC 中位数 | jdk21 os30 中位数 | jdk21 ratio | jdk25 RegC 中位数 | jdk25 os30 中位数 | jdk25 ratio |
|---|---|---|---|---|---|---|
| Total | 134 | 89 | 1.51 | 109 | 77 | 1.42 |

## 跨 JDK 探针 ratio 对比（RegC 中位数 / os30 中位数）

| Probe | jdk21 ratio | jdk25 ratio |
|---|---|---|
| Arithmetic operations | 1.56 | 1.31 |
| Class loading | 1.63 | 1.01 |
| Collections and generics | 1.11 | 1.14 |
| Control flow | 1.55 | 1.43 |
| Crypto APIs | 1.41 | 1.11 |
| Enums | 8.54 | 7.25 |
| Exception handling | 2 | 1.55 |
| Inheritance and interfaces | 0.65 | 0.66 |
| Inner classes | 1.06 | 1.12 |
| Reflection and annotations | 2.15 | 1.81 |
| Resource loading | 0.43 | 0.29 |
| String and Unicode handling | 1.61 | 1.27 |
| Visitor pattern | 1.24 | 1.25 |

## jdk21（openjdk version "21.0.11" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% | os30 中位数 | os30 min~max | os30 p95 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|---|---|
| Total | 134 | 117~262 | 262 | 28.8% ⚠ | 89 | 82~103 | 103 | 6.9% | 1.51 |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.688 | 223.8% ⚠ | 1.08 | 16.4% ⚠ | 1.56 |
| Class loading | 0.351 | 14.9% | 0.215 | 24.8% ⚠ | 1.63 |
| Collections and generics | 10.613 | 14.1% | 9.601 | 12.5% | 1.11 |
| Control flow | 0.178 | 14.8% | 0.115 | 7.3% | 1.55 |
| Crypto APIs | 48.117 | 25.5% ⚠ | 34.121 | 10.2% | 1.41 |
| Enums | 11.637 | 57.7% ⚠ | 1.362 | 16.1% ⚠ | 8.54 |
| Exception handling | 1.014 | 19.1% ⚠ | 0.508 | 21.9% ⚠ | 2 |
| Inheritance and interfaces | 2.924 | 141.7% ⚠ | 4.486 | 10.9% | 0.65 |
| Inner classes | 4.531 | 17.6% ⚠ | 4.269 | 20.9% ⚠ | 1.06 |
| Reflection and annotations | 43.98 | 28.6% ⚠ | 20.418 | 7.5% | 2.15 |
| Resource loading | 1.07 | 15.7% ⚠ | 2.468 | 15.6% ⚠ | 0.43 |
| String and Unicode handling | 7.995 | 24.2% ⚠ | 4.969 | 19.6% ⚠ | 1.61 |
| Visitor pattern | 5.133 | 20% ⚠ | 4.149 | 13.8% | 1.24 |

## jdk25（openjdk version "25.0.3" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% | os30 中位数 | os30 min~max | os30 p95 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|---|---|
| Total | 109 | 103~121 | 121 | 5.4% | 77 | 73~95 | 95 | 9.2% | 1.42 |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.697 | 14% | 1.293 | 9.4% | 1.31 |
| Class loading | 0.172 | 11.4% | 0.171 | 27.1% ⚠ | 1.01 |
| Collections and generics | 8.99 | 14.1% | 7.864 | 10.4% | 1.14 |
| Control flow | 0.167 | 10.6% | 0.117 | 8.3% | 1.43 |
| Crypto APIs | 25.635 | 8.3% | 23.086 | 12.4% | 1.11 |
| Enums | 9.688 | 13.8% | 1.336 | 12.2% | 7.25 |
| Exception handling | 0.785 | 22.5% ⚠ | 0.505 | 12.8% | 1.55 |
| Inheritance and interfaces | 2.265 | 17.6% ⚠ | 3.411 | 8% | 0.66 |
| Inner classes | 3.559 | 14.5% | 3.164 | 23.7% ⚠ | 1.12 |
| Reflection and annotations | 44.061 | 5.5% | 24.279 | 10.9% | 1.81 |
| Resource loading | 0.771 | 16.5% ⚠ | 2.68 | 12% | 0.29 |
| String and Unicode handling | 6.179 | 16.3% ⚠ | 4.859 | 23.1% ⚠ | 1.27 |
| Visitor pattern | 4.244 | 3.1% | 3.4 | 10.4% | 1.25 |

## 方法论说明（issue #887 修尺子）

- 预热 3 轮（独立 JVM，不计入统计）+ 9 轮计量（独立 JVM 冷启动），多 JDK 时同一会话顺序完成。
- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。
- p95 为 nearest-rank 口径（9 轮中第 [ceil(0.95×9)] 位）；方差/标准差为无偏估计（n-1）。
- CV% = 标准差/均值；超过 15% 的行标记 ⚠，并在文首汇总为高方差存疑项。
- 原始样本与统计面同步落 CSV：results/ 下 *-runs.csv（逐轮明细）与 *-stats.csv（统计汇总）。
- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。

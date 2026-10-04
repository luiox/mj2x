# jvm-obf-tester 13-probe 对比（2026-10-04）

- 环境：JDK jdk21 [openjdk version "21.0.11" 2026-04-21 LTS]、jdk25 [openjdk version "25.0.3" 2026-04-21 LTS]（2 个 JDK，同一会话顺序计量）
- 取样：预热 3 轮（不计入统计）+ 9 轮计量；主指标=中位数，附 min~max/p95 与 CV%
- 高方差判定：CV% > 15% 标记 ⚠ 并汇总为存疑项（#887 方法论）
- RegC 产物：C:\sw\ai_worktrees\morpher-1037-fix\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入；直调：开（--regc-direct-calls，native-to-native 直调口径））
- 模式：性能回归（#820 步骤 5，基线 C:\sw\ai_worktrees\morpher-1037-fix\mj2x\benchmark\baseline\regc-perf-baseline.json，MaxTotalRatio=2，MinCoverageRatio=0.9；多 JDK 时取第一个 JDK 的结果）

> **⚠ 高方差警告：结果存疑** — 以下样本 CV% 超过 15% 阈值，
> 不应作为打平/领先结论依据，建议提高 -Runs 或排查环境负载后重测：
> - jdk21 RegC Class loading (CV=16.8%)
> - jdk21 RegC Collections and generics (CV=19.8%)
> - jdk21 RegC Enums (CV=37.7%)
> - jdk21 RegC Inheritance and interfaces (CV=15.3%)
> - jdk21 RegC Resource loading (CV=19.5%)
> - jdk21 RegC String and Unicode handling (CV=21.7%)
> - jdk21 RegC Visitor pattern (CV=21.7%)
> - jdk25 RegC total (CV=16.9%)
> - jdk25 RegC Class loading (CV=25.9%)
> - jdk25 RegC Collections and generics (CV=20.5%)
> - jdk25 RegC Control flow (CV=26.9%)
> - jdk25 RegC Crypto APIs (CV=15.4%)
> - jdk25 RegC Enums (CV=23.1%)
> - jdk25 RegC Exception handling (CV=20.7%)
> - jdk25 RegC Inheritance and interfaces (CV=24.5%)
> - jdk25 RegC Inner classes (CV=26.4%)
> - jdk25 RegC Reflection and annotations (CV=18.2%)
> - jdk25 RegC Resource loading (CV=25.9%)
> - jdk25 RegC String and Unicode handling (CV=19.2%)
> - jdk25 RegC Visitor pattern (CV=26.7%)

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |

## jdk21（openjdk version "21.0.11" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% |
|---|---|---|---|---|
| Total | 112 | 105~157 | 157 | 13.5% |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% |
|---|---|---|
| Arithmetic operations | 1.459 | 14.1% |
| Class loading | 0.254 | 16.8% ⚠ |
| Collections and generics | 8.073 | 19.8% ⚠ |
| Control flow | 0.157 | 12.9% |
| Crypto APIs | 40.609 | 11.9% |
| Enums | 7.664 | 37.7% ⚠ |
| Exception handling | 0.74 | 14% |
| Inheritance and interfaces | 2.857 | 15.3% ⚠ |
| Inner classes | 4.08 | 14.2% |
| Reflection and annotations | 34.419 | 13.3% |
| Resource loading | 0.975 | 19.5% ⚠ |
| String and Unicode handling | 5.718 | 21.7% ⚠ |
| Visitor pattern | 4.083 | 21.7% ⚠ |

## jdk25（openjdk version "25.0.3" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% |
|---|---|---|---|---|
| Total | 98 | 92~148 | 148 | 16.9% ⚠ |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% |
|---|---|---|
| Arithmetic operations | 1.497 | 12.6% |
| Class loading | 0.159 | 25.9% ⚠ |
| Collections and generics | 7.974 | 20.5% ⚠ |
| Control flow | 0.163 | 26.9% ⚠ |
| Crypto APIs | 24.056 | 15.4% ⚠ |
| Enums | 8.257 | 23.1% ⚠ |
| Exception handling | 0.768 | 20.7% ⚠ |
| Inheritance and interfaces | 1.737 | 24.5% ⚠ |
| Inner classes | 3.489 | 26.4% ⚠ |
| Reflection and annotations | 38.79 | 18.2% ⚠ |
| Resource loading | 0.694 | 25.9% ⚠ |
| String and Unicode handling | 6.102 | 19.2% ⚠ |
| Visitor pattern | 4.166 | 26.7% ⚠ |

## 方法论说明（issue #887 修尺子）

- 预热 3 轮（独立 JVM，不计入统计）+ 9 轮计量（独立 JVM 冷启动），多 JDK 时同一会话顺序完成。
- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。
- p95 为 nearest-rank 口径（9 轮中第 [ceil(0.95×9)] 位）；方差/标准差为无偏估计（n-1）。
- CV% = 标准差/均值；超过 15% 的行标记 ⚠，并在文首汇总为高方差存疑项。
- 原始样本与统计面同步落 CSV：results/ 下 *-runs.csv（逐轮明细）与 *-stats.csv（统计汇总）。
- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。

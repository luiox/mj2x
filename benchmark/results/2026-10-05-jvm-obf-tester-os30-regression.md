# jvm-obf-tester 13-probe 对比（2026-10-05）

- 环境：JDK jdk21 [openjdk version "21.0.11" 2026-04-21 LTS]、jdk25 [openjdk version "25.0.3" 2026-04-21 LTS]（2 个 JDK，同一会话顺序计量）
- 取样：预热 3 轮（不计入统计）+ 9 轮计量；主指标=中位数，附 min~max/p95 与 CV%
- 高方差判定：CV% > 15% 标记 ⚠ 并汇总为存疑项（#887 方法论）
- RegC 产物：C:\sw\ai_worktrees\morpher-1162-string-cap\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入；直调：开（--regc-direct-calls，native-to-native 直调口径））
- os30 产物：C:\Users\l1502\Downloads\jvm-obf-tester_final.jar（JNIC 竞品，黑盒对比；977338 字节，2026-08-17 16:11）

> **⚠ 高方差警告：结果存疑** — 以下样本 CV% 超过 15% 阈值，
> 不应作为打平/领先结论依据，建议提高 -Runs 或排查环境负载后重测：
> - jdk21 RegC Crypto APIs (CV=18.9%)
> - jdk21 RegC Exception handling (CV=18.7%)
> - jdk21 RegC Inner classes (CV=16.4%)
> - jdk21 RegC Visitor pattern (CV=20%)
> - jdk21 os30 Class loading (CV=20.5%)
> - jdk21 os30 Crypto APIs (CV=15.3%)
> - jdk21 os30 Exception handling (CV=16.9%)
> - jdk21 os30 Inner classes (CV=22.4%)
> - jdk21 os30 String and Unicode handling (CV=22.4%)
> - jdk25 RegC Arithmetic operations (CV=20.7%)
> - jdk25 RegC Class loading (CV=15.5%)
> - jdk25 RegC Collections and generics (CV=15.6%)
> - jdk25 RegC Inheritance and interfaces (CV=16.3%)
> - jdk25 RegC Resource loading (CV=16.5%)
> - jdk25 RegC String and Unicode handling (CV=19.8%)
> - jdk25 os30 Class loading (CV=28.2%)
> - jdk25 os30 Collections and generics (CV=19.6%)
> - jdk25 os30 Control flow (CV=20.5%)
> - jdk25 os30 Enums (CV=18.2%)
> - jdk25 os30 Exception handling (CV=16.9%)
> - jdk25 os30 Inner classes (CV=16.1%)
> - jdk25 os30 String and Unicode handling (CV=17.3%)
> - jdk25 os30 Visitor pattern (CV=17.9%)

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 跨 JDK 总耗时对比（ms，主指标=中位数）

| 项 | jdk21 RegC 中位数 | jdk21 os30 中位数 | jdk21 ratio | jdk25 RegC 中位数 | jdk25 os30 中位数 | jdk25 ratio |
|---|---|---|---|---|---|---|
| Total | 129 | 86 | 1.5 | 120 | 89 | 1.35 |

## 跨 JDK 探针 ratio 对比（RegC 中位数 / os30 中位数）

| Probe | jdk21 ratio | jdk25 ratio |
|---|---|---|
| Arithmetic operations | 1.54 | 1.25 |
| Class loading | 1.35 | 1.25 |
| Collections and generics | 0.97 | 1.14 |
| Control flow | 1.48 | 1.16 |
| Crypto APIs | 1.49 | 1.08 |
| Enums | 5.88 | 7.83 |
| Exception handling | 1.44 | 1.53 |
| Inheritance and interfaces | 0.76 | 0.59 |
| Inner classes | 1.21 | 1.06 |
| Reflection and annotations | 1.97 | 1.58 |
| Resource loading | 0.38 | 0.34 |
| String and Unicode handling | 1.47 | 1.14 |
| Visitor pattern | 1.11 | 1.28 |

## jdk21（openjdk version "21.0.11" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% | os30 中位数 | os30 min~max | os30 p95 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|---|---|
| Total | 129 | 117~150 | 150 | 10% | 86 | 76~108 | 108 | 10.3% | 1.5 |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.699 | 11.6% | 1.105 | 10.5% | 1.54 |
| Class loading | 0.319 | 13.6% | 0.237 | 20.5% ⚠ | 1.35 |
| Collections and generics | 9.805 | 12.8% | 10.087 | 9.6% | 0.97 |
| Control flow | 0.169 | 11.4% | 0.114 | 8.7% | 1.48 |
| Crypto APIs | 48.666 | 18.9% ⚠ | 32.57 | 15.3% ⚠ | 1.49 |
| Enums | 9.336 | 12.9% | 1.589 | 12% | 5.88 |
| Exception handling | 0.85 | 18.7% ⚠ | 0.59 | 16.9% ⚠ | 1.44 |
| Inheritance and interfaces | 3.008 | 11.8% | 3.953 | 13.7% | 0.76 |
| Inner classes | 4.523 | 16.4% ⚠ | 3.742 | 22.4% ⚠ | 1.21 |
| Reflection and annotations | 39.405 | 10.4% | 19.959 | 9.2% | 1.97 |
| Resource loading | 1.101 | 13.8% | 2.888 | 8.9% | 0.38 |
| String and Unicode handling | 7.845 | 12% | 5.32 | 22.4% ⚠ | 1.47 |
| Visitor pattern | 4.404 | 20% ⚠ | 3.985 | 13.7% | 1.11 |

## jdk25（openjdk version "25.0.3" 2026-04-21 LTS）

### 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% | os30 中位数 | os30 min~max | os30 p95 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|---|---|
| Total | 120 | 110~128 | 128 | 5.1% | 89 | 74~111 | 111 | 12.1% | 1.35 |

### 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.458 | 20.7% ⚠ | 1.168 | 10.4% | 1.25 |
| Class loading | 0.214 | 15.5% ⚠ | 0.171 | 28.2% ⚠ | 1.25 |
| Collections and generics | 9.448 | 15.6% ⚠ | 8.276 | 19.6% ⚠ | 1.14 |
| Control flow | 0.173 | 12.4% | 0.149 | 20.5% ⚠ | 1.16 |
| Crypto APIs | 29.025 | 8.4% | 26.993 | 9.8% | 1.08 |
| Enums | 10.251 | 10.9% | 1.309 | 18.2% ⚠ | 7.83 |
| Exception handling | 0.978 | 13.6% | 0.639 | 16.9% ⚠ | 1.53 |
| Inheritance and interfaces | 2.456 | 16.3% ⚠ | 4.151 | 14.7% | 0.59 |
| Inner classes | 3.769 | 9% | 3.542 | 16.1% ⚠ | 1.06 |
| Reflection and annotations | 49.478 | 7.9% | 31.28 | 14.2% | 1.58 |
| Resource loading | 0.978 | 16.5% ⚠ | 2.864 | 14.6% | 0.34 |
| String and Unicode handling | 6.326 | 19.8% ⚠ | 5.573 | 17.3% ⚠ | 1.14 |
| Visitor pattern | 4.606 | 14.4% | 3.609 | 17.9% ⚠ | 1.28 |

## 方法论说明（issue #887 修尺子）

- 预热 3 轮（独立 JVM，不计入统计）+ 9 轮计量（独立 JVM 冷启动），多 JDK 时同一会话顺序完成。
- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。
- p95 为 nearest-rank 口径（9 轮中第 [ceil(0.95×9)] 位）；方差/标准差为无偏估计（n-1）。
- CV% = 标准差/均值；超过 15% 的行标记 ⚠，并在文首汇总为高方差存疑项。
- 原始样本与统计面同步落 CSV：results/ 下 *-runs.csv（逐轮明细）与 *-stats.csv（统计汇总）。
- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。

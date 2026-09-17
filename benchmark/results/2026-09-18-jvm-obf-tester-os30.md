# jvm-obf-tester 13-probe 对比（2026-09-18）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 3 轮（不计入统计）+ 9 轮计量；主指标=中位数，附 min~max 与 CV%
- 高方差判定：CV% > 15% 标记 ⚠ 并汇总为存疑项（#887 方法论）
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比；977303 字节，2026-08-11 16:16）

> **⚠ 高方差警告：结果存疑** — 以下样本 CV% 超过 15% 阈值，
> 不应作为打平/领先结论依据，建议提高 -Runs 或排查环境负载后重测：
> - RegC Crypto APIs (CV=18.2%)
> - RegC Inner classes (CV=20.1%)
> - RegC Reflection and annotations (CV=15.1%)
> - RegC Resource loading (CV=18.9%)
> - RegC String and Unicode handling (CV=25.3%)
> - RegC Visitor pattern (CV=17.2%)
> - os30 Exception handling (CV=30.4%)
> - os30 Inheritance and interfaces (CV=18.6%)
> - os30 Resource loading (CV=17.8%)
> - os30 Visitor pattern (CV=16%)

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC CV% | os30 中位数 | os30 min~max | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|
| Total | 106 | 96~125 | 10.8% | 95 | 92~108 | 5.4% | 1.12 |

## 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.5 | 12.4% | 0.94 | 6.2% | 1.6 |
| Class loading | 0.136 | 14.7% | 0.17 | 6.4% | 0.8 |
| Collections and generics | 7.002 | 14.2% | 6.816 | 6.5% | 1.03 |
| Control flow | 0.181 | 13.9% | 0.117 | 3.7% | 1.55 |
| Crypto APIs | 39.713 | 18.2% ⚠ | 29.194 | 11.1% | 1.36 |
| Enums | 7.325 | 11.1% | 13.374 | 9.4% | 0.55 |
| Exception handling | 0.889 | 15% | 0.645 | 30.4% ⚠ | 1.38 |
| Inheritance and interfaces | 11.21 | 14.4% | 8.182 | 18.6% ⚠ | 1.37 |
| Inner classes | 3.531 | 20.1% ⚠ | 4.272 | 9.4% | 0.83 |
| Reflection and annotations | 18.655 | 15.1% ⚠ | 15.787 | 9.2% | 1.18 |
| Resource loading | 0.677 | 18.9% ⚠ | 3.738 | 17.8% ⚠ | 0.18 |
| String and Unicode handling | 5.208 | 25.3% ⚠ | 3.504 | 4.8% | 1.49 |
| Visitor pattern | 6.182 | 17.2% ⚠ | 8.423 | 16% ⚠ | 0.73 |

## 方法论说明（issue #887 修尺子）

- 预热 3 轮（独立 JVM，不计入统计）+ 9 轮计量（独立 JVM 冷启动）。
- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。
- CV% = 标准差/均值；超过 15% 的行标记 ⚠，并在文首汇总为高方差存疑项。
- JDK 版本：java version "17.0.8" 2023-07-18 LTS。
- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。

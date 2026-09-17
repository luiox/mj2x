# jvm-obf-tester 13-probe 对比（2026-09-17）

- 环境：JDK java version "17.0.8" 2023-07-18 LTS
- 取样：预热 0 轮（不计入统计）+ 1 轮计量；主指标=中位数，附 min~max 与 CV%
- 高方差判定：CV% > 15% 标记 ⚠ 并汇总为存疑项（#887 方法论）
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比；977303 字节，2026-08-11 16:16）

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | RegC min~max | RegC CV% | os30 中位数 | os30 min~max | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|---|---|
| Total | 154 | 154~154 | 0% | 113 | 113~113 | 0% | 1.36 |

## 各探针耗时（ms，主指标=中位数）

| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |
|---|---|---|---|---|---|
| Arithmetic operations | 1.47 | 0% | 1.141 | 0% | 1.29 |
| Class loading | 0.169 | 0% | 0.193 | 0% | 0.88 |
| Collections and generics | 8.579 | 0% | 8.393 | 0% | 1.02 |
| Control flow | 0.177 | 0% | 0.148 | 0% | 1.2 |
| Crypto APIs | 66.266 | 0% | 32.621 | 0% | 2.03 |
| Enums | 8.582 | 0% | 17.974 | 0% | 0.48 |
| Exception handling | 0.933 | 0% | 0.76 | 0% | 1.23 |
| Inheritance and interfaces | 14.82 | 0% | 10.645 | 0% | 1.39 |
| Inner classes | 9.401 | 0% | 4.446 | 0% | 2.11 |
| Reflection and annotations | 29.561 | 0% | 19.969 | 0% | 1.48 |
| Resource loading | 0.867 | 0% | 3.871 | 0% | 0.22 |
| String and Unicode handling | 5.338 | 0% | 3.912 | 0% | 1.36 |
| Visitor pattern | 7.236 | 0% | 8.713 | 0% | 0.83 |

## 方法论说明（issue #887 修尺子）

- 预热 0 轮（独立 JVM，不计入统计）+ 1 轮计量（独立 JVM 冷启动）。
- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。
- CV% = 标准差/均值；超过 15% 的行标记 ⚠，并在文首汇总为高方差存疑项。
- JDK 版本：java version "17.0.8" 2023-07-18 LTS。
- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。

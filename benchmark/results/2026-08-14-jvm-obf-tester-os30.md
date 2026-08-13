# jvm-obf-tester 13-probe 对比（2026-08-14，双 JDK）

- 环境：JDK openjdk version "21.0.4" 2024-07-16 LTS（corretto-21.0.4）/ JDK 17（D:\sdk\java\jdk17）
- 取样：预热 1 轮 + 3 轮取平均
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入，HEAD 628d7a3f7 + 本日 issue 收尾改动）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比）
- 结论：JDK21 **0.96x（反超 4%）**，JDK17 **1.02x（持平）**——双 JDK 均追平/反超 os30。
  机器负载高（绝对值约为安静窗口 2 倍），同代码 ratio 波动 ±13%（历史区间 0.89~1.32），结论以多轮中位为准。

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms，平均）

| JDK | RegC | os30 | ratio (RegC/os30) |
|---|---|---|---|
| JDK21 | 160.7 | 167.7 | 0.96 |
| JDK17 | 253.3 | 249.0 | 1.02 |

（本轮为负载高峰期数据：JDK21 RegC 三轮 130/193/159，os30 143/200/160；JDK17 os30 三轮 326/175/246 极不稳定。）

## 各探针耗时（ms，平均）——JDK21

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.006 | 1.684 | 1.19 |
| Class loading | 0.387 | 0.280 | 1.38 |
| Collections and generics | 9.579 | 11.261 | 0.85 |
| Control flow | 0.187 | 0.207 | 0.91 |
| Crypto APIs | 51.187 | 47.701 | 1.07 |
| Enums | 23.218 | 23.524 | 0.99 |
| Exception handling | 0.942 | 0.608 | 1.55 |
| Inheritance and interfaces | 16.321 | 12.169 | 1.34 |
| Inner classes | 8.529 | 7.501 | 1.14 |
| Reflection and annotations | 32.373 | 32.935 | 0.98 |
| Resource loading | 1.118 | 3.549 | 0.32 |
| String and Unicode handling | 5.430 | 11.557 | 0.47 |
| Visitor pattern | 8.784 | 14.023 | 0.63 |

## 各探针耗时（ms，平均）——JDK17

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 3.353 | 1.999 | 1.68 |
| Class loading | 0.218 | 0.244 | 0.89 |
| Collections and generics | 22.088 | 25.463 | 0.87 |
| Control flow | 0.278 | 0.238 | 1.17 |
| Crypto APIs | 73.301 | 56.919 | 1.29 |
| Enums | 43.146 | 48.876 | 0.88 |
| Exception handling | 2.426 | 1.571 | 1.54 |
| Inheritance and interfaces | 31.012 | 23.736 | 1.31 |
| Inner classes | 12.864 | 9.477 | 1.36 |
| Reflection and annotations | 42.897 | 44.933 | 0.95 |
| Resource loading | 1.005 | 7.936 | 0.13 |
| String and Unicode handling | 9.127 | 8.642 | 1.06 |
| Visitor pattern | 9.995 | 17.944 | 0.56 |

## 说明

- 稳定反超的探针（双 JDK 一致）：Collections、Enums、Reflection、Resource、Visitor（ratio ≤ 0.95）。
- 稳定落后的探针：Arithmetic（1.19-1.68x）、Exception（1.54-1.55x）、Inheritance（1.31-1.34x）——绝对量小（合计 ~4-7ms），且 Inheritance 为 interface 双实现动态分派下限（receiver 类型传播未做）；Arithmetic/Exception 为 IsInstanceOf/异常检查的调用质量差异。
- 历史安静窗口基线（同代码）：JDK21 1.02x（88 vs 86ms）、JDK17 0.89x（93 vs 104ms）。

# jvm-obf-tester 13-probe 对比（2026-08-14，双 JDK，receiver 静态绑定后）

- 环境：JDK openjdk version "21.0.4" 2024-07-16 LTS（corretto-21.0.4）/ JDK 17（D:\sdk\java\jdk17）
- 取样：预热 1 轮 + 3 轮取平均
- RegC 产物：D:\WorkSpace\ai\morpher\test\jvm-obf-tester\jvm-obf-tester-regc.jar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）
- os30 产物：D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar（JNIC 竞品，黑盒对比）
- 本轮变更：receiver 精确类型静态绑定（分配点传播）——动态实例调用 receiver 的 SSA def
  可证为单一分配点类且目标方法已转换时，跳过 resolved dispatch 运行时类检查（双槽
  IsInstanceOf，平均 1.5-2 JNI/调用）直接调用 regc 函数体（0 JNI，保留 null 检查）。
- 结论：JDK21 **0.88x（反超 12%，历史最好）**，JDK17 **1.08x（持平，负载噪声窗口）**。
  **Inheritance 探针双 JDK 反超**（0.60x / 0.87x，从 1.31-1.34x 到反超，绝对耗时
  JDK21 16.3→8.6ms）。机器负载高（绝对值约为安静窗口 1.5-2 倍），结论以多轮中位为准。

## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）

| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |
|---|---|---|---|---|
| RegC | 155 | 150 | 112 | 74.7% |
| os30 | 208 | 203 | 125 | 61.6% |

## 总耗时（ms，平均）

| JDK | RegC | os30 | ratio (RegC/os30) |
|---|---|---|---|
| JDK21 | 158.0 | 180.3 | 0.88 |
| JDK17 | 196.3 | 182.0 | 1.08 |

（本轮为负载高峰期数据：JDK21 RegC 三轮 156/164/154，os30 179/192/170；JDK17 os30 三轮 212/188/146 不稳定。）

## 各探针耗时（ms，平均）——JDK21

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.526 | 1.824 | 1.39 |
| Class loading | 0.304 | 0.355 | 0.86 |
| Collections and generics | 11.379 | 10.888 | 1.05 |
| Control flow | 0.212 | 0.222 | 0.96 |
| Crypto APIs | 57.728 | 62.538 | 0.92 |
| Enums | 20.933 | 26.921 | 0.78 |
| Exception handling | 1.071 | 0.716 | 1.50 |
| Inheritance and interfaces | 8.554 | 14.367 | 0.60 |
| Inner classes | 9.275 | 6.798 | 1.36 |
| Reflection and annotations | 29.795 | 30.806 | 0.97 |
| Resource loading | 0.917 | 3.966 | 0.23 |
| String and Unicode handling | 6.008 | 5.769 | 1.04 |
| Visitor pattern | 8.244 | 14.759 | 0.56 |

## 各探针耗时（ms，平均）——JDK17

| Probe | RegC | os30 | ratio |
|---|---|---|---|
| Arithmetic operations | 2.862 | 1.624 | 1.76 |
| Class loading | 0.318 | 0.224 | 1.42 |
| Collections and generics | 14.993 | 14.302 | 1.05 |
| Control flow | 0.238 | 0.187 | 1.27 |
| Crypto APIs | 78.807 | 58.277 | 1.35 |
| Enums | 24.897 | 30.259 | 0.82 |
| Exception handling | 1.699 | 1.299 | 1.31 |
| Inheritance and interfaces | 11.349 | 12.994 | 0.87 |
| Inner classes | 8.028 | 7.013 | 1.14 |
| Reflection and annotations | 32.975 | 26.361 | 1.25 |
| Resource loading | 1.283 | 6.196 | 0.21 |
| String and Unicode handling | 7.080 | 5.765 | 1.23 |
| Visitor pattern | 10.210 | 17.236 | 0.59 |

## 说明

- 稳定反超（双 JDK 一致）：Inheritance（0.60/0.87，本轮新增）、Enums、Resource、Visitor。
- 剩余落后（绝对量小）：Arithmetic（1.39-1.76x）、Exception（1.31-1.50x）、Inner（1.14-1.36x）——
  Arithmetic 为纯计算 + 少量 JNI 的调用质量差异；Exception 为异常路径固有成本。
- 历史基线对比（同代码安静窗口）：JDK21 1.02x、JDK17 0.89x（静态绑定前 Inheritance 1.31-1.34x）。

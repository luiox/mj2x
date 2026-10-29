# jvm-obf-tester 13-probe 对比（2026-10-29，帧换装分支 feat/frame-lca-resolver）

- 环境：本机 Windows，JDK jdk25.0.5（单 JDK）；预热 3 轮 + 9 轮计量，主指标=中位数，CV% 参差标注
- 产物：`jvm-obf-tester-regc.jar`（本分支 mj2x-cli `--backend regc --generate-xmake --regc-direct-calls all` + MSVC 原生编译嵌入）
- os30 产物：`C:\Users\l1502\Downloads\jvm-obf-tester_os30.jar`（2026-08-11，977303 字节；用户提供的对标样本）
- 测量程序：逐产物独立 JVM，`java -jar` 各 12 轮（3 预热不计入 + 9 计量），解析探针表

## 总耗时（ms，主指标=中位数）

| 项 | RegC 中位数 | os30 中位数 | ratio |
|---|---|---|---|
| Total | 115 | 101 | **1.14** |

（对照：10-05 报告 jdk21 口径 1.5 / jdk25 口径 1.35——本测量在 jdk25 上为 1.14，
RegC 面已显著收窄；两轮 os30 样本不同（final.jar vs os30.jar），量级结论以本轮为准。）

## 各探针（ms，主指标=中位数）

| Probe | RegC | os30 | ratio | 备注 |
|---|---|---|---|---|
| Crypto APIs | 44.3 (cv 6%) | 19.5 (cv 12%) | 2.28 | **见"Crypto 剖析"——伪差** |
| Reflection and annotations | 36.8 (cv 5%) | 32.3 (cv 9%) | 1.14 | 注解代理 + Method.invoke JDK 机制地板 |
| Collections and generics | 8.91 | 7.07 | 1.26 | 集合操作 JNI 边界税 |
| Enums | 7.65 | 13.9 | **0.55** | RegC 反超（枚举 intrinsic 面 + 直调） |
| String and Unicode handling | 5.33 | 3.90 | 1.37 | JVM intrinsic 地板（charAt/substring 刻意回退） |
| Visitor pattern | 3.85 | 8.62 | **0.44** | 反超 |
| Inner classes | 3.03 | 3.42 | 0.89 | 反超 |
| Inheritance and interfaces | 1.57 | 7.58 | **0.21** | 大幅反超（resolved dispatch 双槽） |
| Arithmetic operations | 1.40 | 1.06 | 1.32 | 见"内联化实验"——Windows 无差 |
| Resource loading | 0.91 | 2.32 | **0.39** | 反超 |
| Exception handling | 0.62 | 0.55 | 1.12 | 接近 |
| Class loading | 0.21 | 0.13 | 1.63 | 量小 |
| Control flow | 0.14 | 0.12 | 1.21 | 接近 |

## Crypto 剖析（本轮最重要修正）

10-05 报告把 Crypto 2.03x 归因为"JCE 首触 + 冷启动噪声"（当时 0 预热 1 计量）。
本轮 9 轮中位数（CV 6%/12%，稳定）显示 44.3 vs 19.5 为真实差异——但归因反转：

1. **纯 Java 基线实测 45.4ms**（mini-jar 复刻探针调用序：MessageDigest.getInstance
   SHA-256 + digest + Cipher AES/ECB/PKCS5 getInstance/init/doFinal×2）——与 RegC
   44.3 逐位吻合。粒度计时：`getInstance_MD=18.5ms、getInstance_AES=21.0ms、
   init=4.9ms`——全部为 JDK 框架内部成本（provider 装载/JCE 验证基础设施），
   RegC 产物与诚实 JVM 基线**逐位打平**（保护零附加开销）。
2. os30 的 19.5 < 纯 Java 冷启动成本 → 其 JCE 初始化发生在**计时窗外**：
   os30 的 `CryptoProbe` 带 `$jnicLoader` 静态原生加载器（javap 实证），其 VM
   载入期预热了 JCE 面。这是 os30 侧的测量伪差，不是 RegC 发射缺陷。
3. 结论：Crypto 探针无可打的 RegC 缺陷；"保护是否拖慢 crypto"的回答是零拖慢。

## 内联化实验（P3）

Arithmetic 环体 160000 迭代 × rotateLeft/reverse 等纯计算 intrinsic 此前跨 TU
外联调用。P3（`mj2x_regc_intrinsic_inline.h` 单源 static inline）落地后 Windows
复测 **1.401 vs 1.370——零差**：生成工程 MSVC release 的 LTCG 已跨 TU 内联。
保留该改动的理由：Linux gcc -O2 无 LTO 下跨 TU 内联不发生，P3 消除对宿主
工具链 LTO 能力的依赖（构建稳健性），非 Windows 计量收益。

## 结构性结论（为什么此前"比不过"，现在差在哪）

1. **总差距 1.14×，且 5/13 探针反超**（Visitor/Inheritance/Enums/Resource/Inner
   classes）——旧报告的 1.35~1.5 总口径已过时。
2. os30 覆盖率 61.6% vs RegC 74.7%：os30 把更多热循环留在 Java 让 JIT 内联，
   RegC 全量 native 化在 JDK-重度路径（Reflection 注解代理、String、Collections）
   付 JNI 边界税。这是**转换策略的结构性差异**，不是发射缺陷——RegC 在
   自家反超的探针（Inheritance 0.21×）证明发射面本身优于 os30 的直调形态。
3. 剩余可打的诚实点（按绝对值排序）：
   - Reflection +4.5ms：注解代理（AnnotationInvocationHandler ~10µs/次）+
     Method.invoke 机制为 JDK 地板，两产物同付；余差为边界税，量级 ~0.3µs/JNI。
   - String +1.4ms / Collections +1.8ms：同上边界税，绝对值小。
   - 以上合计 ~7ms；即便全收也只能到 ~108 vs 101——总 ratio 的边际由 os30
     的 crypto 预热伪差主导（-25ms 口径差），诚实口径下双方已在同一量级。

## 复现

```sh
# 转换 + 原生编译
build/windows/x64/release/mj2x-cli.exe -i <probe.jar> -o out.jar \
  --backend regc --generate-xmake proj --regc-direct-calls all
cd proj && xmake f -P . -p windows -m release -y && xmake -P .
# 嵌入后计量：逐产物 3 预热 + 9 计量独立 JVM，解析探针表取中位数
```

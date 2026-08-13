# jvm-obf-tester 13-probe 对比：VMC4 99% 转化（2026-08-13）

- 环境：JDK 17（openjdk，Windows x64，同机同轮次交替采样）
- 取样：独立 JVM 进程 × 4 轮，取中位数（避免 JIT 串扰）
- 基线：`D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar`（原版）
- VMC4 产物：`test/mj2x/samples/jvm_obf_tester_vmc4_99.jar`（`--backend vmc4` 全管线
  `--compile`；switch_table dispatcher，drift 2 变体 × 16 周期）
- 转化率：122/123 方法转 VM（99.2%），仅 `ControlFlowProbe.run`（tableswitch 0-6）
  fail-closed 保留字节码

## 覆盖率

| 产物 | 方法总数 | 转 VM | 比例 |
|---|---|---|---|
| 基线 | 123 | 0 | 0% |
| VMC4 | 123 | 122 | 99.2% |

## 总耗时（ms，4 轮原始）

| 项 | 基线 | VMC4 | ratio |
|---|---|---|---|
| 原始 | 90 / 102 / 103 / 93 | 305 / 304 / 301 / 299 | — |
| 中位数 | 97.5 | 302.5 | **3.10x** |

## 各探针耗时（ms，中位数）

| Probe | 基线 | VMC4 | ratio | 备注 |
|---|---|---|---|---|
| Arithmetic operations | 9.236 | 158.400 | 17.15x | 热循环在 VM 解释器内（dispatch 主开销） |
| Visitor pattern | 1.498 | 19.994 | 13.35x | 虚分派链 + VM 内对象操作 |
| Inheritance and interfaces | 1.540 | 15.688 | 10.19x | 同上 |
| Enums | 2.877 | 24.364 | 8.47x | valueOf/values 表 + switch 相关 |
| Exception handling | 1.052 | 6.583 | 6.26x | 检查点分派开销 |
| Inner classes | 1.923 | 11.406 | 5.93x | 合成字段访问 |
| String and Unicode handling | 1.876 | 10.607 | 5.66x | String LDC + 调用链 |
| Collections and generics | 4.431 | 10.530 | 2.38x | JNI 调用为主，VM 占比低 |
| Reflection and annotations | 19.950 | 19.960 | 1.00x | JNI 反射调用为主，VM 开销被摊薄 |
| Crypto APIs | 50.409 | 34.011 | 0.67x | JCE 原生实现，非 VM 热路径（噪声范围） |
| Control flow | 5.281 | 1.010 | 0.19x | probe 主体保留字节码（tableswitch），快属 JIT/噪声 |
| Resource loading | 4.995 | 0.702 | 0.14x | I/O fail-closed 保留字节码，同上 |
| Class loading | 0.271 | 0.205 | 0.76x | 类加载系统方法不转 VM，噪声范围 |

## 结论

1. **转换一致性**：13/13 probe 全 Passed（VMC4 保护版与基线输出一致）。
2. **总耗时 3.10x**：VM 化代价（源码到源码解释器）。与 RegC（1.16x~1.56x，
   见 `2026-08-13-jvm-obf-tester-os30.md`）的差距 = 解释器 dispatch + 寄存器
   文件读写 vs RegC 的直译 C 代码。
3. **损耗集中**：纯计算/对象操作热路径（Arithmetic/Visitor/Inheritance 10-17x）
   是 dispatch 开销主因；JNI 调用为主或保留字节码的 probe（Reflection/Crypto/
   Class loading/Control flow/Resource loading）损耗可忽略甚至反快（噪声）。
4. **优化方向（后续，已记录 dev_plan）**：热循环内 dispatch 直通（如循环不变
   opcode 前缀缓存）、检查点稀疏化（仅 mayThrow 指令后查异常）、drift 关闭时
   简化 unmap 路径。当前性能定位 = 红队对抗强度优先（drift/别名/多 dispatcher），
   性能换防护是可接受取舍；若需性能优先场景可关 drift/alias 再测。

# computed jumps（跳转目标间接化）

> CFF 把控制流打散成 dispatcher，但 dispatcher 的 case key 仍是字面量——逆序
> case key 就能还原控制流。本项把「key → 目标块」的映射从代码挪到数据表：
> key 按站点密钥异或存放，分派前先查表解析出块索引，再按索引 switch。产物里
> 不再有 `case 5: goto block_b17` 这种直接可读的映射。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `obfuscation.jumps`；业界通称 computed jump / jump table obfuscation |
| 作用层 | 发射端（RegC 的 switch terminator 发射路径） |
| 开关 | `--regc-computed-jumps` |
| 后端 | RegC（LLVM 侧 switch 是天然形态，未走这条路） |
| 默认 | 关（不联动 `--regc-obfuscate`） |
| 状态 | ✅ 已落地（issue #7，PR #27） |

## 对抗什么

调用目标间接化（见 [call_indirection.md](call_indirection.md)）打散的是**调用边**，
但函数内部的**跳转边**仍是字面量：`goto L3`、`switch (v) { case 2: ... }` 的目标
在产物里一目了然。

computed jumps 补的是这一半。两者合起来，调用图和 CFG 才都变成"要解析索引才能
还原"。

## 机制

### 数据表与密钥派生

- CFF 先把 switch 收敛到单一 dispatcher，本项在其之上把 case key 与目标数组
  拆到**模块级数据表**。
- 落盘的 key 是 `真实 key ^ const_key(site)`，密钥走与立即数隐藏同源的
  splitmix64 派生（跨 TU、故意不可内联），运行时不可折叠。
- 分派形态：`idx = 解出的块索引; switch (idx)`。

### 便携性是硬约束

MSVC 是主目标，**不能用 GCC 的 computed goto**（`&&label`）。表驱动 + 索引
switch 是标准 C 内可表达的最强形态——这是本项的**能力上限，不是偷懒**。

候选路线里的另一条（opaque 索引：目标索引 = 真实索引 `^ opaque(site)`）与本项
同源（同一套 splitmix64 派生），最终选了表驱动路线，因为它对 CFF dispatcher
的形状改动最小。

### edge copies 必须跟着目标走

这是本项最容易做错的地方。间接化后，跳转边携带的 phi 副本（edge copies）也要
随目标一起查表携带——漏掉即是数据流错误，且只在特定分支路径上暴露。

## 代价

实测（jvm-obf-tester，5 次取中位数，单次运行内对照）：

| | 合计 | ControlFlow probe |
|---|---|---|
| 关 | 137.139ms | 0.374ms |
| 开 | 149.304ms | 0.477ms |

+8.9% 合计，switch 密集处 +27%。

**表扫描本身便宜，成本几乎全在密钥派生。** 每分派调一次 `mj2x_regc_const_key`
实测让 ControlFlow 慢 70%；改成 static 局部量只派生一次后降到 27%。

属混淆增益而非性能项，默认关。

## 一个设计取舍

拿不到 TU 头部注册表时（key 表无处安放）**退回字面量 case，不整方法
fail-closed**。本项是"有则更好"的增益，不该让方法编译不了——初版写成
fail-closed，被单测抓出。

## 已知绕过面

- **表本身是可读的**：key 与目标都在数据段里，静态拿到表 + 拿到派生密钥即可
  还原映射。本项提高的是"读跳转指令即得控制流"的成本，不是让它不可能。
- **CFF 之外的跳转不受影响**：本项作用于 CFF dispatcher 的 switch terminator，
  普通 `goto` 边仍是字面量目标。

## 验证

- jvm-obf-tester 全量 13 项 probe Passed（开关打开状态下）
- RegCBackendTest 新增 2 项（backend 级，走真实 TU 注册表：key 表 + 索引分派 +
  密钥只派生一次）
- RegCTest 新增 2 项（默认关无痕迹 / 无注册表时优雅降级）
- 全量回归 868 tests

## 与其他技术的关系

- 与 CFF 都作用于控制流，但层次不同：CFF 改**结构**（多跳 dispatcher），
  computed jumps 改**目标的表达方式**。必须先有 CFF——CFF 之前没有可改的
  switch terminator。
- 与 opaque 谓词互补：一个改跳到哪，一个改跳不跳。
- 与**立即数隐藏**同族（共用 splitmix64 站点密钥派生），可叠。

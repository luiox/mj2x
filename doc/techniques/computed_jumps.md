# computed jumps（跳转目标间接化）

> 把跳转/分支的目标从"字面量基本块"改成"运行时计算的索引"，让块间关系不能靠读
> 跳转指令直接得出。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `obfuscation.jumps`；业界通称 computed jump / jump table obfuscation |
| 作用层 | RegIR pass（拟）→ 发射端 |
| 开关 | 拟 `--regc-computed-jumps`（默认关，不联动 `--regc-obfuscate`） |
| 后端 | 拟 RegC / LLVM |
| 默认 | 关 |
| 状态 | 📋 **未实现**（issue #7） |

## 对抗什么

调用目标间接化（见 [call_indirection.md](call_indirection.md)）打散的是**调用边**，
但函数内部的**跳转边**仍然是字面量：`goto L3`、`switch (v) { case 2: ... }` 的
目标在产物里一目了然，块间关系直接可读。

computed jumps 补的是这一半。两者合起来，调用图和 CFG 才都变成"要解析索引才能
还原"。

## 现状与阻碍

`RegBranchTerminator` / `RegSwitchTerminator` 的 successor 是 `RegSuccessor*`
直指针，目标编译期恒定。CFF 已经把 switch 收敛到单一 dispatcher，但 case key
仍是字面量——逆序 case key 就能还原控制流。

## 候选路线（实现前需先做 spike）

| 路线 | 做法 | 阻碍 |
|---|---|---|
| 跳转表间接化 | case key 与 target 数组拆到模块级表，terminator 改为 `idx = f(state); goto table[idx]` | MSVC 不支持 GCC 的 `computed goto`（`&&label`），需降级为 `switch (table[idx])` 或函数指针表；后者带来函数指针表的符号可见性问题 |
| opaque 索引 | 目标索引 = 真实索引 `^ opaque(site)`，分派前还原 | 与立即数隐藏同族（编译期密钥 + 运行时不可折叠读取），可复用 `const_key_derivation.hpp` 的 splitmix64 派生 |

## 约束（设计前先记下的硬条件）

- **MSVC 是主目标**，产物必须落在 C 标准子集内，不能依赖 GCC 扩展。
- **必须排 CFF 之后**——CFF 之前没有可改的 switch terminator。
- **edge copies 必须跟着目标走**。间接化后 copies 也要查表携带，漏掉即数据流错。
  这是本项最容易做错的地方。
- LLVM 子集：`switch` 是天然形态，但 `getelementptr` + `indirectbr` 需先验证
  现有验证层与发射层是否支持。
- 语义零变化，E2E 必须真实编译 + 运行比对。

## 验收标准

- 新开关（默认关，不被 `--regc-obfuscate` 联动）
- RegIR 单测：标记语义 / 与 CFF 叠加后 dispatcher 表完整性 / 可复现性
- E2E：真实编译 + 运行，输出与未变换一致
- `doc/user_guide.md` + `doc/config_file.md` 同步选项说明

## 与其他技术的关系

- 与 CFF 都作用于控制流，但层次不同：CFF 改**结构**（多跳 dispatcher），
  computed jumps 改**目标的表达方式**。
- 与 opaque 谓词互补：一个改跳到哪，一个改跳不跳。

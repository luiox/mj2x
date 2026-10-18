# safe-env（JNI 函数表保护）

> 快照 JNI 函数表，每个受保护方法入口恢复一次——让红队在 JNI 表上插的 hook 在
> 进入受保护代码后失效。

| 项 | 值 |
|---|---|
| 对标术语 | 无通用业界名；本仓内称 safe-env |
| 作用层 | 发射端（入口恢复）+ 运行时（快照/fail-closed） |
| 开关 | `--regc-safe-env` / `--regc-safe-env-debug` |
| 后端 | RegC ✅ / StackC ❌ / LLVM ❌ / VMC4 ❌ |
| 默认 | 关 |
| 状态 | ✅ 已落地（#748，PR #754/#768） |

## 对抗什么

JNI 函数表（`JNINativeInterface_`）是 native 层最常被 hook 的面：改一个
`CallObjectMethod` 的槽位就能记录所有 JNI 调用、改一个 `FindClass` 就能监控类
加载。整个 native 层的可观测性基本建立在这张表上。

mjdbg 侧有 JNI-hook 分析工具（#756）作常态化对手；safe-env 需要这个常设攻击
对手才能成为可回归的 E2E 指标。

## 机制

1. **捕获**：`JNI_OnLoad` 时对当前线程的 JNI 函数表做快照。
2. **恢复**：每个受保护方法入口，把表恢复成快照内容。

于是 hook 的生效窗口被压到"进入受保护方法之前"——受保护代码内部的 JNI 调用走
的是干净表。

### fail-closed 与调试模式

**捕获时表已污染则 `JNI_OnLoad` fail closed**——这是关键设计。若快照本身就是
被 hook 过的表，恢复只会把 hook 固化成"干净态"，防护反而变成帮凶。宁可加载失败
也不带着污染快照继续。

`--regc-safe-env-debug` 是调试观察模式：检出 hook 只打告警不 fail（隐含开启
`--regc-safe-env`），用于排查"到底是谁在 hook"。

## 代价

| 维度 | 影响 |
|---|---|
| 性能 | 每方法入口一次表恢复（memcpy 量级） |
| 兼容性 | **fail-closed 会让合法环境加载失败**。任何正常 hook（APM、监控 agent、调试器）都会触发 |
| 覆盖面 | 只保护本线程 / 本 env 的表；调用方在外部做的观测不受影响 |

兼容性这一条是它默认关的原因：真实用户环境里 JNI 表被第三方 agent 改动并不罕见。

## 已知绕过面

- **在恢复之后 hook**：hook 方若在受保护方法入口之后再改表（例如在被调用的
  Java 方法里），恢复不覆盖。
- **绕过 JNI 直接调实现**：若攻击者拿到 `JNINativeInterface_` 背后真实函数地址，
  不查表也能调。
- **非 JNI 观测面**：native 断点、内存扫描、ETW 等不走 JNI 表，safe-env 不覆盖。

## 与其他技术的关系

- 与 **运行时反侦测检查点**（[runtime_anti_debug.md](runtime_anti_debug.md)）互补：
  一个护 JNI 表，一个查调试器/JVMTI。两者可叠。
- 与 **native→native 直调**同向：直调减少了 JNI 边界数量，safe-env 保护剩余的
  边界。

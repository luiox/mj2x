# 立即数隐藏（Constant Encryption / 常量加密）

> 整型立即数 C 不再以 `C` 出现在产物里，改成 `E ^ const_key(site)`：E 编译期
> 物化，密钥 K 运行时派生，两者 XOR 才回到 C。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `obfuscation.constants` / `.constants.mba`；业界通称 constant encryption |
| 作用层 | RegIR pass（`applyConstantHiding`）+ 运行时（mj2x-rt） |
| 开关 | `--regc-hide-constants`（**不被 `--regc-obfuscate` 联动**） |
| 后端 | RegC ✅ / LLVM ✅ / StackC ❌ / VMC4 ❌ |
| 默认 | 关 |
| 状态 | ✅ 已落地 |

## 对抗什么

常量是最强的语义锚点：`0x5EED`、`1000`、`0xFFFFFFFF` 这类值直接暴露算法
（魔数、掩码、阈值、块大小）。反编译后按常量搜一遍，算法定位就完成了一半。

## 机制

路线只有一条可行，其余都会被编译器折掉：

```text
C  →  E ^ K
      E = C ^ K         编译期算出，作为立即数物化在产物里
      K = splitmix64(site ^ opaque)   运行时派生，产物里只有 site
```

产物里出现的是 `E` 和一个 `site` 编号，**没有 C，也没有 K**。

### 为什么不能用纯字面量恒等式

两条死路，都踩过：

- `(C ^ k) ^ k` 同值异指令：两个 XOR 会被重结合折叠，结果就是 `C`。
- 任何编译期可求值的恒等式：常量传播直接折回 `C`。

唯一可行的是「**编译期密钥 + 运行时不可折叠读取**」，同 OLLVM 的 constant
encryption 路线。

### 运行时不可折叠怎么保证

`mj2x_regc_const_key` / `mj2x_llvm_const_key` 的 splitmix64 单步，输入先掺一个
`static volatile` 不透明常量。volatile 读迫使每次真实加载，LTO 下的跨 TU
IPSCCP / 常量传播无法把 `const_key(site)` 折成常量——否则 `E ^ K` 被折回 `C`，
整个变换白做。`--native-opt-level` 的 LTO 档同样守得住。

### 双端同源是硬约束

pass 编译期算 E 用的公式（splitmix64 正典 + `kOpaqueConstant`）与 RT 侧 C 实现
**逐位同源**。改任何常数 / 移位 / 顺序都会使 `E ^ K != C`，产物直接算错。这不是
参数调整，是不兼容变更，两端必须同步改。跨语言一致性由 E2E 真实编译 + 运行守。

### site 不掺 seed

site 是函数内局部计数器（0,1,2,...），**不掺 transformSeed**。理由：site 作为调用
参数必然物化在产物里，掺 seed 只增大立即数位宽，不增加分析者成本——读 site 后
同样一步就派生出 K。这项定位是抗静态分析，不是密码学强度。

## 产物形态

```c
// 变换前
r1 = r0 + 0x5EED;

// 变换后
r2 = mj2x_regc_const_key(3);        // K，运行时派生
r3 = r2 ^ 0x...;                    // E，编译期物化的 C ^ K
r1 = r0 + r3;
```

I32 会追加 `Convert` 截断回 32 位；I64 不加（多余截断会破坏宽常量）。

## 顺序纪律

**必须排 CFF 之后**。`isCffPureInstruction` 对 `Intrinsic` 返回 false，若先插入
合成 Intrinsic 会杀死平坦化（零交互保证）。反过来 CFF 的 case key 不迭代
（dispatcher 表完整性），保持字面量。CFF 之前各 pass 生成的字面量（opaque 谓词
的 `1`、state 码）随本 pass 一并隐藏，语义不变。

合成 Intrinsic 留 `SSAEffect::pure()`（无 mayThrow 不触发 ExceptionCheck，非
Call 不进 local ref 统计），与原立即数的效果面一致。

## 代价

| 维度 | 影响 |
|---|---|
| 性能 | **混淆家族里最重的一项**：每个整型立即数多一次运行时调用 |
| 产物体积 | 每常量多两条指令 + 一次调用 |
| 兼容性 | 双端公式是硬绑定，改一端即错 |

这也是它不被 `--regc-obfuscate` 联动、必须显式开启的原因。

## 已知绕过面

- **动态观察**：运行时 K 就在寄存器里，`E ^ K` 的结果必出现。对抗的是静态。
- **volatile 被去掉**：密钥函数若可内联或 volatile 被优化掉，整条链折回 `C`。
- **同源公式可读**：splitmix64 是公开算法，分析者照抄一遍就能自己算 K。这一项
  防的是"扫常量"，不是"防复现算法"。

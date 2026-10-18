# CFF 控制流平坦化（Control Flow Flattening）

> 把结构化的 if/loop 打散成"单一 dispatcher + state 变量"的状态机，让控制流图
> 退化成一个大 switch，看不出原来的块间关系。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `obfuscation.cff`；业界通称 CFF / control flow flattening |
| 作用层 | RegIR pass（`applyControlFlowFlattening`） |
| 开关 | `--regc-obfuscate` / `--regc-cff-dead <n>` / `--regc-cff-state-encrypt` |
| 后端 | RegC ✅ / LLVM ✅（超子集则整方法回退）/ StackC ❌ / VMC4 ❌ |
| 默认 | 关 |
| 状态 | ✅ 已落地 |

## 对抗什么

反编译器的控制流恢复依赖"块的后继是静态可数且有序的"。平坦化后所有块都从同一
个 dispatcher 分发出去，块间关系被 state 变量的取值间接决定——要还原就得先解出
state 的转移表，而不是读 CFG 边。

## 机制

**基础形态**：eligible 的纯 CFG 边替换为 state-dispatcher 转移。每个原基本块
末尾写下一个 state 值，跳回 dispatcher；dispatcher 按当前 state `switch` 到目标块。

**eligible 的判定很严**：函数带有异常路径、内存访问、调用、barrier、throw
terminator 或畸形 CFG 目标时**整函数排除**。这是保守的正确性取舍——宁可不平坦
化，不能改坏语义。

**两个增强**：

- `--regc-cff-dead <n>`：每 N 个 dispatcher 跳转注入 1 个恒假死块。死块条件
  （如 `(state*state+1)==0` 模 2³²）无解恒假，但**依赖运行时 state**，编译器
  无法证明，因此不会被 DCE。块内夹 barrier 进一步防删。
- `--regc-cff-state-encrypt`：state 码由 `splitmix64(seed, index)` 派生（不是
  顺序的 1,2,3），且 dispatcher 入口对 state 做异或（case 常量与写入值不同）。
  去掉这层后，state 码的连续性本身就是还原线索。

## 一个不能省的细节：state 必须 volatile

`cffStateVolatile` **默认开**。原因是实测过的：state 若可跨基本块常量传播，
clang -O2 下 dispatcher 的 `switch` 会被 jump-threading 完全消解，平坦化只剩源码
可读性收益、运行时一点用都没有。关掉它的唯一理由是省下 volatile 读写开销——
接受"平坦化只对抗静态分析"这个降级定位时才可以关。

## 产物形态

```c
// 变换前
if (c) { A(); } else { B(); }
C();

// 变换后（示意）
int state = 1;
for (;;) {
    switch (state) {
    case 1: state = c ? 2 : 3; break;
    case 2: A(); state = 4; break;
    case 3: B(); state = 4; break;
    case 4: C(); return;
    }
}
```

## 代价

| 维度 | 影响 |
|---|---|
| 性能 | **最重的一项**：每个块边界多一次 switch 分派；volatile state 阻断优化 |
| 产物体积 | 显著膨胀（dispatcher + 死块） |
| 覆盖面 | 有异常/调用/内存访问的函数直接排除，实际命中率低于直觉 |

## 已知绕过面

- **动态 trace**：跑一遍记录 state 转移序列，CFG 就还原了。CFF 对抗的是静态。
- **编译器消解**：state 非 volatile 时 -O2 直接消掉（见上）。
- **state 码连续**：不开 `state-encrypt` 时 1,2,3 的顺序自带还原线索。

## 与其他技术的关系

- **立即数隐藏必须排其后**（`isCffPureInstruction` 对 Intrinsic 返回 false），
  但 dispatcher 的 case key 本身保持字面量——分发表完整性优先。
- 与 opaque 谓词互补：一个改结构，一个改条件。

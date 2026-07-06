# mj2x SSA 构造算法（block-argument SSA + 自写 reaching-def）

> 状态：R1a 实现 note，权威随代码演进
> 日期：2026-06-25
> 关系：本文档定义 `ssa_builder` 的构造算法。
> IR 结构定义见 `IR_DESIGN.md`。
> 决策依据：`dev_plan.md` R1-A（block-arg SSA）、R1-B（自写 reaching-def 兜底）。
>
> 本文只讲"怎么从 L1 MIR 构造出 SSA"；"IR 长什么样"在 `IR_DESIGN.md`。

---

## 1. 为什么不用经典 SSA 算法

经典 SSA 构造（Cytron 等的 dominance + iterated dominance frontier + φ 插入 + 变量重命名）需要：
1. Lengauer-Tarjan 支配树计算。
2. 支配边界（DF）计算。
3. 在 DF 集合上迭代放 φ。
4. 支配树 DFS 重命名变量。

这套对一个"先把栈语义消除掉"的目标来说**重了**，而且容易写出 bug。mj2x 改用 **block-argument SSA**（WebAssembly / MLIR / Grunt `JvmSSAImporter` 都用），它把 φ 隐式编码在块的入口参数里，**不需要支配树、不需要显式 φ 插入、不需要重命名**。

代价是 SSA value 稍多（block args），但对 R1 目标（消除 cstack 锚点 + 给 L4/L5 一个干净的 IR）完全够用，且实现简单、易测。

---

## 2. 算法总览（三阶段）

```
L1 Block[]  (CodeConverter 产物，含 MIRNode + predecessors/successors)
   │
   ▼
[阶段 A] reaching-def 数据流         ← 自写，零 libjbytecode 依赖
   │   求：每个块入口处，每个 slot(local/stack) 的 def 集合
   │   用途：(1) 决定每个块需要几个 block arg
   │         (2) 决定前驱传值时取哪个 SSA value
   ▼
[阶段 B] 块结构搭建 + 入口 frame shape
   │   按 reaching-def 结果给每块开 args（≥2 源的 slot = 一个 arg）
   │   建 SSAFunction / SSABlock[] / entry 合成块
   ▼
[阶段 C] 逐块翻译（FrameState 模拟栈）
   │   维护 builder-local FrameState{locals[], stack[]}，逐 MIRNode 翻译：
   │     - 栈操作(DUP/SWAP/...) → 只动 FrameState，不产 IR
   │     - LocalLoad/Store → reaching-def 给值
   │     - 算术/调用/... → 产 SSAInstruction，push result 到 FrameState
   │   terminator 处把 FrameState 里"后继块需要的 slot"作为 successor.args 传出
   ▼
SSAFunction（block-argument SSA）
```

---

## 3. 阶段 A：reaching-def 数据流

### 3.1 slot 抽象

JVM 的值存在两类槽：
- **local slot**：`iload/istore` 等操作的局部变量表下标（`MIRLocalLoad/Store::varIndex`）。
- **stack slot**：操作数栈位置。**但 stack slot 在 SSA 构造里只存在于 builder 的 FrameState，不参与 reaching-def 数据流** —— 因为栈的生命期在一个块内线性，跨块时栈必须为空（JVM 规范：跳转目标处栈高度由 StackMapTable 固定，但同一逻辑栈位置在不同前驱可以是不同值）。

**当前简化**：reaching-def **只跟踪 local slot**。stack 的跨块合并问题转化为"块入口处的栈内容"（见 §3.4）。这覆盖了 `sumTo`/`gcd` 这类循环 phi 场景（phi 都在 local 上）。异常 handler 的单元素栈是唯一特殊入口形状，由合成 exception block arg 表达，不进入通用 stack-slot 数据流。

### 3.2 数据流方程

经典 reaching-def：

```
IN[B]  = ∪_{P ∈ preds(B)} OUT[P]
OUT[B] = gen[B] ∪ (IN[B] − kill[B])
```

其中：
- **def** = 一条对 local slot N 的写（`MIRLocalStore varIndex=N`，或 `MIRIinc varIndex=N`）。
- **gen[B]** = B 内"最后一次"对每个 slot 的写。
- **kill[B]** = B 内对某 slot 的写，杀死"前面对同 slot 的写"。

def 用 `(slot, defId)` 表示，`defId` = 该 MIR 节点在块内的序号 + 块 id（全局唯一）。

### 3.3 求解（不动点迭代）

```
1. 计算每块的 gen[B] / kill[B]（一次扫描 B 内 MIRNode）
2. 初始化 OUT[B] = gen[B]（或空集）
3. worklist：反复直到收敛
     for each block B:
       IN[B] = ∪ OUT[pred] for pred in preds(B)
       new_OUT = gen[B] ∪ (IN[B] − kill[B])
       if new_OUT != OUT[B]: OUT[B] = new_OUT, 把 B 的后继加入 worklist
```

收敛性保证：def 集合单调增长，有上界（全 function 的 def 总数），必收敛。

**复杂度**：O(blocks × avg_defs × iterations)，对 R1 测试样本（几十块）毫秒级。

### 3.4 从 reaching-def 得"是否需要 block arg"

对块 B 的入口，对每个 local slot N：
- 若 `IN[B]` 中 slot N 有 **≥2 个 def** → N 在 B 入口需要一个 block arg（φ）。
- 若 `IN[B]` 中 slot N 有 **恰好 1 个 def** → N 在 B 入口的值就是那个 def（不需要 arg，前驱直接传那个 value）。
- 若 `IN[B]` 中 slot N 有 **0 个 def**（未定义到达） → 用 `SSAOpaqueValue` 兜底（R1 测试样本不应出现；出现 = L1 分析或样本问题，fail-closed）。

**block args 集合** = B 入口所有"≥2 源"的 slot。块的 `args[]` 就按这个建（每个 arg 一个唯一 `SSABlockArg`）。

> 注：R1 实现里可以更激进 —— 对"≥1 源"的 slot 都开 arg（统一传值），简化前驱传值逻辑。Grunt 的 `frameShape()` 就是把入口 frame 里所有 local+stack 都开成 arg。mj2x R1 采用这个更简单的策略：**块入口处所有 live 的 local slot 都开 arg**，前驱一律传值。代价是 arg 多几个（占一点生成 C 的变量数），但逻辑简单、不易错。L4 可加 pass 消除冗余 arg。

---

## 4. 阶段 B：块结构搭建

```
1. 建 SSAFunction，symbol 从方法名+desc 来。
2. 建 entry 合成块（无 args，无指令），它的 terminator 是 Jump → 真入口块。
3. 对 L1 的每个 Block 建 SSABlock：
     id = ids.newBlockId()
     args = 按 §3.4 决定（每 live local slot 一个 SSABlockArg）
4. 建前驱/后继关系（直接复用 L1 Block::predecessors/successors）。
5. 真入口块的初始 locals 来自方法参数（SSAParameter），栈空。
   entry 合成块 Jump 到真入口块时，传 (SSAParameter...) 作为真入口块的 args
   （若真入口块 args 非空；通常方法参数直接对应）。
```

---

## 5. 阶段 C：逐块翻译（核心）

### 5.1 FrameState

builder 维护一个**构造期临时**状态（不进 IR）：

```
FrameState {
  locals: SSAValue?[maxLocals]   // 每个 local slot 当前绑定的 SSA value（或 null=未定义）
  stack:  SSAValue[]             // 操作数栈（纯数据结构，吸收 DUP/SWAP）
}
```

进入块 B 时，`FrameState` 初始化：
- `locals[N]` = 块 B 的第 k 个 arg（若 N 是 B 的 arg slot）或上一块的出口值（reaching-def 给出，§3.4）。
- `stack` = 块 B 入口需要的栈内容。普通控制流块入口为空；异常 handler 块入口先压入合成 exception block arg，使首条 `ASTORE` 能按 JVM 规则消费异常对象。

### 5.2 逐 MIRNode 翻译规则

| L1 MIRNode | FrameState 操作 | 产出 SSA |
|------------|----------------|----------|
| `MIRConstant` | push literal | — (literal 直接进栈) |
| `MIRLocalLoad varIndex=N` | push `locals[N]` | — |
| `MIRLocalStore varIndex=N` | v=pop; `locals[N]=v` | — |
| `MIRIinc varIndex=N, incr` | v=locals[N]; r=Binary(Add, v, lit(incr)); `locals[N]=r` | `SSABinary` |
| `MIRBinaryOp` | rhs=pop, lhs=pop, r=Binary; push r | `SSABinary` |
| `MIRUnaryOp` | v=pop, r=Unary; push r | `SSAUnary` |
| `MIRCast` | v=pop, r=Convert; push r | `SSAConvert` |
| `MIRStackOp(DUP)` | v=pop; push v; push v | — |
| `MIRStackOp(DUP_X1)` | v1=pop,v2=pop; push v1,v2,v1 | — |
| `MIRStackOp(SWAP)` | v1=pop,v2=pop; push v1,v2 | — |
| （其余 DUP 变体） | 按 JVM 栈语义操作 FrameState | — |
| `MIRNewObject/Array` | dims=pop...; r=Allocate; push r | `SSAAllocate` |
| `MIRArrayLoad` | idx=pop, arr=pop, r=ArrayLoad; push r | `SSAArrayLoad` |
| `MIRArrayStore` | v=pop,idx=pop,arr=pop | `SSAArrayStore` |
| `MIRFieldGet` | recv?=pop; r=LoadField; push r | `SSALoadField` |
| `MIRFieldPut` | v=pop, recv?=pop | `SSAStoreField` |
| `MIRInvoke` | args=pop...; r?=Call; push r? | `SSACall` |
| `MIRArrayLength` | arr=pop, r=Intrinsic; push r | `SSAIntrinsic` |
| `MIRMonitor` | v=pop | `SSAIntrinsic` |
| `MIRCheckCast` | v=pop, r=Convert(ref); push r | `SSAConvert` |
| `MIRInstanceOf` | v=pop, r=Intrinsic; push r | `SSAIntrinsic` |
| `MIRThrow` | v=pop | terminator=`SSAThrow(v)` |
| `MIRReturn` | — | terminator=`SSAReturn()` |
| `MIRReturnValue` | v=pop | terminator=`SSAReturn(v)` |
| `MIRBranch` | v=pop | r=Compare; terminator=`SSABranch(r, T, F)` |
| `MIRBranchZero` | v=pop | r=Compare(v,lit0); terminator=`SSABranch(r,T,F)` |
| `MIRUncondBranch` | — | terminator=`SSAJump(target, args)` |
| `MIRSwitch` | v=pop | terminator=`SSASwitch(v, cases, default)` |
| `MIRLabel/MIRFrame/MIRNop` | 忽略 | — |

**栈操作不产 IR** —— 这是"消除 cstack 锚点"的核心机制：栈只在 FrameState 里临时存在，产物 IR 里没有任何栈结构。

### 5.3 终结符的 successor.args 传递

当块 B 的 terminator 要跳到后继块 S：
1. 查 S 的 `args[]`（阶段 B 决定）。
2. 对 S 的每个 arg（对应一个 local slot N 或 stack slot K）：
   - 若是 local slot N → 取 `FrameState.locals[N]`（B 出口处 N 的当前值）。
   - 若是 stack slot → 取 `FrameState.stack[K]`（R1 一般为空）。
3. 构造 `SSASuccessor{block=S, args=[取出的 SSA value...]}`。

这样 S 的每个 arg 在不同前驱进来时拿到不同值 = φ 语义。

### 5.4 类型推断

`SSAInstruction` 的 result type 由操作数类型决定（R1 用 L1 `method_stack` 的类型推导结果，或保守用 `SSA_I32`）。R1 目标是结构正确（无栈槽），类型精确度推到 R2（lowering 需要精确类型）。

---

## 6. 异常处理

带 try-catch 的方法，handler 块入口的栈含单个异常对象，且 protected 块到 handler 是异常边。这需要：
- 异常边纳入 reaching-def 的 preds。
- handler 入口栈 = 单元素（异常）。

当前实现把 `CodeConverter` 携带的 exception table 转成 `SSAExceptionRegion`：

1. try start/end/handler label 都成为 L1 block 边界。
2. 每个 protected block 向 handler 加一条 reaching-def 异常前驱边。
3. handler block 增加一个 `SSAType::REF` 的合成 block arg，slot 为 `kSSAExceptionObjectSlot`。
4. 翻译 handler block 时，builder 先把该 arg 压入 FrameState.stack，再按普通 L1 节点翻译；Java 编译器常见的 handler 起始 `ASTORE` 因此会自然 pop 出异常对象。
5. `SSAExceptionRegion` 记录 protected blocks、handler、exceptionValue 和 caughtType，后续 RegIR/RegC 用它生成 pending-exception dispatch。

这仍不是完整 JVM verifier。通用跨块 operand-stack 合流依旧不建模；当前只显式支持 try/catch handler 的单异常对象入口。

---

## 7. 正确性测试（R1 验收）

### 7.1 结构断言

- `Add(int,int)→int`：SSA 里所有操作数是 `SSAValue`（Parameter/Result/Literal），**无任何栈槽引用**。
- dump 文本过 c2j `CSTACK_RE`（`cstack(\d+)\.(\w)`）→ **零匹配**。
- 每块的 `args.size()` == 每个前驱 `SSASuccessor.args.size()`。

### 7.2 phi 语义断言

- `LoopSum`（`int s=0; for(...) s+=i; return s;`）：循环头块对 `s` 所在 slot 有一个 block arg，且：
  - 前驱1（方法入口/初始化）传 `ICONST_0` 的 result。
  - 前驱2（循环体回边）传 `IADD` 的 result。
  - 即该 arg 真的是 φ，不是单值。

### 7.3 reaching-def 数据流自测

- `reaching_def` 单元测试：构造一个 2-块循环（手工 L1 Block），断言循环头 `IN[B]` 含 ≥2 个 def for 归纳变量 slot。这直接覆盖了 R0.5 发现的 libjbytecode Analyzer 缺陷场景，证明自写 pass 正确。

---

## 8. 与竞品算法的对照

| 维度 | mj2x R1（本文） | Grunt `JvmSSAImporter` | masxinlingvonta | 经典 SSA |
|------|----------------|----------------------|----------------|----------|
| SSA 变体 | block-arg（φ 隐式） | block-arg（φ 隐式） | LLVM alloca+mem2reg | 显式 φ |
| 数据流来源 | **自写 reaching-def**（基于 L1 CFG） | ASM `BasicInterpreter` | ASM `SourceInterpreter` + LLVM | 自写 dominance+DF |
| 支配树 | 不需要 | 不需要 | LLVM 内部算 | 必须 |
| libjbytecode analysis | **不依赖**（实测有 bug） | — | — | — |
| φ 插入 | 不显式插 | 不显式插 | LLVM 算 | 显式插 |
| 异常边 | 支持 handler exception arg + exceptionRegions | 支持（exceptionRegions） | 支持 | 视实现 |

mj2x 与 Grunt 算法骨架相同（block-arg），差别只在数据流来源：Grunt 用 ASM Analyzer（Java 生态成熟），mj2x 因 libjbytecode Analyzer 有 bug 改用自写（基于已验证的 L1 CFG）。

---

## 9. 已知简化（R1 接受，后续补）

1. **block arg 策略**：所有 live local 都开 arg（不只 ≥2 源），arg 数偏多。L4 加 arg-elimination pass。
2. **类型精确度**：R1 用保守类型，R2 lowering 前补精确推断。
3. **异常处理**：已支持 try/catch handler 单异常对象入口；更复杂的 verifier 级异常/栈形状仍保守处理。
4. **stack 跨块合并**：普通控制流仍要求块入口栈空；handler 单元素栈是显式特例。
5. **INVOKEDYNAMIC/MULTIANEWARRAY**：R1a 不直接处理，R1b preprocessor 先展开（dev_plan R1b.4）。

这些简化都不影响 R1 验收标准（无栈槽 + phi 正确）。

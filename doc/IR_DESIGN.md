# mj2x IR 设计（L2 SSA / L3 寄存器 IR）

> 状态：R1a 实现 note，权威随代码演进
> 日期：2026-06-25
> 关系：本文档定义 `src/mj2x/mir/ssa/` 的 IR 数据结构。
> 上层依据：`ARCHITECTURE_REDESIGN.md §3`（五层分层）、`dev_plan.md` R1 决策（R1-A/R1-B/R1-C）。
> 算法依据：`SSA_ALGORITHM.md`（reaching-def + block-arg 构造流程）。
>
> 本文只讲"IR 长什么样"；"怎么从字节码构造出来"在 `SSA_ALGORITHM.md`。

---

## 1. 设计目标与约束

IR 是为**消除 c2j 锚点 2（`cstack[N]`/sp 栈数组）**服务的。硬性约束：

1. **无栈数组语义**：IR 的操作数必须是"值"（有唯一 id），不能是"`cstack[3]` 这种带下标的存储槽"。这是 R1 的验收标准（dump 过 `CSTACK_RE` 零匹配）。
2. **寄存器化天然成立**：每个 value 就是一个虚拟寄存器，L5 生成 C 时直接映射成 `v<id>` 局部变量。**不另做物理寄存器分配**（C 局部变量无限，无意义）。见 dev_plan R1-C。
3. **φ 不显式存在**：采用 **block-argument SSA**（WebAssembly / MLIR / Grunt 路线），φ 编码在前驱→后继边的传值里。这砍掉了经典 dominance + φ 插入 + 重命名这一大坨。见 `SSA_ALGORITHM.md`。
4. **类型复用 L1**：`SSAType` 与 `frontend/mir_node.hpp::PrimitiveKind` 对齐，避免重复。
5. **零 libjbytecode analysis 依赖**：reaching-def 自写（R0.5 实测 libjbytecode Analyzer 有 bug，见 dev_plan R1-B）。IR 本身只依赖 STL。

---

## 2. 顶层结构：`SSAFunction`

```
SSAFunction
├── symbol        : SSAFunctionSymbol { name(id), paramTypes[], returnType }
├── parameters    : SSAParameter[]           // 方法形参，每个是入口可见的 SSA value
├── blocks        : SSABlock[]               // 所有基本块（含 entry）
├── entry         : SSABlock*                // 唯一入口块（合成，terminator=Jump→真入口）
└── exceptionRegions : SSAExceptionRegion[]  // L1 exception table lowered to SSA regions
```

`SSAExceptionRegion { protectedBlocks:SSABlock[], handler:SSABlock*, exceptionValue:SSABlockArg*, caughtType:string }`
describes one exception-table entry.  The handler receives a synthetic REF block
argument at slot `kSSAExceptionObjectSlot`; RegIR lowers that arg to the
register that RegC assigns after a matching pending exception is cleared.

---

## 3. 基本块：`SSABlock`

```
SSABlock
├── id            : SSABlockId（全局递增）
├── args          : SSABlockArg[]     // 入口参数 = 隐式 φ（见 §6）
├── instructions  : SSAInstruction[]  // 块内顺序指令，不含控制转移
└── terminator    : SSATerminator     // 唯一的块出口（Jump/Branch/Switch/Return/Throw/Unreachable）
```

**约束**（构造期不变式，违反 = builder bug）：
- `instructions` 里**不允许**出现控制转移；所有出边归 `terminator` 拥有。
- 每条指令**至多定义一个** result（SSA 单赋值）。
- `args` 的个数 == 所有前驱在跳进来时传值的个数（`SSASuccessor.args.size()`，每个前驱一致）。
- `entry` 块无前驱（合成块），其 `args` 为空。

---

## 4. 值体系：`SSAValue`

所有"操作数"的基类。一个 SSAValue 在整个 function 内有唯一 id，定义一次。

```
SSAValue（抽象基类）
├── id            : SSAValueId（全局递增，= 虚拟寄存器号）
└── type          : SSAType
```

具体子类：

| 子类 | 含义 | 来源 |
|------|------|------|
| `SSAParameter` | 方法形参 | function 入口可见；`index` 标记第几个形参 |
| `SSABlockArg` | 块入口参数（隐式 φ） | 块的 `args[]`；每个前驱传一个值给它 |
| `SSAInstructionResult` | 指令的输出 | 唯一由某条 `SSAInstruction` 定义 |
| `SSAIntLiteral` | 整型常量（ICONST/BIPUSH/LDC int/long） | 字面量 |
| `SSAFloatLiteral` | 浮点常量（FCONST/DCONST/LDC float/double） | 字面量 |
| `SSANullLiteral` | ACONST_NULL | 字面量 |
| `SSAOpaqueValue` | 兜底/未建模值（如 LDC String/Type/Handle，R1 先不细化） | 防止构建崩 |

**关键不变式**：`SSAParameter`、`SSABlockArg`、`SSAInstructionResult` 三者构成"被引用的值"的全部来源；指令的 `operands`/`SSASuccessor.args` 只能引用这三类（或 literal）。

---

## 5. 类型体系：`SSAType`

与 L1 `PrimitiveKind` 对齐（`frontend/mir_node.hpp`），不重复发明：

| SSAType | 对应 PrimitiveKind | C lowering（R2 用） |
|---------|-------------------|---------------------|
| `SSA_I8` | BYTE | jbyte / uint32_t 计算 |
| `SSA_I16` | SHORT | jshort / uint32_t |
| `SSA_I32` | INT（含 BOOL/CHAR 的计算位宽） | jint / uint32_t |
| `SSA_I64` | LONG | jlong / uint64_t |
| `SSA_F32` | FLOAT | jfloat |
| `SSA_F64` | DOUBLE | jdouble |
| `SSA_BOOL` | BOOLEAN | jboolean |
| `SSA_CHAR` | CHAR | jchar |
| `SSA_REF` | OBJECT | jobject（引用类型） |
| `SSA_ARRAY` | OBJECT（带维度） | jobject（R1 用 REF 统一表示，R2 细化） |
| `SSA_VOID` | VOID | 无值（return/void call） |
| `SSA_UNKNOWN` | — | 兜底，构建期不应进入 lowering |

`stackSize(type)`：I64/F64 = 2（JVM category-2），其余 = 1（VOID=0）。**这个尺寸只用于 reaching-def 的槽位对齐**（见 `SSA_ALGORITHM.md`），IR 本身不关心栈。

---

## 6. φ 的表示（核心）：block arguments

**没有显式 φ 节点。** φ 语义由两处协同编码：

```
SSABlock
├── args : SSABlockArg[]          // 每个 arg = 一个"在块入口处取什么值"的 φ

SSATerminator
└── SSASuccessor { block:SSABlock*, args:SSAValue[] }
                                  // 前驱跳到后继时，显式给后继每个 arg 传一个值
```

**示例**（`sumTo` 循环头 `s` 的 φ）：

```
B_entry:                             ; s 的初值
  ...
  goto B_header(s_init, i_init)      ; 前驱1：传 ICONST_0 的 result

B_loop:                              ; s 的循环体新值
  s_new = add(s_arg, i_arg)
  ...
  goto B_header(s_new, i_new)        ; 前驱2：传 add 的 result

B_header(s_arg: I32, i_arg: I32):    ; ← 两个入口 arg 就是 φ
  ...                                ; 块内引用 s_arg 即得"当前 s"
```

`s_arg` 就是 `s` 的 φ —— 它的值取决于从哪条边进来：
- 从 `B_entry` 进来 → `s_arg` = `s_init`
- 从 `B_loop` 回边进来 → `s_arg` = `s_new`

**为什么这等价于经典 φ**：经典 SSA 在 `B_header` 开头放 `s3 = φ(s_init, s_new)`；block-arg SSA 把这个 φ 拆成"后继声明要 s_arg"+"前驱显式传值"。两者数据流等价，但后者：
- 无需支配树（不决定 φ 插哪）。
- 无需变量重命名（每个 value 天然单赋值）。
- L5 生成 C 时，每个 block 用 `goto label` + 入口处对 `barg_<id>` 赋值即可（Grunt `NativeSsaDirectTranslator` 正是这么做）。

**`args` 的个数怎么定**：由 reaching-def 决定。`B_header` 入口处"还有可能被后文引用的 def 集合"里，凡是有 ≥2 个来源的 slot，就需要一个 arg。详见 `SSA_ALGORITHM.md`。

---

## 7. 指令：`SSAInstruction`

块内非控制转移指令。每条至多一个 `result`（可选，void call/store 无 result）。

12 类（对齐 Grunt，覆盖 JVM 计算语义）：

| 指令 | operands | result | JVM 来源示例 |
|------|----------|--------|-------------|
| `SSAUnary` | [value] | 有 | INEG / L2I / I2B |
| `SSABinary` | [lhs, rhs] | 有 | IADD / IMUL / ISHL / LCMP |
| `SSACompare` | [lhs, rhs] | 有(bool) | （IF 指令的判定，见 §8） |
| `SSAConvert` | [value] | 有 | I2L / I2B / 数值转换 |
| `SSALoadField` | [receiver?] | 有 | GETFIELD / GETSTATIC |
| `SSAStoreField` | [receiver?, value] | 无 | PUTFIELD / PUTSTATIC |
| `SSAArrayLoad` | [array, index] | 有 | IALOAD / AALOAD |
| `SSAArrayStore` | [array, index, value] | 无 | IASTORE |
| `SSACall` | [args...] | 可选 | INVOKE*（R1 先建 IR，R2 lowering） |
| `SSAAllocate` | [args...] | 有 | NEW / NEWARRAY / ANEWARRAY / MULTIANEWARRAY |
| `SSAIntrinsic` | [args...] | 可选 | ARRAYLENGTH / MONITORENTER / LCMP 三元比较 |
| `SSABarrier` | [] | 无 | 副作用栅栏（占位，防 DCE 误删） |

每条指令带 `SSAEffect`（L4 passes 用，R1 先填保守值）：
```
SSAEffect {
  mayThrow, readsMemory, writesMemory,
  readsExternalState, writesExternalState,
  callsExternal, resolvesExternal, isBarrier
}
```
**纯算术**（Unary/Binary 无 trap）= `Pure`；div/rem = `Pure.copy(mayThrow=true)`；
数组访问 = `ReadMemory/WriteMemory.copy(mayThrow=true)`；call = `ExternalCall`。

**INVOKE_DYNAMIC** 不在这 12 类里 —— 它由 L1 preprocessor 在翻译前展开为 MethodHandle 链（dev_plan R1b.4），翻译期不再有 invokedynamic。

---

## 8. 终结符：`SSATerminator`

块的唯一出口。6 种：

| Terminator | 携带 | 语义 |
|-----------|------|------|
| `SSAJump` | successors[1] | 无条件跳；传后继 args |
| `SSABranch` | condition:SSAValue, successors[2] (true/false) | 条件跳；各传 args |
| `SSASwitch` | value:SSAValue, cases[]{key, successor}, default:successor | switch；各传 args |
| `SSAReturn` | value:SSAValue? | 返回（void 则 value=null） |
| `SSAThrow` | value:SSAValue | 抛异常（R1：翻译为 Throw+return） |
| `SSAUnreachable` | — | 不可达（构造期占位，验证期应被消除或保留为 trap） |

`SSASuccessor { block:SSABlock*, args:SSAValue[] }` —— 每条出边显式传后继块 args。

**JVM IF 指令的 lowering**：`IF_ICMPLT` → `SSACompare(Lt, lhs, rhs)` 产 bool result → `SSABranch(result, trueTarget, falseTarget)`。`IFEQ` → compare vs `SSAIntLiteral(0)`。

---

## 9. id 分配

全局递增，单线程构建期顺序分配：
- `SSABlockId`、`SSAValueId` 由 `SSAIds`（一个自增计数器集合）发。
- value id **即虚拟寄存器号**，L5 生成 C 时 `v<id>` / `barg_<id>` 直接用。
- id 稳定（构建后不变），方便 dump、调试、pass 做引用分析。

---

## 10. 与 L1 MIR 的关系

L1（`frontend/mir_node.hpp` 的 `MIRNode` 体系）是**接近 ASM 的栈式 IR**，保留完整 JVM 栈语义。R1 的 SSA builder **消费 L1 的 `Block[]`**（复用 `CodeConverter` 的 CFG + `predecessors/successors`），把栈式语义"烤进 block args"：

- L1 `MIRLocalStore varIndex=N` → SSA：pop 一个 value，定义"local N 的当前值 = 该 value"。
- L1 `MIRLocalLoad varIndex=N` → SSA：push"local N 的当前值"（reaching-def 给出是哪个 SSA value）。
- L1 `MIRBinaryOp` → SSA：pop 两个 value，建 `SSABinary`，push result。
- L1 `MIRStackOp(DUP/SWAP/...)` → SSA：在 builder 的 FrameState（locals[]+stack[]）上做纯栈操作，**不产 IR 指令**（DUP 只是让栈顶两个槽指向同一 value）。

**关键**：L1 的栈操作（DUP/SWAP）在 SSA builder 里被"吸收"——它们只影响 FrameState 的栈槽指向，不生成任何 SSA 节点。这就是"消除栈数组"的机制：栈只在构建期作为临时数据结构存在，产物 IR 里没有栈。

---

## 11. 不变量校验（R1 测试硬断言）

构建完成后，`SSAVerifier`（或测试）检查：
1. 每个 `SSAInstructionResult`、`SSAParameter`、`SSABlockArg` 在 function 内 id 唯一。
2. 每条指令的 operands、每个 `SSASuccessor.args` 引用的 value 都在该 value 的支配范围内（R1 用 reaching-def 近似：被引用时其 def 必可达）。
3. 每个 `SSABlock.args.size()` == 每个前驱 `SSASuccessor.args.size()`。
4. **dump 文本不含 `cstack`、`sp`、`cclasses` 等任何 c2j 锚点子串**（验收硬标准）。

---

## 12. 文件组织

```
src/mj2x/mir/
├── ssa/
│   ├── ssa_ids.hpp           // SSABlockId / SSAValueId / SSABlockId 自增
│   ├── ssa_type.hpp          // SSAType 体系（对齐 PrimitiveKind）
│   ├── ssa_value.hpp         // SSAValue 基类 + 7 子类
│   ├── ssa_effect.hpp        // SSAEffect（L4 用）
│   ├── ssa_instruction.hpp   // 12 类指令
│   ├── ssa_terminator.hpp    // 6 类 terminator + SSASuccessor + SSABlockArg
│   ├── ssa_block.hpp         // SSABlock
│   ├── ssa_function.hpp      // SSAFunction + SSAFunctionSymbol + SSAExceptionRegion
│   ├── ssa_builder.hpp/.cpp  // JVM→SSA Importer（见 SSA_ALGORITHM.md）
│   ├── ssa_dump.hpp/.cpp     // 文本 dump
│   └── ssa_verifier.hpp/.cpp // §11 不变量校验
└── reaching_def.hpp/.cpp     // 自写 reaching-def 数据流（零 libjbytecode 依赖）
```

`reaching_def` 放 `mir/` 而非 `mir/ssa/`，因为它服务于 builder，但本身是独立数据流 pass，未来 L4 也会用。

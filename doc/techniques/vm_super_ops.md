# VM 指令级变换：super-ops 与指令碎片化（VMC4）

> 在 VM 指令序列层打散「一条 JVM 字节码 ↔ 一条 VM 指令」的直译关系。super-ops
> 把相邻多条指令合成一条复合指令；碎片化把一条指令拆成多段无意义的中转。
> 二者方向相反、可组合，都只改指令流的形状、不改语义。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `vm.*` 的 super-op / 指令碎片化思路 |
| 作用层 | VMC4 编码器（`VmEncoder`）+ 解释器 handler |
| 开关 | `--vmc4-super-ops` / `--vmc4-fragment` |
| 后端 | VMC4（其他三后端不走这条路） |
| 默认 | 关（且互不联动，符合 `--vmc4-*` 一族"默认关、不被其他开关联动"的约定） |
| 状态 | ✅ 已落地（issue #9） |
| 相关文档 | [vm_virtualization.md](vm_virtualization.md)（VM 本体） |

## 对抗什么

VMC4 把方法编码成自定义指令集的程序 blob，这一步已经换掉了 ISA。但在本项落地
前，**指令流与字节码仍是直译关系**：一条 JVM 字节码对应一条 VM 指令，逐条对照
就能把算法读回来——VM 只做到了「换指令集」，没做到「打散结构」。

两项变换各从一个方向切断这个对应：

- **super-ops**：相邻 N 条 VM 指令 → 1 条复合指令。指令数变少、单条变胖，
  中间值不再以独立寄存器存在。
- **碎片化**：1 条 VM 指令 → M 条等价碎片。指令数变多，每段单独看是无意义的
  中间搬运。

## 机制

### super-ops（`--vmc4-super-ops`）

两类融合，都在编码前一次性规划（需要看到相邻指令序列与全函数使用计数，与逐
指令编码解耦，落点在编码器的 `planSuperOps` 而非 RegIR——融合是 VMC4 指令集
专属的编码格式决策，RegIR 没有 super-op 概念，把后端专属概念泄给 RegC/LLVM/
StackC 共用的 IR 会迫使三个后端都认识一个新指令种类）：

| 模式 | 产物 | 说明 |
|---|---|---|
| `Binary(x, ±1)` | `SuperInc` / `SuperDec` | 去掉 rhs 操作数，指令更短（opcode `0x23`/`0x24`，Phase 3 已有，本项加开关门控） |
| `t = a op1 b` 紧接 `dest = t op2 c` | `FusedBinary` | 同一基本块内相邻、`t` 全函数仅此一处消费；中间值不再独立成寄存器，只活在 handler 的局部变量里（opcode `0x25`，本项新增） |

`FusedBinary` 的编码是 `[op1:u8][op2:u8][type:u8][dest:u16][a:u16][b:u16][c:u16]`，
handler 内联展开两步运算（语义与 `Binary` handler 逐条一致：整数环绕、I32 逐步
截断、除零抛 `ArithmeticException`、F32 的 float 精度逐步舍入）。

**融合判定的三条边界**（单测逐一覆盖）：

1. `±1` 折叠只认 `rhs` 恰为 `IntLiteral(1)` 且 op 为 `Add`/`Sub`——`x+2`、
   `x*1` 都不折叠。
2. `FusedBinary` 要求中间值**唯一消费**：`t` 被第二处读取时不融合（否则折叠后
   有代码读不到该值）。
3. 融合**永不跨基本块**：相邻性以基本块为界，逐块独立规划，也不链式重叠
   （三链只融前两条）。

### 碎片化（`--vmc4-fragment`）

每条计算指令（`Binary`/`Unary`/`Compare`/`Convert`）的寄存器操作数先经专用
临时槽中转一次：`Binary(dest, lhs, rhs)` 变成 `Copy(t0, lhs); Copy(t1, rhs);
Binary(dest, t0, t1)`。纯编码期变换，不新增 opcode。

临时槽（3 个）在寄存器文件高水位末端**惰性预留**——首次使用时取当前 `maxReg`，
预留后立即抬高水位，此后一切常量/寄存器分配只能落在临时槽之上，无条件不重叠
（不能提前预留：编码期新观察到的寄存器会与预占的槽碰撞）。`Copy` handler 整体
拷贝 union，故任何类型的中转都不改位模式，语义恒等。

### 组合

两项可同时开启：super-op 的操作数同样中转一次，不存在「折叠过的指令反而没
被打散」的漏网岛；`FusedBinary` 的三个源操作数各占一个临时槽（临时槽数取 3
即为此上界）。

## 产物形态

- 指令字节流：`Binary` 条数减少（super-ops）或每条前多若干 `Copy`（碎片化）；
  `instructionOffsets`（懒解密分块边界表）恒随之同步，无需额外处理。
- 寄存器文件：`numRegs` 可能 +3（碎片化临时槽）；常量槽仍恒高于全部计算寄存器。
- 解释器：`FusedBinary` 的产物依赖两个 TU 级静态 helper（整数/浮点单步运算），
  仅当产物真含 `FusedBinary` 时才发射（`VmProgram::hasFusedBinary`），避免未使用
  函数告警。opcode 枚举、handler 表、`kAllOpcodes` 支持集、`handlerOrder` 四处
  必须同步——漏一处即运行期崩（VMC4 的单一事实源纪律，见
  [vm_virtualization.md](vm_virtualization.md)）。

## 代价

实测（`app.Bench`：100000 × `Calc.sum(20)+Calc.bits(i)+Calc.fib(10)`，JDK 17，
同机 5 次取中位数，输出与未混淆基线逐字节一致）：

| 配置 | 中位耗时 | 相对 VMC4 默认档 |
|---|---|---|
| JVM 未混淆基线 | 211 ms | — |
| VMC4 默认（两项全关） | 4681 ms | 1.00x |
| VMC4 + super-ops | 4763 ms | 1.02x（噪声内） |
| VMC4 + fragment | 6256 ms | **1.34x** |
| VMC4 + 两项同开 | 6063 ms | 1.30x |

| 维度 | 影响 |
|---|---|
| 性能 | **super-ops 基本中性**（本样本可融合模式少，指令数下降被融合 handler 的双步运算抵掉；理论上 dispatch 次数减少应为净收）。**碎片化实测 +34% 解释耗时**——指令数约 2~3 倍，这是它默认关的原因，面向高保护诉求按需开启 |
| 产物体积 | 碎片化增大 blob（本样本 +2KB 量级）；super-ops 略减小 |
| 编译期 | 两项都只增加一次线性扫描，可忽略 |

## 已知绕过面

- **super-ops 不改变信息量**：`FusedBinary` 只是把两步运算打包，逆向时解出
  handler 即可还原，成本与解两条 `Binary` 相当。它的价值在切断 1:1 对应、
  提高自动化比对的噪声，不在提高单点分析难度。
- **碎片化可被 copy-propagation 抹平**：如果分析工具对 VM 指令流做常量/拷贝
  传播，`Copy` 段会被折叠回原形。它的价值同样在打断「逐条对照」的自动化。
- 两项都不改变 VM 本体的随机化面（opcode 映射 / drift / 加密），那些仍由
  `--vmc4-*` 其他开关负责。

## 与其他技术的关系

- 与 `--vmc4-drift` / `--vmc4-encrypt-program` / `--vmc4-lazy-decrypt` /
  `--vmc4-shuffle-blocks` **正交可叠**：本项改指令内容与条数，那些改字节到语义
  的映射、落盘形态与块布局。
- 与 RegIR transform passes（MBA / CFF / 常量加密 / 调用间接化）**互斥**——
  VMC4 不走 RegIR 变换（分工决定见 vm_virtualization.md）。
- 与 **运行时保护检查点**可叠。

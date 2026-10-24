# 寄存器文件定长化：线性扫描分配 + 溢出槽（VMC4）

> 切断「VM 寄存器号 = SSA value id」的恒等映射——红队拿寄存器编号即可对齐
> SSA 值编号的结构性锚点自此失效。计算寄存器经线性扫描压入定长池、死值槽位
> 复用；池装不下的活值分到稳定溢出槽。纯编码期变换，语义零变化、零运行时开销。

| 项 | 值 |
|---|---|
| 对标术语 | 线性扫描寄存器分配（Poletto & Sarkar）+ spill slot |
| 作用层 | VMC4 编码器（`vm_regfile.cpp` 分配器 + `VmEncoder` 重映射） |
| 开关 | `--vmc4-regfile <n>`（n = 池顶，0 = 关） |
| 后端 | VMC4（其他三后端不走这条路） |
| 默认 | 关（不被 `--vmc4-*` 其他开关联动） |
| 状态 | ✅ 已落地（issue #10；实现定界见下文「与 dev_plan 草案的偏差」） |
| 相关文档 | [vm_virtualization.md](vm_virtualization.md)（VM 本体）、[vm_super_ops.md](vm_super_ops.md)（指令级变换） |

## 对抗什么

RegId.value == SSA value id 恒等映射进 VM 寄存器索引，带来两个分析锚点：

1. **寄存器号直接暴露 SSA 值编号**——红队解码字节流后，寄存器号的序与 SSA
   构造序一致，def-use 链可按编号直接读出，等同于拿着带注释的 IR。
2. **寄存器文件大小 = 方法 SSA 值数**——从 `numRegs` 反推方法中间值数量。

定长化把计算寄存器重排进定长池：寄存器号变成分配结果（与 SSA 编号无关、死值
槽位复用、同一槽位在不同程序点承载不同值），`numRegs` 只反映池顶 + 溢出槽数 +
常量数，不再暴露方法值数。

## 机制

### 分配（`planVmRegfile`）

1. **位点扫描**：RegFunction 摊平成线性位点流（fn.blocks 顺序，与块布局/乱序
   无关——分配只依赖 CFG 真活性，任何线性化下「非重叠区间 ⇒ 不干扰」的包络
   论证都成立）。融合对（super-ops 计划产物）压缩为单条复合指令：被吸收的
   dest 不再存在（不参与分配），复合指令源 = 两条指令全部源操作数。
2. **活性分析**：块级 backward 不动点。边拷贝语义：后继边拷贝的 source 在源块
   terminator 位点被读取、dest（块参数）在目标块入口诞生；异常区域拷贝的
   source 在保护块末尾被读取、dest 在 handler 块入口诞生；`exceptionValue`
   同为 handler 入口定义。
3. **活跃区间**：`[min(定义点, liveIn 块起点), max(使用点, liveOut 块终点)]`
   ——真活性位点的**连续包络（安全过近似）**：liveIn/liveOut 块跨度扩展保证
   跨块携带与乱序线性化下的活位点全部落入包络；非重叠包络必不干扰。
4. **Poletto 线性扫描**：按起点排序，池耗尽时溢出「结束点最远」的区间
   （active ∪ 当前），被溢出者换取**稳定溢出槽**（一个值一个槽，全函数不变）。
   有序容器 + 确定性 tie-break，无未播种随机源（可复现构建不变量）。

### 槽位布局

```text
[0 .. numInputs)            : 方法参数（恒等映射，入口现场不可移动）
[numInputs .. poolTop)      : 分配池（poolTop = n；fragment 开启时 = n - 3）
[poolTop .. poolTop+3)      : fragment 临时槽（仅 --vmc4-fragment 开启时预留）
[n .. n+S)                  : 溢出槽区（S = 被溢出的活值数）
[n+S .. numRegs)            : 常量槽（与历史形态同构，从高水位向上分配）
```

### 溢出槽直接寻址（实现定界：不引入 Spill/Reload opcode）

dev_plan 草案曾写「+ Spill/Reload 两个新 opcode」，实现时改为**直接寻址**：
解释器的寄存器文件是统一 slots 数组，溢出槽就是数组高位索引，指令操作数
（u16）直接引用即可读写。对比 Spill/Reload 双 opcode 方案：

- **零运行时开销**：无 reg↔slot 搬运指令（Spill/Reload 方案每次跨指令携带
  都要一搬一取）。
- **零指令集/运行时面改动**：opcode 枚举、`kAllOpcodes` 支持集、handler 表、
  解释器发射器、`VmInstance.describe()`（trace surface 契约）全部不动——
  VMC4 指令集的「单一事实源四处同步」纪律（见 vm_super_ops.md 产物形态节）
  无需触发。
- **编码器单点重映射**：全部源操作数走 `resolveOperand` 唯一入口，dest 在
  各 `encodeXxx` 重映射，边拷贝/异常拷贝/`exceptionValue` 同面覆盖；映射
  缺失（扫描遗漏）fail-closed 中止编码，方法保留字节码不转 VM。
- 结构性目标完全达成：寄存器号不再镜像 SSA id、池定长。若未来运行时把寄存
  器文件拆成定长栈上数组 + 独立溢出区（handler 只摸定长数组以获得缓存局部
  性），再补 Spill/Reload 双 opcode。

### fail-closed 边界

- `limit < numInputs`：溢出槽区会与参数区重叠（参数是入口现场，被覆写即错
  值）——拒绝编码，诊断含 limit/numInputs 两个数字。
- `--vmc4-fragment` 组合要求 `limit ≥ numInputs + 3`（池顶预留 3 个碎片临时
  槽，不得落入参数区）。
- `numRegs > 0xFFFF`：u16 操作数回绕即静默腐蚀（#1133 同款），编码入口
  fail-closed；`maxReg` 以 u32 承载探测回绕，历史恒等映射形态同受益。

### 与 super-ops / fragment 的组合

- **super-ops**：融合计划在分配前落地（分配器消费同一份 `superOpFusedNext`），
  融合对按单条复合指令参与分配——融合率不受分配影响，被吸收的 dest 不出现在
  任何操作数里。
- **fragment**：临时槽从「高水位末端」改为「池顶 3 槽」，全部计算面保持在
  上限之内；碎片化中转的源可以是常量槽（字面量操作数同样经中转），目的临时
  槽恒在池顶区间。

## 产物形态

- 字节流：指令序列不变（无新增指令），全部寄存器操作数换成分配结果；opcode
  集合、handler、`instructionOffsets`、块布局均不受影响。
- `numRegs`：池顶 + 溢出槽 + 常量；对死值密集的方法**小于**恒等映射形态
  （复用实证，单测断言）。
- 可复现构建：同方法 + 同 limit → 同分配（活性不动点、区间排序、扫描全用
  有序容器）。

## 代价

| 维度 | 影响 |
|---|---|
| 运行时 | **零**（直接寻址无搬运指令；分配结果只是换了一组槽位号） |
| 产物体积 | 通常**减小**（寄存器复用 → numRegs 收缩 → 解释器 slots 数组与操作数字节数略降；本实现无实测数据，量级在字节级） |
| 编译期 | 每方法一次活性不动点 + 线性扫描，方法级规模，可忽略 |
| 逆向 | 寄存器号↔SSA 编号对应失效，def-use 链需真做数据流分析才能重建 |

## 已知绕过面

- 分配是确定性线扫（非随机化面）：逆向后 `numRegs`/池顶/溢出槽边界可见，
  可推断池容量参数，但拿不回 SSA 编号。
- 溢出槽的「一个值一个稳定槽」分配偏保守（压力大的方法溢出多）；slot 共享
  （跨活跃区间复用溢出槽）是后续可做的增强，不改变正确性论证。
- 不改变 VM 本体的其他随机化面（opcode 映射 / drift / 块乱序 / 加密），那些
  仍由 `--vmc4-*` 其他开关负责。

## 与其他技术的关系

- 与 `--vmc4-super-ops` / `--vmc4-fragment` 正交可叠（组合语义见上文）；
  与 `--vmc4-drift` / `--vmc4-encrypt-program` / `--vmc4-lazy-decrypt` /
  `--vmc4-shuffle-blocks` 正交可叠（本项只换操作数编号，不碰字节↔语义映射、
  落盘形态与块布局）。
- 与 RegC/LLVM 的寄存器分配（物理寄存器/CSSA）无共享代码——VMC4 的「寄存器」
  是 VM 槽位，分配目标是无压力的槽位空间，线性扫描足矣。
- 测试矩阵：emit 级 7 用例（`Vmc4BackendTest.Regfile*`：默认关字节一致、池
  复用收缩、压力溢出、fail-closed、fragment/super-ops 组合、控制流解码一致
  性）+ E2E 2 用例（`Vmc4BlockLazyE2E.Regfile*`：小池单开、三开关组合，真实
  编译 + JVM 对拍）。

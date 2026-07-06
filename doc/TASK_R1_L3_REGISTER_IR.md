# Task Packet: mj2x R1 剩余 — L3 寄存器化（SSA → RegIR）

> **状态：deferred（为 RegC 后端预留的路线，当前不执行）**
>
> 当前 mj2x 主线是 C backend（直接吃 L1 栈式 MIR，见 `CBACKEND_REVIVAL_AND_WAVES.md`）。
> 本任务包是「RegC 寄存器式 C 后端」的预备工作，依赖 R1a/R1b 已合并的 SSA 骨架。
> 两条路线定位不同，多后端并存：
> - **C backend**（当前主线）：易实现、广覆盖、对抗强度低（c2j 可还原）
> - **RegC**（本任务包，未来）：依赖 L3 RegIR，对抗强度高（破 c2j 锚点）
>
> 本任务包留待决定启动 RegC 后端时再执行。

 ---

> ⚠️ **先读这条：R1 真实状态与文档偏差**
>
> 文档里多处描述与代码现状有偏差，开工前必须澄清：
>
> - `dev_plan.md §0` 表格写 "R1 待开始"，**已过时**。R1 的两个子任务已合并：
>   - **R1a 已完成**（PR #308 `8a853a6e`）：SSA 骨架 + reaching-def + dump + 设计文档
>   - **R1b 已完成**（PR #309 `b91640a5`）：code_converter 修复 + preprocessor（LDC/invokeDynamic/multiANewArray）
> - **本任务包做的是 R1 剩余的 L3 寄存器化**，即把 R1a 产出的 `SSAFunction` lower 成 `RegModule`。
> - `ROADMAP.md`/`dev_plan.md` 里"复用 libjbytecode analysis 的 Frame/SourceValue"**作废**：
>   R0.5 实测 libjbytecode 的 `Analyzer` 有 bug（`maxLocals` 算错 + 不追踪 reaching-def），
>   `src/unittest/mir/analyzer_probe_test.cpp` 的 6 个判定性测试全 `DISABLED_` 证明。
>   ADR/R1-B 决定 **reaching-def 自写，零 libjbytecode analysis 依赖**。本任务同样不动 libjbytecode analysis。
> - `ROADMAP.md` 写"reaching-def + dominance + φ 插入 + 重命名"**也过时**：R1a 用的是
>   **block-argument SSA**（ADR/R1-A），φ 隐式编码在前驱→后继边的 `SSASuccessor.args[]`/`SSABlock.args[]`，
>   **不做 dominance 计算、不做显式 φ 插入、不做变量重命名**。

## Owner

- Line: mj2x
- Branch: `feat/mj2x/r1-l3-register-ir`
- Base: `dev.3`（或 `main`，视 mj2x AI 当前 base）

## Context

mj2x 的主路线是「L1 栈式 MIR → L2 SSA → L3 寄存器 IR → L4 变换 → L5 RegC」。
R1a 已交付 L2（SSA），R1b 已交付 preprocessor。**L3 是 R1 的最后一块拼图**：
把 `SSAFunction`（操作数含 block arg / SSA value）lower 成 `RegModule`（操作数为扁平的 `(RegId, Type)`）。

L3 落地后，IR 操作数不再携带 stack slot，c2j 锚点 2（`CSTACK_RE` 正则）对 RegIR 失效——
这是 REDESIGN.md §1.3 / §4.1 的核心着力点，也是后续 R4 DCE 能真正生效的前提
（栈式 IR 每条指令都"写 cstack"是副作用，DCE 删不掉；寄存器化打破它）。

**必读文档（按优先级）**：
1. `mj2x/doc/ARCHITECTURE_REDESIGN.md` §0 ADR / §3 分层 / §4 防御矩阵 / §8 待决问题
2. `mj2x/doc/IR_DESIGN.md` §1 约束 / §6 block-arg φ / §9 RegId 即虚拟寄存器号 / §11 不变量
3. `mj2x/doc/SSA_ALGORITHM.md` §3 reaching-def / §5 翻译规则 / §9 R1 简化清单
4. `mj2x/doc/ARCHITECTURE.md` §2.3 数据流（含 L3 RegLowering）/ §3.4 L3 待决项
5. `mj2x/doc/TEST_STRATEGY.md:30,41`（L3 测试硬断言）
6. R1a 产物：`mj2x/src/mj2x/mir/ssa/` 全部 + `reaching_def.{hpp,cpp}`

## Scope

**必须做**：
- [ ] 设计 `RegId` 宽度、`Type` 枚举（是否复用 `PrimitiveKind`）、φ 节点表示
      （ARCHITECTURE.md §3.4 / REDESIGN.md §8.1 待决问题 1）—— 先出 header 骨架
- [ ] `src/mj2x/mir/register_ir/` 下新建：
      - `reg_value.hpp`（RegOperand：`v<id>` + Type）
      - `reg_instruction.hpp`、`reg_block.hpp`、`reg_function.hpp`、`reg_module.hpp`
      - `reg_lowering.hpp` / `.cpp`（`SSAFunction → RegModule`，含 φ 消解 + 虚拟寄存器分配）
- [ ] φ 消解：把 `SSABlock::args[]` + `SSASuccessor::args[]` 物化为扁平 `v<id>` 局部变量
      （block args 不再出现在最终 RegIR，IR_DESIGN.md §9：value id 即虚拟寄存器号）
- [ ] `src/unittest/mir/register_ir_test.cpp`（TEST_STRATEGY.md:30 要求的新测试）
- [ ] 在 `reg_lowering` 末端 dump `RegModule`，验证 c2j 锚点失效

**必须不做（属于后续 wave）**：
- ❌ L5 RegC 寄存器式 C 代码生成（R2）
- ❌ 精确类型推断（R1 用保守类型，R2 lowering 前补）
- ❌ 异常处理 SSA：带 try-catch 方法 **fail-closed 拒绝**（`SSAExceptionRegion` 留空，R2 填）
- ❌ synchronized 块异常锁泄漏（R2 RAII guard）
- ❌ ATHROW 后 JNI 控制流（R2）
- ❌ Native 运行时（R3）
- ❌ L4 变换 pass 框架（R4）
- ❌ 物理寄存器分配（C 局部变量无限，R1-C 明确不做传统线性扫描）

## Shared Contracts

- [ ] **不改 libjbytecode 公开 API**（reaching-def 自写、不用 analysis 子域）
- [ ] **不触发 CONTRACTS.md 更新**（R1 不动任何共享契约；ROADMAP.md:160 的 capability_map 更新是 R2 落地 RegC 实体时的事）

## Documentation

- [ ] `mj2x/doc/IR_DESIGN.md` 补 L3 章节（§12 文件组织当前只列了 `ssa/`，缺 `register_ir/`）
- [ ] `mj2x/dev_plan.md` §0 状态表更新：R1 标记完成，修正"待开始"措辞
- [ ] `mj2x/doc/ROADMAP.md` 修正 R1 描述里的过时措辞（dominance/φ 插入/重命名 → block-arg SSA；
      "复用 libjbytecode Frame/SourceValue" → reaching-def 自写）

## Validation

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x_unittest
xmake run mj2x_unittest
```

测试硬断言（TEST_STRATEGY.md:30/41 + IR_DESIGN.md §11）：
- [ ] dump `RegModule`，grep 不到 `cstack`/`sp`/`cclasses` 任何 c2j 锚点子串
- [ ] RegIR 操作数无 stack slot，全为 `(RegId, Type)` 扁平形式
- [ ] φ 已消解：block args 不出现在最终 RegIR
- [ ] 每个 `RegValue`/`SSAValue` 在 function 内 id 唯一（IR_DESIGN.md §11.1）
- [ ] 复用 R1a 三个 E2E 样本（`add`/`loopSum`/`loopCount`）跑通 lower 全链路

## Done Means

- L3 header 骨架 + `reg_lowering` 实现 + 测试全绿
- 至少 `add`/`loopSum`/`loopCount` 三个样本能 `MethodNode → L1 → SSA → RegModule` 完整跑通
- dump 验证 c2j 锚点 2（`CSTACK_RE`）对 RegIR 失效
- 文档过时措辞已修正，dev_plan 状态已同步

## 已知陷阱（开工前必读）

1. **block-arg SSA 不是经典 SSA**：没有显式 φ 节点，"φ"是前驱→后继边的传值。L3 的 φ 消解就是把
   这些边物化成 `v<id>` 局部变量，**不是传统 phi-elimination**。
2. **reaching-def 只跟踪 local slot**（不跟踪 stack slot）：栈生命期块内线性、跨块栈必须空（JVM StackMapTable）。
   stack 跨块合并（异常 handler 单元素栈）R1 fail-closed 拒绝。
3. **栈操作（DUP/SWAP）在 SSA builder 里已被吸收，不产 IR**（这是消除 cstack 锚点的核心机制）。
   L3 消费的是已经无栈的 SSA，不应再处理栈语义。
4. **`SSAOpaqueValue` 兜底**：R1a 对对象/数组/调用只建 IR、不细化（IR_DESIGN.md §4）。
   L3 样本若扩展到对象/数组/调用，注意这些只透传不解析。
5. **不要重新评估 libjbytecode analysis**：它有 bug 是已确认事实（ADR/R1-B），
   修 analysis 是另一次 spike，不在本任务范围。

# mj2x ROADMAP

> Product-line roadmap for the mj2x blue-team AI.
>
> **v2.1（2026-07-05 C backend 复活）**：撤销 ADR-1，CBackend 作为"易实现、广覆盖"通用后端
> 与 RegC 并行。复活决策与逐波覆盖路线见 `CBACKEND_REVIVAL_AND_WAVES.md`。
> **v2（2026-06-24 架构重构后）**：B0-B3 波次被 R0-R5 替换。
> 重构依据见 `ARCHITECTURE_REDESIGN.md`。v1 的 CBackend 主线已作废（归档 reference）。

---

## 1. Mission

mj2x owns native conversion and protection:

- bytecode → multi-layer IR (stack MIR → SSA → register IR → transform)
- register-style C/C++ code generation (RegC, 主路线)
- optional VMC4 native VM virtualization (可选支线, V2+/R5+)
- native runtime (crypto / antidebug / antijvmti / stringpool)
- protected sample generation

mj2x is the blue-team half of the morpher attack/defense loop. Its outputs must
eventually be attacked by mjdbg. **A wave is "done" only when mjdbg (red-team)
confirms the attack got harder, not when the implementation looks complete.**

---

## 2. Strategic Basis（重构依据）

详见 `ARCHITECTURE_REDESIGN.md §1`。摘要：

- c2j-native-deobfuscator 的攻击理论依赖两个锚点：JNI vtable offset + jvalue 索引模式
  （`cstack[N].i` / `cstrings[N]` / `cclasses[N]`）。
- 旧 CBackend 同时命中两锚点 → 系统性可还原 → 继续修补意义不大（ADR-1 归档）。
- 新主路线：通过 L2 SSA + L3 寄存器化消除锚点 2，L5 RegC + RT 消除锚点 1。
- VMC4 自研 VM 从主线降级为可选支线（ADR-3）。

---

## 3. Backend Lines（重构后）

### RegC（主路线）

Purpose:

- 寄存器式 C/C++ 代码生成，消除 cstack/clocal 锚点
- 命中 c2j Gap A（纯 native 算术）/ C（自定义符号 helper）/ D（常量折叠）

Stages（对应 R 波次）:

- R1: L2 SSA + L3 寄存器 IR 骨架
- R2: L5 RegC 生成 + E2E（生成 C 无 cstack 模式）
- R4: L4 变换 pass（MBA / 常量折叠 / DCE / CFF）

### VMC4（可选支线）

Purpose:

- 对极高价值方法提供 resident opcode 自变异 + opcode alias + 资源切片的额外抗分析层

Timing:

- 排在 R5+ 之后，RegC + RT 闭环后据 mjdbg 攻击反馈决定是否启动
- 设计见 `NATIVE_VM_ROADMAP.md`（已降级）

### CBackend（复活通用后端，2026-07-05 起）

- **撤销 ADR-1**：从 `backend/reference/` 移回 `backend/`，pipeline 重新注册，逐波扩指令覆盖
- 定位："易实现、广覆盖"通用后端，吃 L1 栈式 MIR，直接产 JNI C 源码
- **不承担 c2j 对抗职责**：生成的代码可被 c2j 还原（接受）；对抗强度留给未来的 RegC
- SSA 线（R1a/R1b 已合并）保留代码，但**不接入** C backend pipeline（C backend 直接吃 L1 MIR）
- 逐波覆盖路线（Wave 0-7）见 `CBACKEND_REVIVAL_AND_WAVES.md`
- 历史：v0.1 已交付 → ADR-1 归档 → 复活

---

## 4. Waves（R0-R5，替换 v1 的 B0-B3）

### Wave R0: 架构重构设计落地（本次，已完成）

Goal: 把竞品分析结论转化为可执行的新架构设计。

- `ARCHITECTURE_REDESIGN.md`（核心，保留/退役清单 + 五层分层 + 防御矩阵）
- `ARCHITECTURE.md` v0.2（摘要，修正 doc drift）
- `NATIVE_VM_ROADMAP.md` 降级（VMC4 主线→可选支线）
- `ROADMAP.md` / `dev_plan.md` / `TEST_STRATEGY.md` 改写
- 无代码改动

Done when: 6 份文档 review 通过，分支 `spike/mj2x/arch-redesign`。

### Wave R1: L2 SSA + L3 寄存器 IR 骨架（首块代码落地）

Goal: 在 L1 栈式 MIR 之上建立 SSA 与寄存器化，消除 stack slot 锚点。

- `src/mj2x/mir/ssa/`（reaching-def + dominance + φ 插入 + 重命名）
- `src/mj2x/mir/register_ir/`（RegIR 节点 + φ 消解 + 虚拟寄存器分配）
- 复用 libjbytecode analysis 的 Frame/SourceValue（参考 masxinlingvonta 路径）
- 顺带补齐 L1 缺口：INVOKEDYNAMIC / MULTIANEWARRAY preprocessor 提取
- 单元测试：SSA 构造正确性、寄存器化消除栈槽

Done when: 一个简单方法 MIR→SSA→RegIR，dump 显示操作数为 `(RegId, Type)` 无 stack slot。
此时 c2j 的 `CSTACK_RE` 已对 RegIR 失配（可手工验证）。

### Wave R2: L5 RegC 寄存器式 C 生成 + E2E

Goal: 从寄存器 IR 生成可编译、无 cstack 模式的 C/C++。

- `src/mj2x/backend/regc/`
- MIR subset（常量/局部/算术/控制流/返回）→ RegC → 可被 MSVC 编译
- 符号解析走自定义 helper（非全局 `cclasses[]` 表）
- 不发 "Cannot invoke" throw-reason 字符串
- E2E：Java 方法 → mj2x → .c → cl → 运行 → 结果对比

Done when: 生成 C 经 Ghidra 反编译无 `cstack[N]` 模式；行为与原 Java 一致。

### Wave R3: RT 运行时最小可用

Goal: 建立 native 运行时，打破 c2j 锚点 1（JNI hook）+ Gap B（运行时密钥池）。

- `runtime/crypto/`（SHA256 / HMAC-SHA256 / AES-128，纯 C 无 OpenSSL）
- `runtime/kernel/`（HKDF-SHA256 派生 + 资源 HMAC 校验 + volatile wipe）
- `runtime/antidebug/`（Windows PEB：`__readgsqword(0x60)` + NtGlobalFlag + NtQueryInformationProcess）
- `runtime/antijvmti/`（`env->functions` 篡改检测 + RuntimeMXBean 扫 `-javaagent`）
- `runtime/stringpool/`（运行时派生密钥解密的字符串池）
- 与 RegC 集成：符号 helper、字符串池解密
- 已删除 `runtime/main.cpp`（DLL demo）、`rt_insn`/`rt_memory`（空壳）；R3 core 改为
  `mj2x-runtime-core` + `mj2x_runtime_test`

Done when: 受保护样本在 c2j agent 下 fail-closed，或 trace 显著退化（与 mjdbg 协作验证）。

### Wave R4: L4 变换 pass 系统

Goal: 强化静态路径抗性，使命中 c2j Gap A/D 的变换系统化。

- `src/mj2x/mir/passes/`（pass 框架：schema + Kahn 拓扑排序 planner，确定性）
- 实现 4 个 pass：
  - 常量折叠/扩散（Gap D）
  - 死代码消除（寄存器化后真正生效）
  - MBA 混合布尔算术（Gap A 纯 native 算术）
  - 控制流平坦化 + 不透明谓词（seed 派生）

Done when: 变换后 RegC 经 Ghidra 反编译，c2j 静态 lifter 产出退化（`?.?` stub 或语义损失）。

### Wave R5+: mjdbg 闭环 + 可选 VMC4

Goal: 进入攻防闭环；视反馈决定是否启动 VMC4 深度保护支线。

- 产受保护样本给 mjdbg 攻击
- 据 mjdbg 报告加硬化（每个反馈至少一个硬化改动 + 再生样本）
- 可选：VMC4 自研 VM 作为高价值方法深度保护支线（设计见 `NATIVE_VM_ROADMAP.md`）

Done when: mjdbg 攻击报告显示恢复难度相对前一版样本上升。

---

## 5. Shared Contracts

| Contract | Other owner | Rule |
|---|---|---|
| protected sample format | mjdbg | sample 必须文档化 expected trace surface |
| trace expectations | mjdbg | 隐藏或随机化信号前先协调 |
| libjbytecode usage | mbase | 直接用 OK；共享 wrapper 需协调 |
| runtime build options | build owner | 不静默加重默认依赖（仍无 LLVM） |
| c2j 攻击靶单 | mjdbg | `ARCHITECTURE_REDESIGN.md §4` 防御矩阵每条需 mjdbg 验证 |

R1 落地 RegC 实体后，更新 root `doc/capability_map.md` 与 `doc/CONTRACTS.md`（本次不动）。

---

## 6. Validation

Baseline（重构后，命令不变）:

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
```

For native runtime changes (R3+), document:

- OS / compiler / toolchain
- JDK if JNI is involved
- whether behavior was compared with original Java
- whether c2j agent was used to validate anti-JVMTI（与 mjdbg 协作）

---

## 7. Documentation Duties

mj2x AI maintains:

- `prompt/mj2x.md`
- `doc/ROADMAP.md`（本文）
- `doc/ARCHITECTURE_REDESIGN.md`（★ 重构期最高优先级）
- `doc/ARCHITECTURE.md`
- `dev_plan.md`
- `doc/NATIVE_VM_ROADMAP.md`（已降级）
- `doc/TEST_STRATEGY.md`
- `doc/MVP_PLAN.md`（冻结历史）

Update root `doc/capability_map.md` only when mj2x creates a shared capability
that other lines should discover and reuse.

Update root `doc/CONTRACTS.md` when protected sample format, VM bytecode format,
runtime ABI, or mjdbg-facing trace expectations change.

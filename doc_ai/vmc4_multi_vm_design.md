# VMC4 多虚拟机后端设计

> 设计文档：VMC4 后端的目标形态——**可变多虚拟机（Themida 风格）**——以及它如何接入 mj2x 现有后端契约。
> 代码尚未实现，本文是设计与实施依据。
>
> 分工（每件事只有一处权威）：
> - **本文**：VMC4 的整体设计——为什么要做多 VM、VM 实例与指令集建模、dispatcher 策略、per-method/per-class 可变性、接入点、与 JavaShroud/Themida 的范式差异与改进。
> - `ir_design.md` §13：VMC4 复用的 L1 → SSA → RegIR 前端（不在本文重复指令 lowering 细节）。
> - `regc_codegen.md`：RegC 的生成策略与 fail-closed 哲学，VMC4 继承同一套语义保真/fail-closed 原则。
> - `phantom_shield_codegen_notes.md` §7-8：Themida `VM_TIGER_WHITE` 现实参照物（二进制后处理范式）。
>
> 状态：设计稿，未进入 `dev_plan.md` 待办。实现前需经 mjdbg 红队反馈确认 RegC/runtime 闭环已稳定（见 `doc/roadmap.md:66`）。

---

## 1. 背景与动机

### 1.1 mj2x 现状：VM 后端尚未存在

VMC4 在仓库里只以"暂不作为主线"的文字出现：

- `doc/roadmap.md:46` ——「VMC4 | 高价值方法可选 VM 虚拟化支线 | 暂不作为主线」
- `doc/roadmap.md:66` ——「不把 VMC4 提前拉回主线，除非 RegC/runtime 闭环证明需要」
- `doc/mvp_release_scope.md:98` ——「VMC4 native VM backend and VM runtime」列在 **Explicitly Out of Scope**
- `doc/architecture.md:147` ——「VMC4 as the default path」列在 **Non-Goals**
- `dev_plan.md` ——当前待办（R5/R6/R6.1/R6.2）**无 VMC4 条目**

现有三个后端（`mj2x-core/src/mj2x/backend/`）全部是"把 Java 字节码翻译成 native 源码"的**直接翻译器**，没有任何 opcode 编码、handler table、dispatch loop、解释器循环这类 VM 解释器基础设施：

| 后端 | 目录 | 范式 |
|---|---|---|
| StackC | `backend/stackc/` | 栈式 JNI C，正确性基线 |
| RegC | `backend/regc/` | 无栈寄存器式 C/C++，保护主线 |
| LLVM IR | `backend/llvmir/` | 受限的 textual IR |

**结论**：VMC4 是从零建一个新后端，不是改造现有结构。但后端挂载契约（`backend.hpp:48`）和前端 IR（L1/SSA/RegIR）都已就绪，新后端是"插件式接入"。

### 1.2 为什么目标定为 Themida 风格多 VM，而不是单一固定 VM

业界两条 VM 混淆路线：

- **Tigress/Themida**：每个受保护函数生成**结构不同的独立 VM**——不同指令集编码、不同 dispatcher 算法、不同 handler 顺序。逆向者对一个 VM 的理解**无法跨函数复用**，每个函数都要重新分析。这是真正的"多 VM"。
- **单一解释器 + 外围可变封装**：所有函数共享同一个 ISA 和解释器内核，只在 opcode 别名、resource 路径、native 二进制变形上做差异化。逆向者拿到一份内核就能反推完整指令集，**跨样本/跨函数可复用分析**。

我们对 JavaShroud（`C:\sw\JavaShroud`）做了深度核实，它的 VM 属于第二类，且"多 dispatcher"基本未落地——详见 §2.2。如果 mj2x 照抄，会继承它的硬伤：**ISA 跨样本可恢复**。

VMC4 的目标是**第一类**：per-method 或 per-class 粒度的可变 VM 实例，让红队（mjdbg）对每个 VM 实例的分析成本相互独立。

---

## 2. 参照物分析

### 2.1 Themida（phantom_shield 逆向佐证）

`phantom_shield_codegen_notes.md` §7-8 记录了 Themida SDK 的 VM 化机制：

- `:202` —— 内嵌 `ThemidaSDK.h` + `SecureEngineCustomVMs`（`VM_TIGER` 标记）
- `:205-209` —— `VM_TIGER_WHITE_START/END` 包裹生成代码，配合 `SecureEngineCustomVMs` 的**编译期 VM 化**

**范式差异（关键）**：Themida 是**二进制后处理**（对已编译 native 加 VM 壳），mj2x 是**源码到源码**（产 C）。所以 mj2x 的"VM 化"不能照搬 Themida 的 SDK 标记 + 后处理壳，而要落地为"**生成一个 C 写的解释器 + 编码后的 opcode 字节数组**"。这个范式差异决定了 VMC4 的全部实现选择（见 §4.1）。

### 2.2 JavaShroud —— 反面教材（哪些不要做）

对 `C:\sw\JavaShroud` 的深度核实结论：**单一固定解释器内核 + 外围可变封装**，宣称与实现的差距很大。

| JavaShroud 宣称 | 代码实际 | 位置 |
|---|---|---|
| 6 种 dispatcher 形态（switch/threaded/call/if-nest/interpolation） | `js_vm_dispatcher.c` **全是 TODO 占位**，真正只有一种 switch/if 链 | `core-engine/src/main/native/js_vm_dispatcher.c:34-106` |
| 多 dispatcher profile（0-5） | profile 1-5 的 case 匹配里 `& 0u` 掩码，**退化为 profile 0** | `js_vm_core.c:5456-5499` |
| opcode 随机化 | `generateOpcodeMapping()` 只覆盖 16 个 const opcode，~130 个 canonical 编号固定 | `MethodVirtualizationTransforms.kt:667-679` |
| 解释器可变 | 内核 `js_vm_core.c`（7276 行手写）逻辑固定，只在**源码文本层**注入 junk/重排 handler | `NativeRecompilationTransforms.kt:1053-1082` |
| 每构建独立 opcode 映射 | opcode 映射在方法循环**之外**调用，是"每构建一份"而非"每方法一份" | `MethodVirtualizationTransforms.kt:211-212` |

JavaShroud 的真正多变性来自：ALT 别名 opcode、super-operator 折叠、resident opcode 自修改 mask、per-method profile salt、native 源码级变形。这些都是"外围噪声"，**ISA 本身跨样本可恢复**——拿到一份 native 库就能反推完整指令集和 handler 语义。

**VMC4 明确不采用 JavaShroud 的路线**。它的内核固定 + 外围噪声模式适合"抬高一次性分析成本"，不适合 mj2x 的红蓝对抗目标（mjdbg 会对同一保护产物反复攻击，内核可复用 = 强度归零）。

### 2.3 借鉴点

尽管不照抄，JavaShroud 有几个可借鉴的设计：

- **canonical opcode + ALT 别名**：同一语义多个 opcode 编码。VMC4 可用，但作为 per-VM 实例的**可变维度之一**，而非全局固定别名。
- **super-operator 折叠**：把多条指令折叠成一条复合指令。可作为 per-VM 实例的指令集膨胀维度。
- **resident opcode 自修改 mask**：dispatch 后 rewrap opcode。可作为 handler 行为可变维度。
- **per-method profile salt**：影响 dispatch 行为。VMC4 把它升级为"per-method 独立 VM 实例"。

---

## 3. 设计目标与非目标

### 3.1 目标

1. **多 VM 实例**：受保护方法按 per-method 或 per-class 粒度获得**结构不同的 VM 实例**（不同 opcode 编号映射、不同 handler 顺序、不同 dispatcher 算法）。一个 VM 实例的分析结论不适用于另一个。
2. **ISA 与解释器随机化**：opcode 编号、handler 实现形态、dispatcher 调度方式都由 seed 驱动生成，不是固定常量。
3. **复用现有前端**：VMC4 消费 L1 → SSA → RegIR 前端（与 RegC 共享），不重新从 JVM 字节码建模。RegIR 只有 13 个 instruction family（`reg_instruction.hpp:21-36`），是 VM opcode 建模的干净输入。
4. **复用现有后端契约**：实现 `mj2x::Backend` 子类（`backend.hpp:48`），产物仍是"改写 class + C 源码"，仍走 ACC_NATIVE + RegisterNatives（`regc_backend.hpp:23-28`），不引入新的 class 注入机制。
5. **fail-closed**：继承 RegC 的语义保真/fail-closed 原则（`regc_codegen.md` §1）。任何无法保证语义正确性的情况保留字节码、不转 VM。

### 3.2 非目标

- **不做 Themida 式二进制后处理壳**。VMC4 是源码生成器，产物是 C 解释器 + opcode 数组，不是对已编译 native 加壳。
- **不做全 AOT**。VMC4 只虚拟化选定的高价值方法，不是把整个 jar 转 VM。
- **不宣称绝对不可逆**。目标是抬高跨样本/跨函数分析复用成本，与 mjdbg 红队形成闭环量化。
- **VMC4 不作为默认后端**。`--backend vmc4` 显式选择，与 StackC/RegC/LLVM 并列。

---

## 4. 架构设计

### 4.1 范式定位：源码到源码的 VM 化

```text
受保护 Java 方法
  → L1 MIR（CodeConverter，与 RegC 共享）
  → SSA（ssa_builder，与 RegC 共享）
  → RegIR（reg_lowering，与 RegC 共享）
  → 【VMC4 分叉点】RegIR → opcode 流编码（VmEncoder，按 VmInstance 描述）
  → 【VMC4 分叉点】VmInstance 描述 → C 解释器源码（VmInterpreterEmitter）
  → 产物：每方法的 opcode 字节数组 + 共享/独立的解释器 + JNI wrapper
  → 标 ACC_NATIVE + RegisterNatives 绑定（复用现有机制）
```

**分叉点在 RegIR 之后**。RegC 在这里发无栈 C；VMC4 在这里把 RegInstruction 编码成 opcode，并按 VmInstance 描述生成解释器。两条路径前端完全共享，只在后端发射形态分叉。

### 4.2 核心抽象：VmInstance（VM 描述对象）

VMC4 的中心抽象是 **VmInstance**——一个 VM 实例的完整描述，由 seed 驱动生成，包含：

```text
VmInstance
├── opcodeTable      // RegInstructionKind → opcode 编号的映射（随机化）
├── handlerOrder     // handler 在 dispatch 表里的排列顺序（随机化）
├── dispatcherKind   // dispatcher 算法（switch / 函数指针表 / computed goto / if-nest）
├── operandEncoding  // operand 编码格式（定长/变长、字节序、是否带类型 tag）
├── aliasSet         // 该实例的 ALT 别名 opcode（每实例独立，非全局固定）
├── superOpSet       // 该实例启用的 super-operator 折叠规则
├── maskScheme       // resident opcode 自修改 mask 方案（可选）
└── seed             // 驱动上述所有维度的种子
```

**关键不变量**：

- **一个 VmInstance 对应一份解释器源码 + 一份 opcode 编码规则**。同一个 VmInstance 保护的所有方法共享这套规则；不同 VmInstance 的规则相互独立。
- **VmInstance 的粒度可配**：per-method（每个方法一个独立 VM，最强可变性，代码膨胀最大）或 per-class（同类方法共享一个 VM，平衡可变性与体积）。
- **seed 是确定性的**：相同 seed + 相同前端 IR 生成相同的 VmInstance，保证可复现构建与单测可断言。

### 4.3 指令集建模：RegIR → opcode

VMC4 不从 JVM 字节码的 200+ 条指令建模，而是从 RegIR 的 13 个 instruction family 建模（`reg_instruction.hpp:21-36`）：

| RegInstructionKind | VM opcode 语义 | 备注 |
|---|---|---|
| Copy | 寄存器间移动 | phi/块参数消除后的显式拷贝 |
| Unary | 取负/位取反 | `SSAUnaryOp` |
| Binary | 算术/位运算 | `SSABinaryOp`，含整数环绕/除零语义 |
| Compare | 比较并产生分支条件 | `SSAComparePredicate` |
| Convert | 类型转换 | `SSAConvertKind`，含基本类型互转 |
| LoadField / StoreField | 字段读写 | `SSAFieldRef` |
| ArrayLoad / ArrayStore | 数组读写 | 带元素类型 |
| Call | 方法调用 | `SSAMethodRef`，invokevirtual/special/static/interface |
| Allocate | new / newarray | `SSAAllocationKind` |
| Intrinsic | JDK intrinsic / barrier | `intrinsicName`，复用 RegC intrinsic registry |
| Barrier | monitor / 安全点 | `barrierReason` |

控制流（SSA block 的 terminator）单独建模为 branch/switch/return opcode。每个 family 的具体 opcode 数量由 VmInstance.aliasSet 和 superOpSet 膨胀——例如 Binary 在某实例可能有 1 个 canonical + 2 个 ALT 别名 + 1 个 super-op（折叠 Binary+Compare），在另一实例可能只有 1 个 canonical。

### 4.4 Dispatcher 策略（多形态真实落地）

这是与 JavaShroud 拉开差距的关键维度。VMC4 的 dispatcherKind 是**真实生成的不同 C 结构**，不是占位 TODO：

| dispatcherKind | 生成的 C 结构 | 逆向特征 |
|---|---|---|
| SwitchTable | `switch(opcode){case...}` | 经典跳表，IDA 可识别为 switch |
| FuncPtrTable | `handlers[opcode](ctx)` 函数指针表 | 间接调用，需重建函数指针表 |
| ComputedGoto | `goto *labels[opcode]`（GCC 扩展） | threaded code，需平台支持 |
| IfNest | `if(opcode==A){...}else if(opcode==B)...` | 无跳表，线性 if 链 |
| StateDispatcher | state 寄存器 + 平坦化 dispatcher（类似 RegC CFF） | 控制流平坦化 |

每实例选一种（由 seed 决定）。**同一个 VM 化方法在不同构建里可能用不同 dispatcher**，红队针对 switch 跳表的分析工具对 FuncPtrTable 无效。

> 注意：ComputedGoto 依赖 GCC 扩展 `Labels as Values`，MSVC 不支持。CLI 需提供 dispatcher 白名单选项，默认在 MSVC 下排除 ComputedGoto。

### 4.5 可变性维度矩阵

一个 VmInstance 的"可变性"来自以下维度的 seed 驱动随机组合：

| 维度 | 可变内容 | 跨实例独立性 |
|---|---|---|
| opcode 编号 | RegInstructionKind → 0..255 的映射 | 每实例独立 |
| handler 顺序 | dispatch 表/handler 函数的排列 | 每实例独立 |
| dispatcher 算法 | §4.4 五种之一 | 每实例独立 |
| operand 编码 | 定长/变长、字节序、类型 tag | 每实例独立 |
| ALT 别名 | 同语义多 opcode | 每实例独立（非 JavaShroud 的全局固定） |
| super-operator | 指令折叠规则 | 每实例独立 |
| handler 形态 | 同语义 handler 的不同 C 实现（等价变形） | 每实例独立 |
| bogus handler | 无效 handler 注入比例 | 每实例独立 |

**对比 JavaShroud**：JavaShroud 的 opcode 映射是"每构建一份"且只覆盖 16 个 opcode（§2.2）；VMC4 的 opcode 映射是"每实例一份"且覆盖全部 13 个 family。这是"多 VM"与"伪多 VM"的本质区别。

### 4.6 产物形态

```text
生成的 C 翻译单元（finalize 输出）
├── VM 解释器（per-class 一份，或 per-method 一份，取决于粒度配置）
│   ├── handler 函数集（按 VmInstance.handlerOrder 排列）
│   ├── dispatch loop（按 VmInstance.dispatcherKind 生成）
│   └── opcode 解码器（按 VmInstance.operandEncoding）
├── 每方法的 opcode 字节数组（const unsigned char vm_code_<hash>[] = {...}）
├── 每方法的 JNI wrapper（标 ACC_NATIVE + RegisterNatives 绑定，复用 regc 机制）
└── RegisterNatives 注册表（finalize 发射，复用 regc 的 cloader/Loader 路径）
```

粒度选择的影响：

- **per-class**：同类方法共享一个解释器，体积可控，类内方法间 VM 规则共享（红队破解一个类的一个方法后，同类其他方法的 VM 规则已知）。
- **per-method**：每方法独立解释器，最强可变性，代码膨胀最大（每个方法都带一份解释器源码）。适合高价值方法。

建议默认 **per-class + 高价值方法 per-method**（类似 JavaShroud 的 CriticalPlus 策略，但粒度真正独立），由 CLI/注解控制。

---

## 5. 接入点

VMC4 接入 mj2x 现有契约，改动面可控。按依赖顺序：

### 5.1 构建脚本（无需改）

`mj2x/xmake.lua:11` 的 `add_files("src/mj2x/**.cpp")` 会自动纳入新目录。新建 `mj2x-core/src/mj2x/backend/vmc4/` 即可，**无需改 xmake.lua**。

### 5.2 后端契约（无需改）

实现 `mj2x::Backend` 子类（`backend.hpp:48`），实现 5 个虚函数即可：`name()` / `compileMethod()` / `injectIntoClass()` / `finalize()` / `generatedSource()`。`NativeSourceLanguage` 枚举（`backend.hpp:23`）复用 `C99`——VM 解释器是 C 输出。

### 5.3 前端复用（无需改）

VMC4 从 Pipeline 拿 L1 block（`pipeline.cpp:90` 的 `converter.convert(...)`），自建 SSA/RegIR（复用 `ssa_builder` + `reg_lowering`），在 RegIR 之后分叉。前端代码零改动。

### 5.4 CLI 接线（3 处必改）

- `mj2x-cli/src/main.cpp:65-70` —— `enum class CEmitterBackend` 加 `Vmc4`
- `mj2x-cli/src/main.cpp:158-173` —— `parseBackendName` 加 `"vmc4"` 分支
- `mj2x-cli/src/main.cpp:829-853` —— 实例化分支加 `std::make_unique<Vmc4Backend>(...)`

### 5.5 per-method 后端选择（新能力，需新增）

当前是"全方法二选一标 native"（`pipeline.cpp:96-100` 对所有方法用同一个 backend 列表）。VMC4 的 per-method/per-class 粒度需要这层选择能力：

- **方案 A（最小侵入）**：VMC4 后端内部维护"哪些方法进 VM、哪些 fail-closed 保留字节码"的筛选逻辑，Pipeline 不变。粒度由注解或 CLI 传入 Vmc4Backend。
- **方案 B（通用化）**：Pipeline 支持 per-method backend 路由（一个方法可指定不同后端）。改动面大，但对未来多后端组合有用。

建议 MVP 先用方案 A，把 per-method 选择封装在 Vmc4Backend 内部。

### 5.6 注解触发（可选）

复用 `mj2x-annotations` 机制（`Native.java`），新增 `@VmProtected` 或扩展 `@Native` 带 `backend="vmc4"` 参数。MVP 可先用 CLI 全局开关 `--backend vmc4`，注解触发后置。

---

## 6. 随机化与可复现

### 6.1 seed 注入

VMC4 需要一个 seed/RNG 注入点（当前后端无此机制，RegC 的 MBA/CFF 是固定变换不带 seed，见 `reg_transform_passes.hpp`）。设计：

- CLI：`--vmc4-seed <uint64>` 显式指定，用于可复现构建与单测断言。
- 默认：未指定时从构建时间戳 + 输入 jar 哈希派生，保证不同构建产物不同。
- per-method/per-class 派生：主 seed + 方法/类身份哈希 → 该方法/类的 VmInstance seed。保证同构建内不同方法/类的 VM 实例相互独立。

### 6.2 可复现构建

相同 `--vmc4-seed` + 相同输入 → 相同 VmInstance → 相同解释器源码 + 相同 opcode 数组。这是单测可断言的前提（断言固定 seed 下生成的解释器结构特征，如 handler 数量、dispatcher 形态）。

### 6.3 确定性约束

VmInstance 的所有随机维度都必须从 seed 确定性派生，**不得引入未播种的随机源**（如系统 `rand()`、未播种的 `mt19937`）。否则破坏可复现构建。

---

## 7. 失败策略与语义保真

继承 RegC 的 fail-closed 哲学（`regc_codegen.md` §1）：

- **VMC4 fail-closed 点**：RegIR 指令含 VM 无法编码的语义（如未支持的 intrinsic、未支持的 invokedynamic 形态）→ 保留字节码、不标 native、留可观测日志 `[Vmc4Backend] Skipping <method>: <reason>`。
- **语义保真优先于可变性**：任何可变性维度（opcode 随机化、dispatcher 形态、bogus handler）都**不得改变指令语义**。例如整数溢出环绕、除零抛异常、NaN 比较、引用同一性比较必须在 VM handler 里精确复刻，和 RegC helper 一致。
- **bogus handler 不得执行**：注入的无效 handler 只能污染静态分析（增加逆向看到的 handler 数量），**不得在运行时被 dispatch 命中**（否则改变语义）。这点与 JavaShroud 的 `& 0u` 退化不同——JavaShroud 的 profile 1-5 是"匹配但不执行"，VMC4 的 bogus handler 是"存在但 dispatch 表里不指向它"。

---

## 8. 与 mjdbg 红队的闭环

VMC4 的保护强度必须由 mjdbg 量化验证（`doc/roadmap.md:10-18` 的红蓝闭环）：

- **expected trace surface**：每个 VMC4 样本必须写明红队应观察到的 trace 特征（dispatcher 形态、handler 数量、opcode 编码熵）。
- **跨实例分析复用性测试**： mjdbg 对方法 A 的 VM 分析结论是否适用于方法 B 的 VM。若适用，说明粒度/可变性不足。
- **强度归零基准**：若 mjdbg 能自动化还原某个 VmInstance 的 ISA，该实例的强度记为 0，反馈调整可变性维度。

VMC4 进入主线的前置条件：RegC/runtime 闭环已稳定 + mjdbg 提供 c2j/VM 分析 agent（见 `dev_plan.md` R5 阻塞说明）。

---

## 9. 实施分期建议

按风险与依赖排序的分期（供后续进入 `dev_plan.md` 时参考）：

### Phase 0：单 VM MVP（验证管线）
- 单一固定 VmInstance（seed 固定、dispatcher 固定为 SwitchTable、无别名/super-op）
- RegIR → opcode 编码器 + 最小解释器（switch dispatch）
- 复用 RegC 的 fail-closed 与语义 handler
- 目标：端到端跑通一个简单方法，证明源码到源码 VM 范式可行
- 验证：e2e round-trip + 解释器产物可编译可执行

### Phase 1：多 dispatcher 形态
- 实现 §4.4 五种 dispatcherKind 的真实生成
- CLI `--vmc4-dispatcher <kind>` 选择
- 目标：证明 dispatcher 可变性可落地（对比 JavaShroud 的 TODO 占位）

### Phase 2：opcode 映射随机化 + per-instance seed
- VmInstance 抽象 + opcodeTable/handlerOrder 随机化
- per-class 粒度（同类方法共享实例）
- 可复现构建（`--vmc4-seed`）
- 目标：证明"多 VM 实例"的核心价值——跨实例 ISA 独立

### Phase 3：完整可变性维度
- ALT 别名、super-operator、handler 形态等价变形、bogus handler
- per-method 粒度（高价值方法独立 VM）
- 目标：可变性维度矩阵全部落地

### Phase 4：红队闭环 + 强度量化
- mjdbg 对 VMC4 样本产出 weakness report
- 跨实例分析复用性测试
- 强度归零基准与可变性调参

---

## 10. 待决问题（实现前需确认）

1. **per-method 粒度的代码膨胀上限**：每方法一份解释器源码，体积膨胀几何？是否需要"解释器模板 + per-method 数据表"的混合形态（一份解释器代码 + 每方法独立的 opcode 表/handler 顺序数据）来控制体积？这会影响 VmInstance 抽象的"代码"与"数据"边界。
2. **computed goto 的平台支持矩阵**：MSVC 不支持 `Labels as Values`，ComputedGoto dispatcher 需平台守卫。是否接受"Windows 默认排除 ComputedGoto"？
3. **与 RegC 的 handler 复用边界**：VMC4 的语义 handler 与 RegC 的 `_mj2x_regc_*` helper 是否共享实现？共享可减少重复但增加耦合。
4. **invokedynamic 在 VM 中的支持路径**：RegC 的自定义 bootstrap 已支持（`regc_invokedynamic_strategy.md`），VMC4 是否直接复用 RegIR 的 `Intrinsic("indy:<index>")` 透传，还是在 VM 层重新建模？
5. **注解粒度控制**：`@VmProtected` 是否需要支持 per-method 配置（如 `dispatcher=...`、`seed=...`），还是全靠 CLI 全局控制？

这些问题留待实现启动时在 `dev_plan.md` 具体任务里决策，本文只记录"需要决"。

---

## 11. 实现状态（Phase 0+1 已落地）

**MVP 范围**（Phase 0+1）已完成并通过单测验证。代码位于 `mj2x-core/src/mj2x/backend/vmc4/`，单测在 `unittest/src/backend/vmc4_backend_test.cpp`（10/10 通过）。

### 已实现

| 组件 | 文件 | 说明 |
|---|---|---|
| 指令集定义 | `vm_opcode.hpp/.cpp` | VmOpcode 枚举（13 family + 6 terminator 的子集）、VmType/VmUnaryOp/VmBinaryOp/VmComparePred、VmDispatcherKind、VmProgram |
| VM 实例抽象 | `vmc4_instance.hpp/.cpp` | VmInstance + Vmc4Options。MVP 固定 opcode 映射，仅 dispatcherKind/seed 有差异 |
| RegIR→opcode 编码器 | `vm_encoder.hpp/.cpp` | 两遍编码、字面量常量池去重、block 目标解析、fail-closed |
| 解释器源码发射 | `vm_interpreter_emit.hpp/.cpp` | runtime prelude（mj2x_vm_slot + JVM div/rem helper）+ 5 种 dispatcher 形态 + 数据数组发射 |
| 后端 facade | `vmc4_backend.hpp/.cpp` | compileMethod/finalize/injectIntoClass，镜像 RegCBackend 结构，复用前端链路与 RegisterNatives 绑定 |
| CLI 接线 | `mj2x-cli/src/main.cpp` | `--backend vmc4` + `--vmc4-dispatcher <kind>` + `--vmc4-seed <hex>` |
| 单元测试 | `unittest/src/backend/vmc4_backend_test.cpp` | 编码成功/fail-closed/解释器源码结构/5 种 dispatcher 特征 |

### MVP 支持范围

- **计算 family**：Copy / Unary / Binary / Compare / Convert
- **控制流 terminator**：Jump / Branch / Return / Unreachable
- **类型**：I32 / I64 / F32 / F64（BOOL/I8/I16/CHAR 归一到 I32）
- **5 种 dispatcher**：SwitchTable / FuncPtrTable / ComputedGoto / IfNest / StateDispatcher（全部真实落地，非占位）

### MVP fail-closed 边界（保留字节码）

- LoadField / StoreField / ArrayLoad / ArrayStore / Call / Allocate / Intrinsic / Barrier / Throw / Switch
- 含对象引用（REF）参数/返回的方法
- 构造器 `<init>` / `<clinit>`

### Phase 1 待决问题的实际决策

- **问题 1（代码膨胀）**：MVP 采用"解释器模板 + per-VM 数据表"形态——5 种 dispatcher 共享 handler 逻辑片段，只在外层调度结构上不同，体积可控。per-method 独立解释器源码留 Phase 3。
- **问题 2（computed goto）**：emit 阶段生成 `goto *` + `&&label`（GCC 扩展）。MSVC 编译生成的 C 会失败，但 emit（C++ 生成 C 文本）不依赖 GCC。CLI 默认 switch_table，显式指定 computed_goto 时由调用方保证用 GCC/clang 编译。
- **问题 3（handler 复用）**：MVP 不复用 RegC 的 `_mj2x_regc_*` helper。VMC4 handler 内联进 dispatcher，语义（整数环绕、除零、UShr）独立实现，与 RegC 的 `binaryExpr()`（`regc_emitter.cpp:1033`）行为对齐但代码独立。
- **问题 4（invokedynamic）**：MVP fail-closed（Intrinsic family 不编码）。后续 Phase 直接复用 RegIR 的 `Intrinsic("indy:<index>")` 透传，与 RegC 阶段3 已支持的 indy 解耦。
- **问题 5（注解粒度）**：MVP 用 CLI 全局控制（`--vmc4-dispatcher`/`--vmc4-seed`）。`@VmProtected` 注解触发留后续。

### 后续 Phase（未实现，记此跟进）

- **Phase 2**：opcode 映射随机化 + per-instance seed 驱动 + per-class 粒度（VmInstance.opcodeTable/handlerOrder 真正生效）
- **Phase 3**：ALT 别名、super-operator 折叠、bogus handler、handler 形态等价变形、per-method 独立解释器源码
- **Phase 4**：mjdbg 红队闭环 + 跨实例分析复用性测试 + 强度量化
- **e2e 测试**：`unittest/src/backend/vmc4_backend_e2e_test.cpp`（需 JDK + MSVC 工具链，镜像 `regc_backend_e2e_test.cpp` 的 fixture），验证 Java→VM→native→Java 端到端 round-trip

# mj2x — 翻译管线技术演进谱系

> 状态: 长期设计资产。本文梳理 mj2x 把 JVM 字节码搬到 native 层的**技术能力代际演进**，
> 从最初的直译栈字节码一路到 JVMTI 深度优化与内存池。
>
> 关系: 与 `roadmap.md`（R0-R5 波次，回答"做哪些"）互补，本文回答"每一类技术能力如何一代代演进、
> 为什么是这个顺序"。波次是交付节奏，演进谱系是技术脉络；某一代能力可能横跨多个波次。
> 当前已落地的代际见各节"现状"标注，权威已实现清单以 `doc_ai/capabilities.md` 为准。

---

## 演进总览

```
Gen 1 直译栈字节码            — 正确性基线，每条指令对应一次 JNI
  │
  ▼
Gen 2 符号缓存                — class/method/field ID 缓存，减少 JNI 查找
  │
  ▼
Gen 3 基元 intrinsic          — 包装类/算术不发 JNI，native 等价内联
  │
  ▼
Gen 4 字符串 intrinsic        — String LDC/parseInt/toString native 化
  │
  ▼
Gen 5 容器映射 + Object→Any   — HashMap/HashSet/ArrayList native 映射，Object 收口为 Any
  │                              （仅内部、不与 JVM 交互的范围）
  │
  ▼
Gen 6 调用语义全覆盖          — invokespecial/invokestatic/invokevirtual 完整 native 语义
  │
  ▼
Gen 7 invokedynamic 自实现    — 自建一套 CallSite/MutableCallSite 语义，只处理内部
  │
  ▼
Gen 8 JVMTI 深度优化 + CallSite 对象化
  │                            — 借助 JVMTI 自设计 CallSite（封装成对象，非裸函数指针），
  │                              runtime 与代码生成器结合；简单 RAII 即可
  ▼
Gen 9 内存池                  — 申请/释放统一管理，解决 native 侧内存生命周期
```

**正交支线 — 部分虚拟化（VM）**: 与翻译主线并行，可选叠加。既是保护强度路线（指令集自定义、
常量池加密、opcode alias），也可能是性能路线（Java 栈式语义不好优化，寄存器式 VM 生成的代码可能
更快）。见 [§部分虚拟化支线](#部分虚拟化支线vm)。

**正交支线 — 运行时反侦测**: 反 jvmtp/IDE、反 java agent（arthas 类）、反 jvmti dump agent、
反 native 调试器（x64dbg 类）。与翻译代际独立，属于运行时硬化。见
[§运行时反侦测体系](#运行时反侦测体系issue-411)。

**贯穿原则**: Gen 1-4 解决"少发 JNI、提性能、缩攻击面"；Gen 5-7 解决"内部代码零 JVM 交互"；
Gen 8-9 解决"深度优化与运行时可靠性"。每一代都以前一代为前提，不跳级。

---

## Gen 1 — 直译栈字节码（正确性基线）

**目标**: 把每条 JVM 栈指令一对一翻译成等价的 C/JNI 代码，先求"能跑、行为对"。

**技术形态**:
- 用 `union jvalue stack[]` + `int sp` 在 C 里重建操作数栈
- 每条 `getfield`/`invoke*`/`*aload` 都翻译成一次对应 JNI 调用
- 每个 JNI 调用现场查找 class/method/field ID

**现状**: ✅ 已落地（CBackend，25 个 E2E 覆盖）。定位是"广覆盖正确性基线"，**不承担对抗强度**
（同时命中 c2j 的两个锚点：JNI vtable offset + jvalue 索引模式，系统性可还原 —— 这是 RegC 主路线要消除的）。

**为什么是第一代**: 没有正确性基线，后续所有优化都无法验证是否保持语义。这是所有后端的"对照基准"。

---

## Gen 2 — 符号缓存（减少 JNI 查找）

**目标**: 同一个 class/method/field ID 在一次调用里反复查找是纯浪费，缓存掉。

**技术形态**:
- class/method/field ID 首次解析后缓存（按 owner+name+desc 三元组）
- 缓存生命周期绑定到 JNIenv 或类卸载事件
- 静态字段/方法的 ID 可跨调用复用

**现状**: ⚠️ 未落地。CBackend 当前每次 `GetMethodID`/`GetFieldID`（capabilities.md 已记录此限制）。
RegC 的 `_mj2x_regc_*` helper 同样未缓存。

**为什么在 Gen 1 之后**: 必须先有正确的直译路径，才能在不改语义的前提下把"查找"和"使用"分离、
安全地插入缓存层。

---

## Gen 3 — 基元 intrinsic（包装类/算术不发 JNI）

**目标**: 当调用 `Math.sin` / `Integer.parseInt` / 包装类算术时，不发 JNI，直接内联 native 等价实现。

**技术形态**:
- runtime 提供 native 实现（`mj2x_math_*` / `mj2x_int_*`）
- 代码生成器在 Call 分支识别 JDK 方法签名 → 直接发 runtime 符号调用
- 仅边界（JNI 入口 wrapper）保留 JNI；intrinsic 调用点零 JNI

**现状**: ✅ Math/parseInt 已落地（RegC E2E 覆盖，`regc_intrinsic.md`）。包装类 valueOf/intValue、
Random 因涉及对象逃逸表示（native struct ↔ jobject 边界转换）暂走 JNI，待 Gen 5 解决对象表示后接入。

**为什么在 Gen 2 之后**: 基元/算术是最纯的"无对象逃逸"场景，验证"识别签名 → 内联 native"这套机制时不被
对象生命周期问题干扰。是 intrinsic 机制的安全起点。

---

## Gen 4 — 字符串 intrinsic（String LDC/parse/toString）

**目标**: String 字面量、parseInt、toString 不走 JNI 方法派发。

**技术形态**:
- String LDC 经 `_mj2x_intrinsic_string_literal`（NewStringUTF）物化为 jstring —— **这是 String intrinsic
  唯一碰 JNI 的地方**
- parseInt 经 `_mj2x_intrinsic_parse_int`（GetStringUTFChars + strtol），解析全程在 native
- toString 反向（int → NewStringUTF）

**现状**: ✅ String LDC 贯通 + parseInt 已落地。toString 待补。

**为什么在 Gen 3 之后**: 字符串是第一个"必须经过 JNI 边界一次"（因为 jstring 是 JVM 对象）的 intrinsic 类型。
Gen 3 的纯算术 intrinsic 完全不碰 JNI，先把识别机制跑通；Gen 4 才引入"边界唯一 JNI、内部纯 native"的模式，
为后续涉及对象的 intrinsic 建立模板。

---

## Gen 5 — 容器映射 + Object → Any（内部零 JVM 交互）

**目标**: `HashMap`/`HashSet`/`ArrayList` 三个核心容器映射为 native 实现；`Object` 收口为类似 `Any` 的
native 抽象，**仅用于一块范围内部、不与 JVM 交互的代码**。

**技术形态**:
- runtime 提供 `mj2x_hashmap`/`mj2x_hashset`/`mj2x_arraylist`（native 数据结构）
- 代码生成器识别这些容器的分配/操作，发 native 调用而非 JNI
- `Object` 在"确认为内部范围"的代码里映射为 `mj2x_any`（类型擦除的 native 容器）
- **逃逸检测**: 一旦容器/Any 对象要跨 JNI 边界（作为 native 方法返回值、或传入未覆盖的 JDK 方法），
  降级为 hybrid —— 该对象回退 JNI，其余仍 native

**现状**: ❌ 未落地。是 Gen 4 之后下一步重点。前置依赖：对象逃逸分析（判断哪些对象不跨边界）。

**为什么在 Gen 4 之后**: 容器内部会大量用到基元和字符串（put/get/contains 的 key/value），Gen 3/4 让这些
元素操作已经 native 化，容器本身再 native 化才有意义。且容器是"算法密集、JVM 交互少"类的核心依赖，
Gen 5 直接铺向"整类下沉"（roadmap R4+）。

**这是性能拐点**: Gen 1-4 提升"单次调用"的 JNI 表面；Gen 5 起，"大量转换时的内部性能"才真正拉开 ——
一个内部循环里 HashMap 操作全 native，不再每个 put/get 都跨 JNI。

---

## Gen 6 — 调用语义全覆盖（invokespecial/static/virtual）

**目标**: 把 `invokespecial`/`invokestatic`/`invokevirtual` 的完整 JVM 语义在 native 侧实现一遍，
**只处理内部、不跟外部 Java 代码交互的部分**。

**技术形态**:
- 内部方法调用（被调方法也在 native 编译范围内）直接走 native 函数指针，不发 JNI
- `invokevirtual` 需要方法分派 → 自建 vtable 或 dispatch table
- `invokespecial`（构造器/super/private）走确定的直接调用
- 外部方法（被调方不在 native 范围）仍走 JNI（fail-open 到 Gen 1 的 call helper）

**现状**: ⚠️ 部分。RegC 当前内部 static/static→native 调用已支持（`CallHelpersRoundTrip` E2E），
但 virtual 分派、构造器链的完整 native 语义未做。

**为什么在 Gen 5 之后**: 一旦容器和对象能 native 化（Gen 5），方法调用就成了下一个 JNI 源头 ——
一个 native 类内部调另一个 native 类的方法，现在却绕道 JNI。Gen 6 让"内部调用零开销"。

---

## Gen 7 — invokedynamic 自实现

**目标**: `invokedynamic`（lambda、方法引用、字符串拼接）的 CallSite 语义自己实现一套，**只处理内部、
不跟外部 Java 交互的部分**。

**技术形态**:
- 自建 CallSite / MutableCallSite 的 native 表示
- bootstrap method 解析在 native 侧完成（对内部 lambda）
- 识别到 invokedynamic 指向内部代码时，native 解析；指向外部（反射、动态代理）时 fail-closed 走 JNI 或拒绝

**现状**: ❌ 未落地。已有 preprocessor 把 invokedynamic 提取到合成方法（`regc_invokedynamic_strategy.md`），
但"自实现 CallSite 语义"是更深的阶段。

**为什么在 Gen 6 之后**: invokedynamic 是"调用"的最复杂形式，依赖前序所有调用语义都已 native 化。
跳过 Gen 6 直接做 indy 会留下调用链上的 JNI 断点。

---

## Gen 8 — JVMTI 深度优化 + CallSite 对象化

**目标**: 借助 JVMTI 做更深层次的优化；自设计一套 CallSite，对应 JVM 的 CallSite，但**不直接调用函数指针，
而是封装成对象**，以便 runtime 和代码生成器结合。

**技术形态**:
- CallSite 是对象（带元数据、guard、版本号），不是裸函数指针
- runtime 持有 CallSite 表，代码生成器生成的调用经 CallSite 对象间接跳转
- JVMTI 提供运行时信息（已加载类、方法内联决策、去优化触发）
- 简单 RAII 管理 CallSite 生命周期（构造注册、析构注销）
- 这一层让"运行时优化"和"静态生成的代码"能结合 —— 例如 guard 失败时回退到慢路径

**现状**: ❌ 未落地，是较远的阶段。

**为什么在 Gen 7 之后**: 需要先有完整的调用语义（Gen 6/7），CallSite 对象化才有载体。JVMTI 深度优化是
"在已经零 JNI 的内部代码之上再叠优化"，不是补功能。

**与保护强度的关系**: CallSite 对象化让控制流数据（跳转目标）从"代码里的立即数"变成"runtime 持有的对象"，
对静态分析更难还原 —— 但这要等 mjdbg 红队验证才算数（不预先声称保护强度）。

---

## Gen 9 — 内存池（申请/释放统一管理）

**目标**: 解决 native 侧的内存生命周期问题，搞内存池，统一处理申请和释放。

**技术形态**:
- Gen 5 的容器、Gen 8 的 CallSite 对象、包装类对象等都会在 native 侧分配内存
- 引用计数（简单 RAII）作为初版 GC（roadmap R3 已规划）
- 内存池化：按对象类别分池（容器池、CallSite 池、临时缓冲池），减少 malloc/free 碎片
- 边界 wipe：敏感缓冲用 `kernel::secureWipe` 清零（runtime 已有）

**现状**: ❌ 未落地。runtime 已有 `kernel::secureWipe` 和 HKDF 派生，但无内存池。

**为什么在最后**: 内存问题是"量"的问题，不是"质"的 —— Gen 1-8 先把功能跑通（少量 malloc 可接受），
Gen 9 再统一收口性能与生命周期。提前做内存池会因为对象模型（Gen 5/8）还在变而反复重写。

---

## 代际与波次的对应（参考，非锁死）

| 代际 | 对应 roadmap 波次 | 备注 |
|---|---|---|
| Gen 1 直译 | 已完成（v0.1 → 复活） | CBackend 基线 |
| Gen 2 缓存 | 未排期 | 待提上 |
| Gen 3 基元 intrinsic | R2-R3（部分已交付） | Math/parseInt 已通，包装类待 Gen 5 |
| Gen 4 字符串 intrinsic | R3（部分已交付） | LDC/parseInt 已通 |
| Gen 5 容器 + Any | R4+ | 整类下沉的前置 |
| Gen 6 调用语义 | R4+ | virtual 分派为主 |
| Gen 7 invokedynamic | R5+ | 依赖 Gen 6 |
| Gen 8 JVMTI + CallSite | R5+ | 深度优化支线 |
| Gen 9 内存池 | R5+ long-term | 量变收口 |

> **正交支线**（不依赖 Gen 1-9，可独立启动/叠加）：
> - **部分虚拟化（VM）**：R5+ 可选支线（ADR-3），保护强度 + 性能双路线，设计见 `native_vm_roadmap.md`
> - **运行时反侦测（issue #411）**：四类威胁（jvmtp/IDE、java agent、jvmti dump、native 调试器），
>   现有 antidebug/antijvmti 初版切片，待纵向加深

代际是技术依赖关系，波次是交付节奏。某代能力可在其依赖的代完成后择机插入最近的波次。

---

## 部分虚拟化支线（VM）

> 与翻译主线（Gen 1-9）**正交**：可独立演进、可选叠加到任意代际之上。设计详见
> `doc/native_vm_roadmap.md`（已降级为可选支线，ADR-3）。

### 双重定位

部分虚拟化同时是**保护强度路线**和**性能路线**，这两点经常被混淆，要分开看：

**作为保护强度路线**（更常被讨论的）：
- 自定义指令集 → 静态反汇编失效（攻击方得先逆向 opcode 表）
- 常量池加密 / opcode alias / resident opcode 自变异 → 提高符号恢复难度
- 对极高价值方法提供比 RegC 更深的保护层

**作为性能路线**（容易被忽略的一点）：
- Java 字节码是**栈式**语义，栈机不好做寄存器分配、指令调度这类优化
- 如果 VM 设计成**寄存器式**（而非栈式），虚拟分发 + 寄存器式解释/JIT 产出的代码，
  **可能比直译栈字节码更快**——因为生成代码的优化空间更大
- 即"VM 未必性能差"。竞品 jvmtp/jumanji144 走 VM 虚拟化主线推进很快，部分原因正是
  寄存器式 VM 在生成代码质量上有优势

### 为什么是"部分"虚拟化

不做全量 VM 化（把整个类都塞进 VM），而是**部分**：
- 只把极高价值方法（密钥处理、核心算法）虚拟化
- 其余方法仍走 RegC 翻译主线
- VM 字节码与 RegC 的 native 代码共存，边界明确

这与 roadmap 的定位一致：VMC4 是"排在 R5+ 之后，RegC + RT 闭环后据 mjdbg 攻击反馈决定是否启动"
的可选支线。

### 与翻译代际的关系

- VM 支线**不依赖** Gen 1-9 的任何一代，可独立启动
- 但 VM 支线**受益于** Gen 5（容器/Any）和 Gen 9（内存池）：VM 解释器内部也要分配对象、管理内存
- 启动时机：建议在 RegC + RT 闭环后（mjdbg 已能攻击 RegC 样本），用 VM 给"被攻破的方法"加第二层

### 现状

❌ 未启动。`doc/native_vm_roadmap.md` 是设计稿，VMC4 V0（executable correctness）未落地。

---

## 运行时反侦测体系（issue #411）

> issue #411 要求 runtime 添加一系列侦测性功能。与翻译代际（Gen 1-9）和虚拟化支线**正交**：
> 它硬化的是"运行时环境"，不是"代码生成质量"。可叠加到任意代际的产物上。
>
> 现有初版切片：`mj2x-runtime/src/antidebug/`（IsDebuggerPresent + CheckRemoteDebuggerPresent）、
> `mj2x-runtime/src/antijvmti/`（JNI table 篡改检测 + `-javaagent`/`-agentpath`/`-agentlib`/JDWP 参数扫描）。
> 这只覆盖了 issue 的一部分，离完整侦测体系差距明显。

### 四类威胁与侦测方向

| # | 威胁（issue #411） | 例子 | 现有覆盖 | 待补侦测方向 |
|---|---|---|---|---|
| 1 | 反 jvmtp 调试（IDE 类 attach） | IntelliJ IDEA 远程调试 | ❌ 无 | JDWP 握手检测、`TransportServer` 端口扫描、调试线程枚举 |
| 2 | 反 java agent（非 agentpath 参数） | arthas、bytekit | ⚠️ 仅参数扫描 | 运行时扫描已附加 agent（`Instrumentation`、transformer 链）、ClassFileTransformer 枚举、不能光靠启动参数 |
| 3 | 反 jvmti agent（dump 手段） | mjt-dump 这类 JVMTI dump | ⚠️ 仅 JNI table 篡改 | JVMTI 能力查询（`CanAccessLocalFrame`/`CanGenerate...`）、agent load 事件、已加载 agent 枚举 |
| 4 | 反 native 调试器 | x64dbg、WinDbg | ⚠️ 仅 IsDebuggerPresent | PEB 检查（`NtGlobalFlag`、`BeingDebugged`）、`NtQueryInformationProcess`（`ProcessDebugPort`/`ProcessDebugFlags`）、断点指令扫描（`0xCC`）、硬件断点 DR 寄存器检查、调试对象句柄枚举 |

### 设计原则（反侦测）

1. **不能光靠启动参数**：issue #411 第 2 条明确点名——arthas 这类可以 `attach` 到已运行的 JVM，
   不经 `-javaagent` 启动参数。必须**运行时**扫描已附加的 agent / transformer，而非只查命令行。
2. **分层、可组合**：每类侦测返回信号 flag（沿用现有 `antidebug::DebugSignals` 模式），由策略层
   组合决定 fail-closed 反应。不把"侦测"和"反应"耦合在一个函数里。
3. **平台抽象**：Windows-specific 的 PEB/NtQuery 走 `platform.h` 抽象，Linux 的 ptrace/`/proc` 同理，
   保持 `mj2x-runtime-core` 不直接依赖 OS API 的可测试性。
4. **可 mock**：现有 antijvmti 的"快照 + 比较"模式是好范例——侦测函数接受可注入的输入（fake vtable、
   fake agent 列表），单测无需真实调试器。
5. **红队认定强度**：与所有保护特性一样，反侦测的强度提升必须由 mjdbg 红队验证后才算数（不预先声称）。

### 与现有 antidebug/antijvmti 的关系

现有模块是 R3 初版切片，覆盖 issue #411 的极小一部分：
- `antidebug::collectDebugSignals` 只查 `IsDebuggerPresent`/`CheckRemoteDebuggerPresent` → 远不够 native 调试器侦测（需 PEB、NtQuery、断点扫描）
- `antijvmti::containsAgentOption` 只扫启动参数 → 远不够 java agent 侦测（需运行时 transformer 枚举）
- `antijvmti::isJniTableStable` 检测 JNI table 篡改 → 是 jvmti agent 的一小部分

**演进策略**：在现有模块上**纵向加深**每类侦测，而非新建平行模块。issue #411 的四类威胁分别对应
现有 antidebug（#1、#4）和 antijvmti（#2、#3）的扩展。

### 现状

⚠️ 初版切片已落地（单测覆盖）。完整四类侦测体系未实现，是 issue #411 的待办。

---

## 设计原则（贯穿所有代际）

1. **fail-closed 边界**: 每一代遇到"与外部 Java 交互"的部分（反射、动态代理、类加载、未覆盖 JDK 方法）
   一律降级走 JNI 或拒绝，不静默产出错误代码（沿用 `prompt/mj2x.md` 边界）。
2. **内部优先**: 所有 native 化优化只针对"内部范围"（不逃逸到 JVM 的代码），绝不声称全 AOT。
3. **正确性优先于性能**: 每一代必须有 E2E 与 Java 基准逐项对比，行为一致才算交付。
4. **保护强度由红队认定**: Gen 5-8 的对抗收益（减少 JNI 表面、CallSite 对象化）需 mjdbg 攻击验证后才算
   "变强"，不在实现阶段预先声称。
5. **渐进、不跳级**: 每一代以前一代为前提；缓存（Gen 2）未做就直接做 CallSite（Gen 8）会埋下语义回归。

# mj2x — Architecture Design

> Morpher Java Bytecode to X
> 多后端 JVM 字节码 → native 保护编译器
> 设计版本: **v0.2** （2026-06-24 架构重构后）
>
> **重要**: v0.2 反映 `architecture_redesign.md` 确立的五层 IR 分层。
> v0.1 的 CBackend 主线定位曾作废（ADR-1），已于 2026-07-05 复活为"易实现、广覆盖"
> 通用后端，与 RegC 并行。对抗强度留给 RegC，CBackend 不承担 c2j 对抗职责。
> 本文是摘要性文档；权威分层定义、保留/退役清单、防御矩阵见
> `architecture_redesign.md`。本文与该文档冲突时以 redesign 文档为准。

---

## 1. Overview

### 1.1 目标

将 JVM 字节码（`.class` / `.jar`）经过多层中间表示变换，生成为抗逆向分析的 native 产物。
mj2x 是 morpher 攻防闭环的**蓝队**：产出受保护样本供 mjdbg（红队）攻击，据反馈硬化。

主路线（ADR-3，见 redesign 文档 §0）：

```
字节码 → [L1] 栈式 MIR → [L2] SSA → [L3] 寄存器 IR
       → [L4] IR 变换 pass → [L5] 寄存器式 C/C++ 生成
                                       │
                                       ▼
                            [RT] native 运行时（crypto / antidebug / antijvmti / stringpool）
```

设计目标不是"翻译器"，而是"系统性地移除 c2j-native-deobfuscator 依赖的两个锚点"
（JNI vtable offset + jvalue 索引模式）。详见 redesign 文档 §1。

### 1.2 非目标

- 通用 Java AOT 编译器（不处理 GC、多线程等完整运行时）
- 字节码验证（由 libjbytecode analysis 层覆盖）
- 100% JVM 指令语义兼容（特定边界行为有差异，同 masxinlingvonta）
- LLVM 依赖（`dependencies.md` 明确无 LLVM；SSA/寄存器分配自研）

### 1.3 已知语义不兼容（fail-closed 边界，沿用 `prompt/mj2x.md`）

| 场景 | 行为 | 处理时机 |
|------|------|---------|
| `synchronized` 块内抛异常 | `MONITOREXIT` 被跳过 → 锁泄漏 | R2 起用 C++ RAII guard 修复（构造 `MonitorEnter` / 析构 `MonitorExit`，栈展开保证释放，PR#305 review） |
| `ATHROW` 后继续 JNI | `Throw` 不中断 C 控制流 | R2 起翻译为 `Throw + return` |
| `ACC_SYNCHRONIZED` 方法 | C 不能模拟 JVM monitor | R1 起仍拒绝，fail-closed |
| `INVOKEDYNAMIC` | CBackend 保留 bootstrap 元数据并生成 MethodHandle runtime bridge；RegC 后续展开 | CBackend 已覆盖 javac string concat/lambda |

### 1.4 参考项目（仅设计借鉴，不依赖）

- **masxinlingvonta** — 栈 IR → LLVM alloca → mem2reg → SSA 路线；其"用 ASM SourceInterpreter 拿 reaching-def"思路被 L2 复用，但 SSA/寄存器分配自研而非外包 LLVM
- **JavaShroud** — VBC4 虚拟机 + 运行时反调试 + 资源切片；其"减少 JNI 暴露面""运行时密钥绑定""pass 框架（schema + 拓扑排序 planner）"设计被 RT/L4 借鉴
- **c2j-native-deobfuscator**（攻击方）— 其两锚点理论是 mj2x 防御设计的反向输入

---

## 2. Architecture

### 2.1 Pipeline（v0.2 五层）

```
          ┌─────────────────────────────────────────────────────────┐
          │                      Pipeline                            │
          │                                                          │
  .class ──▶  InputLoader (复用)                                     │
  / .jar  │       │                                                  │
          │       ▼                                                  │
          │  CompilerIndex (复用, 含 Utf8StringArena)                 │
          │       │                                                  │
          │       ▼                                                  │
          │  Preprocessors (插拔式)                                   │
          │       │                                                  │
          │       ▼                                                  │
          │  [L1] CodeConverter → 栈式 MIR (复用)                     │
          │       │                                                  │
          │       ▼                                                  │
          │  [L2] SSA 构造 (★新增)                                   │
          │       │                                                  │
          │       ▼                                                  │
          │  [L3] 寄存器 IR (★新增)                                  │
          │       │                                                  │
          │       ▼                                                  │
          │  [L4] IR 变换 pass (★新增: 常量折叠/DCE/MBA/CFF)          │
          │       │                                                  │
          │       ▼                                                  │
          │  Backend (插拔式)                                         │
          │       │  [L5] RegC → 寄存器式 C/C++ (★主路线)            │
          │       │  CBackend → cstack C (归档 reference, 不默认注册)│
          │       │                                                  │
          │       ▼                                                  │
          │  Postprocessors (插拔式)                                  │
          │       │                                                  │
          │       ▼                                                  │
          │  OutputWriter (复用)                                      │
          └─────────────────┬────────────────────────────────────────┘
                            │
              ┌─────────────┴─────────────┐
              │                           │
        Hybrid JAR                  .c / .cpp + native runtime
        (native stubs               (crypto / antidebug / antijvmti /
         + 资源)                     stringpool, ★ 重写)
```

每层的职责、设计要点、为什么是这个顺序，见 `architecture_redesign.md §3`。

### 2.2 阶段说明

| 阶段 | 输入 | 输出 | 职责 |
|------|------|------|------|
| **Load** | `.class` / `.jar` | `CompilerIndex` | 解析字节码，建类/方法/字段索引（复用，含 arena） |
| **Preprocess** | `CompilerIndex` | 修改后 `CompilerIndex` | 标记待编译方法、提取复杂指令（INVOKEDYNAMIC/MULTIANEWARRAY） |
| **L1 Convert** | `MethodNode` | `Block[]` + `MIRNode[]` | 字节码 → 后端无关栈式 IR（复用 frontend） |
| **L2 SSA** | `Block[]` | SSA 形 `Block[]` | reaching-def + dominance + φ 插入 + 重命名（★新增） |
| **L3 RegIR** | SSA `Block[]` | 寄存器 IR | φ 消解 + 虚拟寄存器分配（★新增） |
| **L4 Pass** | 寄存器 IR | 变换后寄存器 IR | 常量折叠/DCE/MBA/CFF（★新增） |
| **L5 Backend** | 寄存器 IR | `BackendResult` | RegIR → 寄存器式 C/C++（★主路线） |
| **Postprocess** | `CompilerIndex` + 结果 | 修改后 `CompilerIndex` | 设 ACC_NATIVE、注入 loader |
| **Write** | `CompilerIndex` | JAR / .c / .dll | 序列化输出（复用） |

### 2.3 核心数据流

```
.class bytes
    │
    ▼
libjbytecode::ClassReader → ClassNode (tree API)
    │
    ▼
[L1] CodeConverter::convert(MethodNode)
    │   ├── 遍历 InsnList + 分析控制流 + 构建基本块
    │   └── 每条指令 → MIRNode (栈式, 操作数含 stackSlot/localIndex)
    ▼
vector<Block*>  (栈式 MIR)
    │
    ▼
[L2] SsaBuilder::build(blocks)
    │   ├── reaching-def (复用 libjbytecode SourceInterpreter 风格)
    │   ├── dominance 计算
    │   └── φ 插入 + 重命名
    ▼
SsaModule (SSA 形)
    │
    ▼
[L3] RegLowering::lower(ssa)
    │   ├── φ 消解
    │   └── 线性扫描寄存器分配 → 虚拟寄存器 vN
    ▼
RegModule (寄存器 IR, 操作数 = (RegId, Type))
    │   ★ 此处 cstack[N]/sp 锚点已消除
    ▼
[L4] PassPipeline::run(regModule)
    │   └── 常量折叠 / DCE / MBA / CFF (seed 派生)
    ▼
变换后 RegModule
    │
    ▼
[L5] RegCBackend::compileMethod(...)
    │   └── RegIR → 普通 C/C++ (局部变量 = reg, 无 jvalue 联合, 无 cstack 数组)
    ▼
BackendResult { .text = "C/C++ source", .kind = REGISTER_C_SOURCE }
```

---

## 3. Core Data Structures

> 公共 API 契约定义在 header 的 Doxygen 注释里，本节不重复字段表（遵循 `prompt/mj2x.md`
> "Do not write API tables in docs"）。这里只描述结构关系与设计约束。

### 3.1 CompilerIndex（复用，v0.1 drift 修正）

`CompilerIndex` 持有全局类/方法/字段索引与共享的 `Utf8StringArena`。
**v0.1 文档曾描述 `methodIndex_` / `fieldIndex_` 成员 —— 实际不存在**（doc drift）。
当前实现是 `classes_: map<string, CompilerClass>`，方法/字段通过所属类访问。
R1 若需要快速方法/字段反查，届时再加索引成员。

`CompilerClass` 持有 `ClassNode*`、`methods`/`fields` vector、`modified`/`isLibrary` 标志。
`CompilerMethod` 持有 `MethodNode*`、`markedForCompile`/`compiled`/`backendResult`。
`MethodID` / `FieldID` 是 `{owner, name, desc}` 三元组，可哈希。
`JarPassthroughEntry` 持有非 class zip 条目（name + data + compressionMethod），供 JAR 回写保真。

### 3.2 [L1] MIR（复用 frontend，后端无关栈式 IR）

`MIRNode` 是所有节点基类（含 `MIRKind kind`）。`mir_node.hpp` 定义了覆盖 JVM 指令的节点族
（常量/局部/栈操作/算术/转换/控制流/返回/对象/数组/字段/方法调用/类型/monitor/throw/frame）。

关键约束：
- **只含数据，不含代码生成逻辑** —— 每个子类仅携带操作数
- **后端按需遍历** —— L2/L3/L5 自行决定如何处理每种节点
- 操作数仍含 stack slot / local index（这是 L1 栈式特征，L3 寄存器化后消除）

已知 L1 缺口（R1 补齐）：`INVOKE_DYNAMIC` / `MULTI_NEW_ARRAY` 当前翻译为 `MIRNop` 占位，
对应 struct 未定义；R1 加 preprocessor 提取。

### 3.3 Block（复用）

`Block` 持有 `id`、`nodes: vector<unique_ptr<MIRNode>>`、`successors`/`predecessors`、
`exceptionHandler`。非 copyable（`unique_ptr` 成员）。`ExceptionHandler` 持有
`startLabel`/`endLabel`/`handlerLabel`/`catchType`。

CFG 由 `CodeConverter::analyzeControlFlow` 从 jump/switch 指令的 label target 构建。
L2 SSA 在此 CFG 上做 dominance。

### 3.4 [L2] SSA / [L3] 寄存器 IR（★新增，R1 落地）

节点结构、`RegId` 宽度、φ 表示、`Type` 枚举是否复用 `PrimitiveKind` 等**留给 R1 header 设计**，
不在本文预先定义（见 redesign 文档 §8 待决问题）。设计约束：
- SSA 形必须能在 L1 Block CFG 上构造，复用 libjbytecode analysis 的 reaching-def 信息
- 寄存器 IR 操作数必须是 `(RegId, Type)`，不得保留 stack slot 下标 —— 这是打破 c2j 锚点 2 的硬约束

### 3.5 [L4] Pass 框架（★新增，R4 落地）

借鉴 JavaShroud：`ModuleDefinition`（schema: params/ordering/compatibility）+ Kahn 拓扑排序
planner（确定性，稳定 tie-break）。schema 格式（TOML 还是 C++ 结构体）R4 决定。

---

## 4. Backend Interface

`Backend` 抽象接口定义在 `backend.hpp`（`name()` / `compileMethod()` / `injectIntoClass()` /
`finalize()`）。`BackendResult` 定义在 `backend_result.hpp`。

`BackendResult::Kind` 当前为 `{ NONE, C_SOURCE, BINARY }`。**v0.1 文档曾列出 `LLVM_IR` —— 实际不存在**（doc drift）。
R2 落地 RegC 时加 `REGISTER_C_SOURCE`。

### 4.1 [L5] RegC Backend（★主路线，R2 落地）

从寄存器 IR 生成普通 C/C++：
- 局部变量 = SSA 寄存器，**无 `union jvalue`，无 `cstack[]` 数组**
- JNI 调用收敛：类/方法/字段解析走 runtime helper（非全局 `cclasses[]` 表）
- **不发 "Cannot invoke X.Y(args)" throw-reason 字符串**（c2j `throw_reason.py` 的唯一兜底）
- 纯计算（算术、比较）直接生成 C 表达式，不经 JNI（命中 c2j Gap A）

实现细节（节点结构、helper ABI）R2 定。

### 4.2 CBackend（[ARCHIVED reference]）

`c_backend.{hpp,cpp}` 是 v0.1 的唯一后端，用 `union jvalue stack[N]` + `int sp` 模拟操作数栈，
每个字段/方法/对象操作都翻译为 JNI 调用。**它同时命中 c2j 的两个锚点，系统性可还原**
（见 redesign 文档 §1.2），原 ADR-1 归档决策已撤销（2026-07-05）：

- ADR-1 已撤销：CBackend 复活，保留在 `backend/`，pipeline 重新注册，逐波扩指令覆盖
- 定位："易实现、广覆盖"通用后端，不承担 c2j 对抗职责（对抗强度留给 RegC）
- 其 11 个测试保留
- 撤销理由与逐波路线见 `cbackend_revival_and_waves.md`

---

## 5. Module Layout（v0.2）

```
mj2x/
├── xmake.lua                         — 构建入口（mj2x-core / mj2x-cli / mj2x_unittest）
├── doc/
│   ├── architecture_redesign.md      — ★ 重构权威设计（v0.2 最高优先级）
│   ├── architecture.md               — 本文档（v0.2 摘要）
│   ├── roadmap.md                    — R0-R5
│   ├── dependencies.md               — 依赖（无 LLVM）
│   ├── _archive/                     — 历史文档（MVP_PLAN, NATIVE_VM_ROADMAP 等）
│   └── ...
├── src/
│   ├── mj2x/
│   │   ├── mj2x.hpp                  — 聚合头
│   │   ├── core/                     — Pipeline + CompilerIndex（复用 + 扩展）
│   │   ├── io/                       — InputLoader / OutputWriter（复用）
│   │   ├── frontend/                 — [L1] 栈式 MIR（复用）
│   │   ├── mir/                      — ★ [L2] ssa/ + [L3] register_ir/ + [L4] passes/（R1/R4 新增）
│   │   ├── preprocessor/             — 诊断/改写入口；CBackend 保留 INVOKEDYNAMIC 元数据
│   │   ├── postprocessor/            — 复用（注入 loader）
│   │   └── backend/
│   │       ├── backend.hpp           — 接口（复用）
│   │       ├── backend_result.hpp    — 复用 + 扩展 Kind
│   │       ├── regc/                 — ★ [L5] 寄存器式 C 生成（R2 主路线）
│   │       └── reference/            — CBackend 归档（R1 移入）
│   ├── mj2x-cli/                     — CLI 入口
│   └── unittest/                     — 测试（分层见 TEST_STRATEGY）
└── runtime/                          — ★ [RT] 重写（R3）
    └── src/
        ├── crypto/                   — SHA-256 / HMAC-SHA256 / AES-128 / constant-time compare
        ├── kernel/                   — HKDF-SHA256 + HMAC verify + volatile wipe
        ├── antidebug/                — debugger signal probes
        ├── antijvmti/                — JNI function-table tamper check + agent option scan
        ├── stringpool/               — HKDF-derived string-pool decoder
        ├── runtime_entry.cpp         — shared-library wrapper symbol
        ├── rt_str_obf.{h,cpp}        — legacy XOR helper retained for compatibility
        ├── jni_wrapper.h
        └── platform.h
```

> `runtime/main.cpp`（Windows DLL 注入 demo）和 `rt_insn`/`rt_memory` 空壳已在 R3 runtime core 清理。

---

## 6. Build Integration

构建配置见 `mj2x/xmake.lua` 与 `mj2x/runtime/xmake.lua`。R1-R4 落地新模块时按现有模式
扩展 `add_files`。依赖约束：仍无 LLVM，无 OpenSSL；crypto 集成审计过的轻量库（PR#305 review，
候选 tiny-AES-c / Mbed TLS / BearSSL 精简子集），R3 落地时在 `dependencies.md` 锁定具体选型。

依赖图（v0.2，R3 后）：

```
mj2x-cli ──▶ mj2x-core ──▶ libjbytecode ──▶ libca_core
                                      ├──▶ libtyped
                                      └──▶ libcrypto
                         ──▶ libjstr
                         ──▶ libjzip
mj2x_unittest ──▶ mj2x-core + test_helper + gmock
runtime(jni_lib) ──▶ JDK（仅 runtime 编译需要）
```

---

## 7. 与 v0.1 的差异摘要

| 维度 | v0.1 | v0.2 |
|------|------|------|
| 自我定位 | "C 翻译器" | "多后端 native 保护编译器，主路线寄存器式 C/C++" |
| 主后端 | CBackend（cstack/sp JNI C） | RegC（寄存器式 C/C++） |
| CBackend | 唯一后端 | 归档 reference，不默认注册 |
| IR 层次 | 单层栈式 MIR | 五层：L1 栈式 → L2 SSA → L3 寄存器 → L4 pass → L5 生成 |
| 运行时 | 几乎空（DLL demo + stub） | 重写：crypto/kernel/antidebug/antijvmti/stringpool |
| VMC4 自研 VM | 与 CBackend 并行主线 | 降级为 V2+ 可选支线 |
| 防御设计依据 | 无竞品分析 | 以 c2j 两锚点理论为反向输入（见 redesign §1） |
| doc drift | `methodIndex_/fieldIndex_`、`LLVM_IR` 误述 | 已修正 |

# mj2x — Architecture Redesign

> 状态: **Spike / 设计探索** — 分支 `spike/mj2x/arch-redesign`
> 日期: 2026-06-24
> 关系: 本文档是 mj2x 架构重构的权威设计 note，覆盖 `architecture.md` v0.1 与
> `native_vm_roadmap.md` 中已被本次决策推翻的部分。后续 R1 起的代码 PR 以本文档为准。
> 依据: `ref_samples/竞品分析报告.md`、`ref_samples/调研参考.md`、对 `ref_samples/`
> 下 JavaShroud / c2j-native-deobfuscator / masxinlingvonta / native-obfuscator 源码的直接分析。

---

## §0 决策记录 (ADR)

本次重构基于一次竞品分析（见 `ref_samples/竞品分析报告.md`），核心结论是：
现有 CBackend（现 StackC）走的"字节码 → `cstack[N]`/`sp` JNI C 直译"路线，正是攻击工具
c2j-native-deobfuscator 系统性还原的靶子。继续在这条路线上打补丁意义不大。

四项关键决策（来自设计评审）：

| # | 决策 | 选择 | 落地含义 |
|---|------|------|---------|
| ADR-1 | 现有 CBackend（MIR→`cstack[]`/sp JNI C）如何处理？ | ~~归档为 reference，不再演进~~ **【已撤销，2026-07-05；2026-07-08 更名 StackC】** | 复活为"易实现、广覆盖"通用后端，现位于 `backend/stackc/stackc_backend.{hpp,cpp}`，pipeline 注册为 StackC，逐波扩指令覆盖。**不承担 c2j 对抗职责**（对抗强度留给 RegC）。撤销理由与边界见 `stackc_revival_and_waves.md §0`。下表"归档 reference"等历史表述保留作决策溯源，不再生效。 |
| ADR-2 | 新 IR 第一块落地哪个？ | **先 SSA + 寄存器 IR**；runtime 第二块 | R1 = L2 SSA + L3 寄存器 IR 骨架；R3 才是 runtime |
| ADR-3 | 生成后端主路线？ | **寄存器式 C/C++ 为主**（非自研 VM，非 LLVM） | 主生成后端是 RegC；VMC4 自研 VM 从主线降级为 V2+ 可选增强 |
| ADR-4 | 本次产出形式？ | **先出完整重构设计文档 + ROADMAP 改写** | 本次 PR 全部是 `.md`；代码骨架是后续 PR |

**ADR-3 与 `native_vm_roadmap.md` 的冲突**：该文档原定 VMC4 自研 VM 与 CBackend（现 StackC）并行为
两条主路线。本次决策把 VMC4 降为 V2+ 可选路径，主线让位给寄存器式 C/C++。理由：
(a) 资源聚焦 —— mj2x 人手有限，自研 VM 解释器（含 resident opcode 自变异、opcode alias、
资源切片）是 ~7 周量级，寄存器 IR + RegC 在更低成本下命中大部分 c2j gap；
(b) 性能 —— RegC 生成普通 C/C++，可被 MSVC 优化，远好于 VM 解释执行；
(c) 避免 LLVM 依赖 —— `dependencies.md` 明确无 LLVM，寄存器 C/C++ 不引入新依赖。
VMC4 的设计（resident opcode 自变异、HMAC 资源封装）本身有价值，作为 RegC 之外"高价值
方法深度保护"的可选支线保留，见 `native_vm_roadmap.md` 的 Repositioning Notice。

---

## §1 战略基线

### 1.1 c2j 的攻击理论（两锚点）

c2j-native-deobfuscator 整套工具链的理论根基（其 `docs/static-reverse-approach.md §2`）是：

> 恢复每条 JVM 指令不需要识别完整机器码序列，只需要识别两个不变量：
> 1. 这条 asm 触发了 JNI 的哪个 vtable 槽（`env->functions` 偏移）；
> 2. 操作数指向 `cstack` / `clocal` / `cstrings` / `cclasses` / `cmethods` 的哪个索引。

只要被保护产物同时命中这两个锚点，c2j 的三条路径（动态 JVMTI hook、静态 Ghidra+tree-sitter
模式匹配、Unicorn 模拟）就能完整还原字节码。

### 1.2 现有 CBackend（现 StackC）同时命中两锚点

- **锚点 1 命中**：StackC 把每个字段读写、方法调用、对象创建都翻译为
  `(*env)->Call*Method` / `Get*Field` 等 JNI 调用，c2j 的 `jni_hook.cpp` 全量劫持
  ~80 个 vtable 槽即可恢复所有参数。
- **锚点 2 命中**：StackC 用 `union jvalue stack[N]` + `int sp` 模拟操作数栈，
  正则 `cstack(\d+)\.(\w)`（c2j `core.py:49`）精确匹配；类/方法/字段引用走静态数组表，
  命中 `cmethods[N]` / `cclasses[N]` 模式。

结论：现有 StackC 是 c2j 的**正中靶心**，系统性可还原。继续修补（INVOKEDYNAMIC、异常处理、
JNI 缓存）不改变命中锚点的事实，故"意义不大"。

### 1.3 唯一稳健的防御方向

c2j 自身代码暴露了它的边界（见 `竞品分析报告.md §2.4.4` + 竞品分析报告 PART 1 §6 的 Gap A-J）。
能真正打破 c2j 的防御只有"移除或变换锚点"这一类：

| 防御手段 | 打破的锚点 | 对应 c2j Gap |
|---------|-----------|-------------|
| 纯 native 算术（不经 JNI） | 锚点 1（无 JNI 可 hook） | Gap A |
| 寄存器化消除 `cstack[]` 数组 | 锚点 2（正则失配） | masxinlingvonta 验证 |
| 自定义符号解析 helper（非全局表） | 锚点 2（无 `cclasses[]` 可解析） | Gap C |
| 不发 "Cannot invoke" throw-reason 字符串 | 锚点 2 兜底（c2j 唯一 fallback） | Gap C |
| 运行时派生密钥的字符串/常量池 | 锚点 1+2（解密后内容不可静态恢复） | Gap B |
| 反 JVMTI（`env->functions` 篡改检测） | 锚点 1（动态路径前提） | Gap I |
| 反模拟（rdtsc/syscall 指纹） | Unicorn 兜底 | Gap J |

mj2x 的新架构围绕"系统性地移除这两个锚点"设计。

---

## §2 保留 / 退役清单

对现有 mj2x 源码与文档逐一裁决。**本次只宣告决策，代码移动是 R1 PR 的事**。

### 2.1 源码（`mj2x/src/`）

| 模块 | 决策 | 理由 |
|------|------|------|
| `io/input_loader`, `io/output_writer`, libjzip 依赖 | **保留** | 与生成后端无关的基础设施，JAR I/O 已验证 |
| `core/compiler_index`（含 `Utf8StringArena`、`JarPassthroughEntry`） | **保留** | 全局索引，新分层复用 |
| `core/pipeline` | **保留 + 扩展** | 加 L2/L3/L4 IR 阶段与多后端分发 |
| `frontend/mir_node`, `block`, `code_converter` | **保留为 L1 栈式 IR** | 正是"接近 ASM 的栈式 IR"，后端无关，是新分层的基础 |
| `frontend/type_utils`, `opcode_utils`, `method_stack` | **保留** | L1 IR 翻译辅助 |
| `backend/backend.hpp`, `backend_result.hpp` | **保留 + 扩展** | 接口抽象 OK；`BackendResult::Kind` 加 `REGISTER_C_SOURCE` |
| `backend/stackc/stackc_backend.{hpp,cpp}` | **已撤销：复活并更名为 StackC**（2026-07-05 复活；2026-07-08 更名） | ADR-1 撤销：复活为"易实现、广覆盖"通用后端，pipeline 注册为 StackC，逐波扩指令覆盖。不承担 c2j 对抗职责（对抗强度留给 RegC）。 |
| `preprocessor/mark_all_preprocessor` | **保留** | 标记逻辑复用 |
| `postprocessor/`（空接口） | **保留** | 后续注入 loader/stub 用 |

### 2.2 运行时（`mj2x/runtime/`）

| 文件 | 决策 | 理由 |
|------|------|------|
| `rt_str_obf.{h,cpp}` | **保留** | 已实现的 XOR 字符串混淆，并入新 runtime 的 stringpool |
| `main.cpp`（Windows DLL 注入 demo） | **删除** | 与编译器无关的实验残留（硬编码 `com/example/MainActivity`、`VK_END` 循环），AGENTS 边界外 |
| `rt_insn.{h,cpp}`（仅 `rt_nop` 宏） | **删除/重写** | 空壳无价值 |
| `rt_memory.{h,cpp}`（一个 `malloc` stub） | **删除/重写** | 空壳无价值 |
| `jni_wrapper.h`, `platform.h` | **保留** | JNI include 包装仍有用 |
| `xmake.lua`（`jni_lib` target） | **保留 + 重构** | R3 重写 runtime 内容后适配 |

### 2.3 测试（`mj2x/src/unittest/`）

| 测试 | 决策 |
|------|------|
| `frontend/code_converter_test.cpp`（13 例） | **保留**（L1 IR 基线） |
| `core/*`（compiler_index / io / input_loader / jar_io / e2e） | **保留** |
| `backend/stackc_backend_test.cpp` | **保留**：作为 StackC 子串匹配回归测试；历史 legacy 归档计划已随 ADR-1 撤销。 |

### 2.4 文档

| 文档 | 决策 |
|------|------|
| `mvp_plan.md` | **冻结为历史**：旧 CBackend MVP 已交付，顶部加完成标记，不再更新 |
| `architecture.md` | **改写为 v0.2**：反映新五层分层 |
| `native_vm_roadmap.md` | **降级改写**：VMC4 从主线→V2+ 可选 |
| `roadmap.md` | **改写**：B0-B3 → R0-R5 |
| `dev_plan.md` | **改写**：状态从"MVP 完成"→"架构重构期" |
| `test_strategy.md` | **改写**：测试分层对齐新 IR |
| `dependencies.md` | **保留**：无新依赖（仍无 LLVM） |
| `handover_context.md`, `task_mvp_completion.md` | **保留为历史** |

---

## §3 新分层架构

```
.class / .jar
   │
   │  InputLoader (复用)  →  CompilerIndex (复用, 含 arena)
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [L1] 栈式 MIR   (复用 frontend/code_converter + mir_node + block) │
│      接近 ASM，逐指令翻译，保留完整 JVM 语义                        │
│      ★ 这正是用户要求的"接近 ASM 库的栈式 IR"，现状即是            │
└──────────────────────────────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [L2] SSA 构造   (★ 新增 src/mj2x/mir/ssa/)                        │
│      基于 L1 Block CFG + reaching-def 分析                         │
│      dominance 计算 → φ 插入 → 变量重命名                          │
│      设计要点: 复用 libjbytecode analysis 已有的 Frame/SourceValue │
│      (masxinlingvonta 即用 ASM SourceInterpreter 拿 reaching-def, │
│       免手写 φ 插入)                                              │
└──────────────────────────────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [L3] 寄存器 IR  (★ 新增 src/mj2x/mir/register_ir/)                │
│      SSA → 寄存器化: φ 消解 + 线性扫描寄存器分配到虚拟寄存器 vN    │
│      操作数从 (stackSlot, sp) 变为 (RegId, Type)                  │
│      ★ 这一步消除 cstack[N]/sp 锚点 —— c2j 的 CSTACK_RE 失配      │
└──────────────────────────────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [L4] IR 变换 pass  (★ 新增 src/mj2x/mir/passes/)                  │
│      - 常量折叠/扩散            (命中 c2j Gap D)                  │
│      - 死代码消除               (寄存器化后 DCE 真正生效,          │
│                                    破 c2j"每指令写 cstack"假设)   │
│      - MBA 混合布尔算术          (纯 native 算术, 命中 Gap A)     │
│      - 控制流平坦化/不透明谓词   (生成期 seed 派生)                │
│      pass 框架仿 JavaShroud:                                     │
│        ModuleDefinition(schema: params/ordering/compatibility)   │
│        + Kahn 拓扑排序 planner (确定性, 稳定 tie-break)           │
└──────────────────────────────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [L5] 寄存器式 C/C++ 生成  (★ 新增 src/mj2x/backend/regc/)         │
│      RegIR → 普通 C/C++:                                          │
│        - 局部变量 = SSA 寄存器 (无 jvalue 联合, 无 cstack 数组)    │
│        - JNI 调用收敛: 符号解析走自定义 helper (非 cclasses[] 表) │
│        - 不发 "Cannot invoke X.Y(args)" throw-reason 字符串       │
│          (这是 c2j syms.py 失败时的唯一兜底, 见 throw_reason.py)  │
│      命中 c2j Gap: A(纯native算术) B(运行时密钥池)                │
│                    C(自定义符号helper) D(常量折叠)                 │
└──────────────────────────────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ [RT] Native 运行时  (★ 重写 runtime/)                             │
│      - crypto/:  集成审计过的轻量库 (PR#305 review: 不自研密码学) │
│                   候选 tiny-AES-c / Mbed TLS / BearSSL 精简子集   │
│                   覆盖 SHA256 / HMAC-SHA256 / AES-128; 无 OpenSSL │
│      - kernel/:  HKDF-SHA256 密钥派生 + 资源 HMAC 校验            │
│                   + 安全擦除用平台 API (PR#305 review):           │
│                     Windows SecureZeroMemory / POSIX explicit_bzero│
│                     / C11 memset_s; 兜底不透明函数指针调 memset   │
│                     (volatile 指针在 LTO/激进 DSE 下仍可能被消除) │
│      - antidebug/: Windows PEB (__readgsqword 0x60 → BeingDebugged│
│                     + NtGlobalFlag 0x70 + NtQueryInformationProcess│
│                     ProcessDebugPort)                             │
│      - antijvmti/: env->functions 指针篡改检测                    │
│                     (c2j vtable 劫持的直接反制)                   │
│                     + 多维 agent 检测 (PR#305 review):            │
│                       (a) RuntimeMXBean.getInputArguments 扫      │
│                           -javaagent (只抓启动期 agent)           │
│                       (b) Native 侧扫已加载模块: Windows          │
│                           EnumProcessModules / Linux /proc/self/maps│
│                           找 instrument.dll/.so (抓动态 attach)   │
│                       (c) Native 侧查 JVMTI capability 是否被启用 │
│      - stringpool/: 运行时派生密钥解密的字符串池 (命中 Gap B)     │
│      ★ JNI 收敛: 纯计算在 native 内完成, JNI 只在真正需要         │
│        JVM 互操作时调用 —— "减少 JNI、减少 hook trace 面"          │
└──────────────────────────────────────────────────────────────────┘
```

### 3.1 为什么是这个分层顺序

1. **L1 已有且正确**：栈式 MIR 是用户明确要的起点，且后端无关，无需重写。
2. **L2 在 L1 之上加 SSA**：SSA 是后续一切优化（DCE、常量折叠、寄存器分配）的前提。
   复用 libjbytecode 的 `SourceInterpreter`/`Frame<SourceValue>` 拿 reaching-def，免手写
   dominance+φ 插入（masxinlingvonta 已验证此路径可行）。
3. **L3 在 L2 之上寄存器化**：这一步是打破 c2j 锚点 2 的关键。寄存器 IR 的操作数是
   `(RegId, Type)`，不再有 `cstack[N]` 数组下标，CSTACK_RE 正则失配。
4. **L4 在 L3 之上做变换**：寄存器化之后 DCE 才能真正消除指令（栈式 IR 每条指令都"写 cstack"，
   是副作用，DCE 不能删 —— 这正是 c2j "DCE 永远不发生"假设的来源；寄存器化打破它）。
5. **L5 从 RegIR 生成 C/C++**：因为前面已经无栈数组，生成物天然无 cstack 模式。
6. **RT 与 L5 协作**：runtime 提供符号 helper、字符串池解密、反 JVMTI；L5 生成的代码调用
   runtime 而非直接 JNI 全局表。

### 3.2 与 masxinlingvonta 的关键差异

masxinlingvonta 走"栈式 IR → LLVM alloca → LLVM mem2reg → SSA"路线，把 SSA/优化/寄存器分配
**外包给 LLVM**。代价是引入 LLVM 依赖（违背 `dependencies.md`），且 LLVM IR 层有 KLEE 等成熟
符号执行工具。mj2x 选择**自研轻量 SSA + 寄存器分配**：可控、无新依赖、生成物是普通 C/C++
（可被 MSVC 优化但不暴露 LLVM IR 中间态）。

### 3.3 VMC4 自研 VM 的位置（ADR-3 降级后）

VMC4 不再是主路线，但保留为**可选支线**：对极高价值方法（license check、核心算法），RegC 之外
可额外走 VMC4 虚拟化（resident opcode 自变异、opcode alias、资源切片）获得更强抗分析能力。
该支线的设计沿用 `native_vm_roadmap.md`（已加 Repositioning Notice），但实现排在 R5+ 之后。

---

## §4 防御矩阵（c2j 攻击 → mj2x 防御 → 落地层 → mjdbg 验证）

这张表是 mjdbg 的攻击靶单。每条防御落地后，由 mjdbg 验证 c2j 对应攻击路径是否退化。

| c2j 攻击手段 | c2j 代码位置 | mj2x 防御 | 落地层 | mjdbg 验证标准 |
|------------|------------|----------|--------|--------------|
| JNI vtable 劫持（`env->functions` 交换） | `jni_hook.cpp:1047 install()` | 反 JVMTI：运行时检测 `env->functions` 是否被替换为非原始表 + 多维 agent 检测（启动期扫 argv + 动态 attach 扫已加载模块 + JVMTI capability 查询，见 §3 RT） | RT antijvmti | 注入 c2j agent 后程序 fail-closed 或 trace 缺失 |
| `cstack[N].i` / `cstrings[N]` 正则匹配 | `core.py:49 CSTACK_RE`、`driver.py:869 _CACHE_SLOT_RE` | 寄存器化消除栈数组 | L3 | Ghidra 反编译 RegC 产物无 `cstack` 模式 |
| Ghidra 伪 C 模式匹配（match_assignment/if/return/void_call） | `core.py:312/519/214/443` | MBA + 常量折叠 + 自定义符号 helper 改变 C 结构 | L4/L5 | c2j 静态 lifter 产出退化（`?.?` stub 或语义损失） |
| 符号追踪（`syms.py` SymClass/MethodId/FieldId） | `lifter/syms.py` | 不发 "Cannot invoke" throw-reason 字符串 + 自定义 helper | L5 | c2j `throw_reason.py` 无匹配，call site 留 `?.?` |
| `cclasses/cmethods/cfields` 表索引解析 | `core.py:49 TABLE_RE`、`driver.py:113 cache_table` | 类/方法/字段 ID 走 runtime helper，无全局静态表 | L5 + RT | c2j `cache_table` 无可绑定地址 |
| Unicorn 符号执行（纯函数 oracle） | `j2c_emu.py` | 运行时密钥绑定 + 反模拟指纹（rdtsc/syscall） | RT | unicorn `_unmapped` 硬停或结果错误 |
| `NativeMethodBind` 事件捕获注册 | `agent.cpp:119 on_native_method_bind` | JNI_OnLoad 直接绑定，不走 `RegisterNatives` | RT | c2j `bind` 事件缺失 |
| 纯 native 算术（不经 JNI） | c2j 动态路径完全不可见 | MBA + 内联比较 + 常量折叠 | L4 | 仅 oracle 可得 I/O，无字节码还原 |
| 异常处理恢复 | `agent.cpp:168 exc/excCatch` | native 侧 catch+clear（c2j 已知 Gap F 未实现） | L5 + RT | `tryCatchBlocks` 留空 |
| INVOKEDYNAMIC 链折叠 | `Main.kt:310` 仅 tag 不 collapse | 重度 invokedynamic 使用 | L1 + L4 | c2j 输出 tag-but-not-collapsed 退化 |
| `<clinit>` 字符串表解密 | c2j `roadmap.md` 承认未实现 | 运行时派生密钥的字符串池 | RT stringpool | c2j 留 `Foo.a(0,17)` 不透明索引 |

### 4.1 防御优先级

按"打破锚点的力度 × 实现成本"排序，落地顺序对应 ROADMAP 的 R1-R5：

1. **R1（L2+L3）**：寄存器化 —— 打破锚点 2，成本中等，收益最大。
2. **R2（L5 RegC）**：生成无 cstack 模式的 C —— 让 R1 的成果可见可验证。
3. **R3（RT）**：反 JVMTI + 运行时密钥池 —— 打破锚点 1 + Gap B，成本高但收益关键。
4. **R4（L4 passes）**：MBA + 常量折叠 —— 强化静态路径抗性。
5. **R5+**：mjdbg 闭环验证 + 可选 VMC4。

---

## §5 字节码覆盖矩阵

L1 栈式 MIR 对 JVM 指令的覆盖现状（来自 `mir_node.hpp` + `code_converter.cpp` 实读）：

### 5.1 L1 已完整翻译的 MIRKind

`NOP, LABEL, CONSTANT, LOCAL_LOAD, LOCAL_STORE, STACK_DUP, STACK_DUP_X1, STACK_DUP_X2,
STACK_DUP2, STACK_SWAP, BINARY_OP, UNARY_OP, CAST, IINC, BRANCH, BRANCH_ZERO,
UNCOND_BRANCH, SWITCH, RETURN, RETURN_VALUE, NEW_OBJECT, NEW_ARRAY, ARRAY_LENGTH,
ARRAY_LOAD, ARRAY_STORE, FIELD_GET, FIELD_PUT, STATIC_GET, STATIC_PUT, INVOKE,
CHECK_CAST, INSTANCE_OF, MONITOR_ENTER, MONITOR_EXIT, THROW, FRAME`

### 5.2 L1 的占位/未完成项（fail-closed 边界）

| MIRKind | 现状 | 新分层处理 |
|---------|------|-----------|
| `INVOKE_DYNAMIC` | `code_converter.cpp:310` 翻译为 `MIRNop` 占位 | L1 加 preprocessor 提取到合成方法（参考 masxinlingvonta `InstructionExtractor`） |
| `MULTI_NEW_ARRAY` | `code_converter.cpp:425` 翻译为 `MIRNop` 占位 | L1 加 preprocessor 展开 |
| `LINE` | enum 有值但无 struct，未翻译 | L1 元数据，低优先级 |

> 注：`mir_node.hpp` 的 `MIRKind` enum 有 39 个值（dev_plan.md 旧称"34 种"是历史口径）。
> `MIRInvokeDynamic`/`MIRMultiNewArray`/`MIRLine` 的 struct **实际未定义**，enum 值存在但
> converter 落到 nop。这是 R1 要先补齐的 L1 缺口。

### 5.3 fail-closed 边界（沿用 `doc/prompt/mj2x.md`）

不支持的字节码场景必须 fail-closed 并给出明确原因，不得静默生成错误代码：

- `ACC_SYNCHRONIZED` 方法（需 JVM 级 monitor 管理）—— R1 起仍拒绝
- `synchronized` 块内抛异常（锁泄漏）—— R2 起用 **C++ RAII guard** 修复（PR#305 review）：L5 生成局部 Guard 对象，构造调 `MonitorEnter`、析构调 `MonitorExit`，靠栈展开保证任意退出路径（正常/return/异常）都释放锁，比手写 try/close 健壮
- `ATHROW` 后 JNI 控制流 —— R2 起翻译为 `Throw + return`

---

## §6 分阶段里程碑（与 roadmap.md 对齐）

本节是摘要，权威定义在 `roadmap.md` 的 R0-R5。

| Wave | 内容 | 验收 | 产出 |
|------|------|------|------|
| **R0**（本次） | 架构重构设计落地 | 6 份文档 review 通过 | 本文档 + ARCHITECTURE v0.2 + NATIVE_VM_ROADMAP 降级 + ROADMAP + dev_plan + TEST_STRATEGY |
| **R1** | L2 SSA + L3 寄存器 IR 骨架 | 一个简单方法 MIR→SSA→RegIR，dump 无 stack slot | `src/mj2x/mir/ssa/`, `src/mj2x/mir/register_ir/` + 单元测试 |
| **R2** | L5 寄存器式 C 生成 + E2E | Java 方法 → mj2x → .c → cl → 运行 → 结果正确；生成 C 无 `cstack` 模式 | `src/mj2x/backend/regc/` + E2E 测试 |
| **R3** | RT 运行时最小可用 | 受保护样本在 c2j agent 下 fail-closed 或 trace 显著退化 | `runtime/{crypto,kernel,antidebug,antijvmti,stringpool}/` |
| **R4** | L4 变换 pass 系统 | 变换后 RegC 经 Ghidra 反编译，c2j 静态 lifter 产出退化 | `src/mj2x/mir/passes/`（schema + Kahn planner + 4 个 pass） |
| **R5+** | mjdbg 闭环 + 可选 VMC4 | mjdbg 攻击报告显示恢复难度上升 | 受保护样本 + mjdbg 报告；VMC4 视情况启动 |

每个 R 波次的"完成"以 mjdbg（红队）的攻击反馈为准，而非实现复杂度。这是 `doc/prompt/mj2x.md`
attack/defense loop 的硬性要求。

---

## §7 与既有文档的关系

| 文档 | 本次后的状态 | 关系说明 |
|------|------------|---------|
| **本文档（architecture_redesign.md）** | **权威** | 重构期最高优先级，覆盖冲突 |
| `architecture.md` | v0.2，摘要性 | 引用本文档 §3 的分层；v0.1 的 CBackend 主线定位曾作废，现以 StackC 名义复活 |
| `native_vm_roadmap.md` | 降级，加 Repositioning Notice | VMC4 从主线→V2+ 可选；设计内容保留 |
| `roadmap.md` | 改写 | R0-R5 替换 B0-B3 |
| `mvp_plan.md` | 冻结历史 | 旧 CBackend MVP 已交付，不再更新 |
| `dev_plan.md` | 改写 | 状态切换到架构重构期 |
| `test_strategy.md` | 改写 | 测试分层对齐 L1-L5 + RT |
| `dependencies.md` | 不变 | 仍无 LLVM，无新依赖 |

冲突解决规则：本文档与其它文档冲突时，以本文档为准，直至相关文档被同步改写。

---

## §8 待决问题（留给后续 PR）

以下不在本次设计文档范围，留给 R1 代码 PR 时定：

1. **RegIR 节点具体字段**：`RegId` 宽度、`Type` 枚举是否复用 `PrimitiveKind`、φ 节点表示 —— 留给 R1 header 设计。
2. **SSA 实现细节**：是自写 dominance 还是直接用 libjbytecode `SourceInterpreter` 的 reaching-def 近似 SSA —— R1 spike 决定。
3. **pass 框架的 schema 格式**：TOML 还是 C++ 结构体 —— R4 决定。
4. **runtime 与 RegC 的符号 helper ABI**：是 C 函数符号还是 token 表 —— R3 与 L5 协同设计。
5. ~~**`backend/reference/` 的目录落点**：是 `src/mj2x/backend/reference/` 还是单独 `legacy/` —— R1 代码移动时定。~~ **已撤销：ADR-1 已反转，CBackend 复活并更名为 StackC，现位于 `backend/stackc/`，不设 reference 目录。**

这些都是实现细节，不影响本次架构决策的正确性。

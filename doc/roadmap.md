# mj2x 路线图

> mj2x 蓝队产品线路线图。
>
> **v3（2026-07-07 JDK 内建 intrinsic + 整类下沉战略）**：
> Intrinsic 不是独立路线，而是翻译管线进化到一定阶段后的自然能力升级。
> 翻译管线成熟后（RegC → RT → Pass），将 JDK 常用方法 native 等价实现嵌入管线，
> 减少 JNI 调用、提升性能、提高保护水平。整类下沉是 intrinsic 覆盖充分后的进一步延伸——
> 算法密集、JVM 交互少的类整体丢进 native，内部零 JNI。
> 不做全 AOT（反射等太难），从"垃圾版"基元类型开始，逐轮扩大。
> 背景：竞品 jvmtp/jumanji144 在纯保护路线（VM 虚拟化、混淆 jumps/merge、antidebug）
> 上推进极快（1.2.2→1.3.0 两周 3 个 release），mj2x 选择差异化战略——用翻译管线 +
> intrinsic + 整类下沉来拉开差距。
>
> **v2.1（2026-07-05 StackC 复活）**：撤销 ADR-1，StackC 作为"易实现、广覆盖"通用后端
> 与 RegC 并行。复活决策与逐波覆盖路线见 `stackc_revival_and_waves.md`。
> **v2（2026-06-24 架构重构后）**：B0-B3 波次被 R0-R5 替换。
> 重构依据见 `architecture_redesign.md`。v1 的 CBackend 主线曾作废（归档 reference），现已作为 StackC 复活。

---

## 1. 使命

mj2x 的核心能力是 Java 字节码 → native 编译管线。管线的进化路线：

**阶段 1 — 反 c2j 保护（R0-R4）**：
- 字节码 → 多层 IR（栈式 MIR → SSA → 寄存器 IR → 变换）
- 寄存器式 C/C++ 代码生成（RegC），消除 cstack/clocal 锚点
- native 运行时（crypto / antidebug / antijvmti / stringpool）
- 受保护样本生成

**阶段 2 — Intrinsic 内联（R2 起逐步引入，R4+ 扩大覆盖）**：
- 翻译管线成熟后，将 JDK 常用方法 native 等价实现嵌入管线
- 减少 JNI 调用 = 减少 c2j 可 hook 表面 + 提升性能
- 方法级替换 → 包装类 → String/Math → 集合，逐轮扩大

**阶段 3 — 整类下沉（R4+，intrinsic 覆盖充分后）**：
- 算法密集、JVM 交互少的类整体丢进 native，内部零 JNI
- 仅边界保留 JNI 入口，翻译后无字节码可还原
- 不做全 AOT：反射、动态代理、类加载等边界场景不处理

可选：VMC4 native VM 虚拟化（V2+/R5+ 视反馈决定）

mj2x 是 morpher attack/defense loop 的蓝队半环。其输出最终必须由 mjdbg 攻击。
**一个波次只有当 mjdbg（红队）确认攻击变难时才算"完成"，而不是当实现看起来完整。**

> **技术演进谱系**: 本文按 R 波次组织交付节奏（"做哪些"）；跨波次的技术能力代际演进
> （直译栈字节码 → 符号缓存 → 基元/字符串 intrinsic → 容器映射 + Any → 调用语义 → invokedynamic →
> JVMTI CallSite → 内存池）见 [`doc/translation_evolution.md`](translation_evolution.md)（"每类技术如何一代代演进"）。
> 波次与代际互补，某代能力可择机插入最近的波次。

### 1.1 竞争定位

| 维度 | jvmtp/jumanji144 | mj2x |
|------|-----------------|------|
| 核心路线 | VM 虚拟化 + 混淆 + antidebug | 翻译管线 → intrinsic → 整类下沉（渐进进化） |
| JDK 类型 | 基础 String/Math intrinsic（1.3.0 新增） | R2 初版基元/包装类 → R3 String/Math → R4 集合/工具类 |
| 用户使用模式 | 方法级保护标记 | 方法级保护 → 覆盖扩大 → 类级整块下沉 |
| 保护哲学 | 运行中保护（VM/antidebug/safe-env） | 编译时消除 JNI 表面 + 运行时反调试 |
| 迭代策略 | 快速功能叠加（两周 3 版） | **管线先行，intrinsic 逐步注入**：管线不成熟不做 intrinsic；
  intrinsic 从"垃圾版"起步，逐轮扩大 |
| 不做 | — | 全 AOT、反射 native 化、动态代理、类加载器 |

---

## 2. 战略基础（重构依据）

详见 `architecture_redesign.md §1`。摘要：

- c2j-native-deobfuscator 的攻击理论依赖两个锚点：JNI vtable offset + jvalue 索引模式
  （`cstack[N].i` / `cstrings[N]` / `cclasses[N]`）。
- 旧 CBackend（现 StackC）同时命中两锚点 → 系统性可还原 → 继续修补意义不大（ADR-1 归档）。
- 新主路线：通过 L2 SSA + L3 寄存器化消除锚点 2，L5 RegC + RT 消除锚点 1。
- VMC4 自研 VM 从主线降级为可选支线（ADR-3）。

---

## 3. 后端路线（重构后）

### RegC（主路线）

目标：

- 寄存器式 C/C++ 代码生成，消除 cstack/clocal 锚点
- 命中 c2j Gap A（纯 native 算术）/ C（自定义符号 helper）/ D（常量折叠）

阶段（对应 R 波次）：

- R1：L2 SSA + L3 寄存器 IR 骨架
- R2：L5 RegC 生成 + E2E（生成 C 无 cstack 模式）
  - RegC 中嵌入方法级 intrinsic：识别 Math/Random/基元包装类调用 → 直接内联 native 实现
- R3：RT 运行时支撑 intrinsic 实现（内存管理、字符串基础操作）
- R4：L4 变换 pass + intrinsic 覆盖扩展 + 整类下沉

### VMC4（可选支线）

目标：

- 对极高价值方法提供 resident opcode 自变异 + opcode alias + 资源切片的额外抗分析层

时机：

- 排在 R5+ 之后，RegC + RT 闭环后据 mjdbg 攻击反馈决定是否启动
- 设计见 `native_vm_roadmap.md`（已降级）

### StackC（复活通用后端，2026-07-05 起）

- **撤销 ADR-1**：从 `backend/reference/` 复活并更名到 `backend/stackc/`，pipeline 重新注册，逐波扩指令覆盖
- 定位："易实现、广覆盖"通用后端，吃 L1 栈式 MIR，直接产 JNI C 源码
- **不承担 c2j 对抗职责**：生成的代码可被 c2j 还原（接受）；对抗强度留给未来的 RegC
- SSA 线（R1a/R1b 已合并）保留代码，但**不接入** StackC pipeline（StackC 直接吃 L1 MIR）
- 逐波覆盖路线（Wave 0-7）见 `stackc_revival_and_waves.md`
- 历史：v0.1 已交付 → ADR-1 归档 → 复活

---

## 4. 波次（R0-R5，替换 v1 的 B0-B3）

### Wave R0：架构重构设计落地（已完成）

目标：把竞品分析结论转化为可执行的新架构设计。

- `architecture_redesign.md`（核心，保留/退役清单 + 五层分层 + 防御矩阵）
- `architecture.md` v0.2（摘要，修正 doc drift）
- `native_vm_roadmap.md` 降级（VMC4 主线→可选支线）
- `roadmap.md` / `dev_plan.md` / `test_strategy.md` 改写
- 无代码改动

完成标志：6 份文档 review 通过，分支 `spike/mj2x/arch-redesign`。

### Wave R1：L2 SSA + L3 寄存器 IR 骨架（首块代码落地）

目标：在 L1 栈式 MIR 之上建立 SSA 与寄存器化，消除 stack slot 锚点。

- `src/mj2x/mir/ssa/`（reaching-def + dominance + φ 插入 + 重命名）
- `src/mj2x/mir/register_ir/`（RegIR 节点 + φ 消解 + 虚拟寄存器分配）
- 复用 libjbytecode analysis 的 Frame/SourceValue（参考 masxinlingvonta 路径）
- 顺带补齐 L1 缺口：INVOKEDYNAMIC / MULTIANEWARRAY preprocessor 提取
- 单元测试：SSA 构造正确性、寄存器化消除栈槽

完成标志：一个简单方法 MIR→SSA→RegIR，dump 显示操作数为 `(RegId, Type)` 无 stack slot。
此时 c2j 的 `CSTACK_RE` 已对 RegIR 失配（可手工验证）。

### Wave R2：L5 RegC 寄存器式 C 生成 + 初版 Intrinsic 内联

目标：从寄存器 IR 生成可编译、无 cstack 模式的 C/C++；
同时在 RegC 码生成阶段嵌入方法级 intrinsic 内联，减少 JNI 调用。

**RegC 生成：**
- `src/mj2x/backend/regc/`
- MIR subset（常量/局部/算术/控制流/返回）→ RegC → 可被 MSVC 编译
- 符号解析走自定义 helper（非全局 `cclasses[]` 表）
- 不发 "Cannot invoke" throw-reason 字符串
- E2E：Java 方法 → mj2x → .c → cl → 运行 → 结果对比

**初版 Intrinsic（方法级替换，垃圾版）**：
- 目标：当 RegC 遇到 JDK 方法调用时，不发 JNI 调用，直接内联 native 等价实现
- 覆盖：基元类型包装类的 valueOf/intValue/parseInt 等基础方法
- 实现：`runtime/intrinsic/wrapper/` — 各包装类 native 结构 + 基本操作
  - `Integer` → native struct + `mj2x_int_valueOf`/`mj2x_int_intValue`/`mj2x_int_parseInt`
  - `Long`/`Short`/`Byte`/`Float`/`Double` → 类推
- 基础类型映射：`int/long/short/byte/float/double/boolean/char` → C 原生类型
- 依赖 libca 基础类型（`i32`/`i64`/`f64` 等）
- 不支持：字符串、数组、异常、方法调用链、GC

完成标志：
- RegC：生成 C 经 Ghidra 反编译无 `cstack[N]` 模式；行为与原 Java 一致
- Intrinsic：`Integer.valueOf(42).toString()` 在 RegC 下发纯 native 计算，无 JNI 调用

### Wave R3：RT 运行时 + Intrinsic 运行时整合

目标：建立 native 运行时，打破 c2j 锚点 1（JNI hook）+ Gap B（运行时密钥池）；
同时运行时承载 intrinsic 实现（String 操作、内存管理等），
intrinsic 实现在此波次从松散代码整合为受运行时管理的模块。

**RT 运行时：**
- `runtime/crypto/`（SHA256 / HMAC-SHA256 / AES-128，纯 C 无 OpenSSL）
- `runtime/kernel/`（HKDF-SHA256 派生 + 资源 HMAC 校验 + volatile wipe）
- `runtime/antidebug/`（Windows PEB：`__readgsqword(0x60)` + NtGlobalFlag + NtQueryInformationProcess）
- `runtime/antijvmti/`（`env->functions` 篡改检测 + RuntimeMXBean 扫 `-javaagent`）
- `runtime/stringpool/`（运行时派生密钥解密的字符串池）
- 与 RegC 集成：符号 helper、字符串池解密
- 已删除 `runtime/main.cpp`（DLL demo）、`rt_insn`/`rt_memory`（空壳）；R3 core 改为
  `mj2x-runtime-core` + `mj2x_runtime_test`

**Intrinsic 运行时整合：**
- R2 的包装类 intrinsic 迁移到 `runtime/intrinsic/`，受运行时管理
- **Math/Random intrinsic**：`runtime/intrinsic/math/`
  - `java.lang.Math` → 直接调 C `math.h`：`abs`, `min/max`, `sin/cos/tan`, `sqrt`, `pow`
  - `java.util.Random` → 原生 XorShift128+ PRNG
- 初版 GC：引用计数（包装类对象），包装 `malloc`/`free`
- 堆分配：`mj2x_alloc` / `mj2x_free`，基于 libca `Bytes`/`BytesMut` 管理缓冲区

**运行时与 intrinsic 的依赖关系**：
- runtime 提供内存管理、平台抽象（`platform.hpp`）、crypto、错误处理（`Result<T,E>`）
- intrinsic 实现调用 runtime 的基础设施，不直接依赖 OS API

完成标志：受保护样本在 c2j agent 下 fail-closed，或 trace 显著退化（与 mjdbg 协作验证）；
包装类 + Math 计算的类经 mj2x → .c → cl → 运行，结果与 Java 一致。

### Wave R4：L4 变换 pass + Intrinsic 覆盖扩展 + 整类下沉初版

目标：强化静态路径抗性（c2j Gap A/D）；扩大 intrinsic 覆盖；
intrinsic 覆盖充分后支持整类下沉——算法密集类整体 native，内部零 JNI。

**L4 变换 pass：**
- `src/mj2x/mir/passes/`（pass 框架：schema + Kahn 拓扑排序 planner，确定性）
- 实现 4 个 pass：
  - 常量折叠/扩散（Gap D）
  - 死代码消除（寄存器化后真正生效）
  - MBA 混合布尔算术（Gap A 纯 native 算术）
  - 控制流平坦化 + 不透明谓词（seed 派生）

**String intrinsic（Math/Random 之后，基于 libca UTF-16 字符串）：**
- 等待 libca 提供 Java 兼容的 UTF-16 字符串类型（自定 `Char16` 存 u16，不用 `char16_t`，见 luiox/libca#138）
- mj2x runtime 在其上封装 `mj2x_string`，对齐 `java.lang.String` 语义
  - 支持：`length`, `charAt`, `equals`, `concat`, `substring`（初版）
  - `indexOf`, `replace`, `split`, `trim`, `compareTo`, `String.valueOf(...)`
  - StringBuilder/StringBuffer：可变字符串缓冲，链式 append
- `String.format` 初版（`%d`, `%s`, `%f`）

**其他 intrinsic 扩展：**
- `java.util.Arrays`：`sort`（基元快排）、`binarySearch`、`fill`、`copyOf`
- `java.lang.System`：`arraycopy`, `currentTimeMillis`, `nanoTime`
- `java.util.Objects`：`equals`, `hashCode`, `requireNonNull`
- 异常处理 native 化：`try/catch/finally` + `throw` 在 native 层模拟
  - 简单类型：`NullPointerException`, `IllegalArgumentException`, `ArithmeticException`
  - 实现：`setjmp`/`longjmp` 或 error code 跳转表

**整类下沉初版（intrinsic 覆盖充分后的延伸）**：
- 用户接口：`mj2x --native-class com/example/MyLogic`
- 前置分析器验证类中所有方法无 JNI 逃逸（无反射调用、无未覆盖 JDK 方法）
- 若通过验证，整类编译为纯 C，内部方法调用全 native 闭环，边界仅暴露 JNI 入口
- 若有逃逸，降级为 hybrid：逃逸方法留 JNI，非逃逸方法 native
- 不做全 AOT：反射、动态代理、类加载、Instrumentation 等不处理

完成标志：变换后 RegC 经 Ghidra 反编译，c2j 静态 lifter 产出退化；
HashMap<Integer, String> 操作全 intrinsic 化；整类下沉 E2E 测试通过（纯算法类，零 JNI）。

### Wave R5+：mjdbg 闭环 + 持续 Intrinsic 扩展 + 可选 VMC4

目标：进入攻防闭环；持续扩大 intrinsic 覆盖；视反馈决定 VMC4。

- 产受保护样本给 mjdbg 攻击
- 据 mjdbg 报告加硬化（每个反馈至少一个硬化改动 + 再生样本）
- Intrinsic 按需扩展（次序取决于实际项目需求）：
  - `ArrayList` / `HashMap` / `HashSet` 等集合类
  - `java.util.regex.Pattern` / `Matcher` 正则
  - `java.time.*` 时间类
  - `synchronized` → 原生 mutex
  - 用户自定义 intrinsic（配置/注解声明）
- 可选：VMC4 自研 VM 作为高价值方法深度保护支线（设计见 `native_vm_roadmap.md`）
- long-term：根据实际使用经验重写 intrinsic 实现（从垃圾版升级为成熟版）

完成标志：mjdbg 攻击报告显示恢复难度相对前一版样本上升；intrinsic 覆盖满足实际项目需求。

---

## 5. 共享契约

| 契约 | 其他 owner | 规则 |
|---|---|---|
| 受保护样本格式 | mjdbg | 样本必须文档化 expected trace surface |
| trace 期望 | mjdbg | 隐藏或随机化信号前先协调 |
| libjbytecode 使用 | mbase | 直接用 OK；共享 wrapper 需协调 |
| runtime 构建选项 | build owner | 不静默加重默认依赖（仍无 LLVM） |
| c2j 攻击靶单 | mjdbg | `architecture_redesign.md §4` 防御矩阵每条需 mjdbg 验证 |

---

### 5.1 对 libca 的依赖契约

mj2x runtime（包括 intrinsic 实现）构建在 libca 之上，不自建基础轮子。

| mj2x 需要 | libca 提供 | 说明 |
|-----------|-----------|------|
| 基础类型 | `i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/usize/byte` | Java int→i32, long→i64 等直接映射 |
| 错误处理 | `Result<T,E>` / `Ok()` / `Err()` / `TRY` | runtime 内部错误传播 |
| 字符串 | `Utf16String`（需新增，见 luiox/libca#138）| Java String 的 UTF-16 char[] 对应，直接匹配 JNI 边界编码 |
| 字节缓冲 | `Bytes` / `BytesMut` | 数组数据载体、序列化缓冲区 |
| 字符串池 | `Utf8StringArena` / `Utf8StringPool` | StringTable、字符串去重 |
| 加密 | `SHA256` / `HMAC` / `ChaCha20` / `Base64` / `Random` | runtime crypto 直接复用 |
| 时间 | `DateTime` — `Date`/`Time`/`DateTime` | `java.time.*` 的基底 |
| 平台检测 | `CA_PLATFORM_WINDOWS/LINUX` 宏 | antidebug 平台分支 |
| 无 RTTI 转型 | `cast<T>()` / `dyn_cast<T>()` / `Any` | IR 节点多态（与 mbase 合约一致） |

**当前 libca 未覆盖、mj2x runtime 需自建或要求 libca 补充**：
- UTF-16 字符串：已提 issue luiox/libca#138，libca 实现后 runtime 在其上封装 Java String 语义
- 通用 `Vec<T>`（动态数组）：libca 无，目前用 `std::vector`。若 libca 后续补充 `Vec<T>`，runtime 可迁移
- `HashMap<K,V>` / `HashSet<T>`：libca 无，runtime 自建或 libca 补充后迁移
- GC / 引用计数：libca 无，runtime 自建
- 数学函数：依赖 C `math.h`，无需 libca
- 线程/互斥锁：libca.core 有遗留实现但当前 `libca/` 未模块化；runtime 直接调 OS API

---

## 6. 验证

基线（重构后，命令不变）：

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
```

对于 native 运行时变更（R3+），记录：

- OS / 编译器 / 工具链
- JDK（如果涉及 JNI）
- 行为是否与原 Java 对比
- 是否使用 c2j agent 验证抗 JVMTI（与 mjdbg 协作）

---

## 7. 文档职责

mj2x AI 维护：

- `prompt/mj2x.md`
- 本文 `doc/roadmap.md`
- `doc/translation_evolution.md`（跨波次技术能力代际演进谱系）
- `doc/architecture_redesign.md`（★ 重构期最高优先级）
- `doc/architecture.md`
- `dev_plan.md`
- `doc/native_vm_roadmap.md`（已降级）
- `doc_ai/test_strategy.md`
- `doc/mvp_release_scope.md`（当前 MVP 冻结范围）
- `doc/mvp_plan.md`（冻结历史）

维护 `mj2x/doc_ai/capability_map.md` 作为 mj2x 详细能力地图。

当受保护样本格式、VM 字节码格式、runtime ABI 或面向 mjdbg 的 trace 期望变更时，
更新根 `doc_ai/contracts.md`。

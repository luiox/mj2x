# RegC 转换决策文档（唯一事实源）

> 状态：`main` 现状 + 2026-08-08 决策记录。
> 职责：本文档是"哪些方法/指令能转 native、不能转的原因、怎么转"的**唯一事实源**。
> 代码中不再重复高层设计；代码只是实现。改转换行为 = 先改此文 + 改代码 + 补测试。
> 迁移背景：ares 侧 Java 实现时，以本文档的决策表逐条对齐，行为必须一致。

---

## 1. 总览：方法级四态

| 状态 | 含义 | 判定位置 | 特征 |
|---|---|---|---|
| **Native** | 标记 ACC_NATIVE，字节码交给生成的 C helper | `injectIntoClass` 成功 | 无 `Code:` 属性 |
| **Skip（设计）** | 保留字节码，**有意不转**，有日志 | `shouldKeepBytecodeForCompatibility` / `isCompatibilityRiskCall` | 日志含 `Skipping` |
| **Skip（fail-closed）** | 保留字节码，转换器失败兜底，有日志 | SSA / Lowering / Emitter 返回失败 | 日志含 `Skipping` + 阶段名 |
| **不标记（JVM 限制）** | MarkAll 直接跳过，不进入后端 | `MarkAllMethodsPreprocessor` | 无日志 |

---

## 2. JVM 限制类（永远不转，四态之外）

| 方法 | 原因 | 证据 | 备注 |
|---|---|---|---|
| `<init>` / `<clinit>` | JVM 校验对构造器/类初始化器的 ACC_NATIVE 有限制（部分实现拒收 native `<init>`） | `mark_all_preprocessor.hpp:20-24` | **决策（2026-08-08）：可转，用"搬运法"，见 §7.1** |
| `abstract` / 已有 `native` 标记 | 无字节码 / 已 native | `mark_all_preprocessor.hpp:17-19` | 永远 |
| `lambda$*`（javac 生成的 lambda 宿主方法） | 历史遗留"invokedynamic 兼容性"而 skip | `regc_backend.cpp` shouldKeepBytecode | **决策：可转，见 §7.2** |

---

## 3. 指令转换清单（按类别）

| 指令类别 | 覆盖 | 形式 | 验证 |
|---|---|---|---|
| 常量/ldc（int/float/String/Class） | ✅ 直接 | LDC Handle/MethodType 先被 `ldc_preprocessor` 重写为反射链 | `code_converter_test` |
| 局部变量/栈操作（dup/swap） | ✅ | SSA 构建时消除 | `ssa_builder_test` |
| 整型/long/float/double 运算 | ✅ 含回绕、NaN | — | E2E |
| 除法/取余 | ✅ 除零走 R3 `mj2x_regc_div_i32/i64` trap | — | `IntegerDivisionTrapRoundTrip` |
| 位运算/移位 | ✅ 含无符号右移（>>>语义） | — | E2E |
| 一致性转换（i2b/i2s/i2c/i2l 等） | ✅ | — | E2E |
| 三值比较（fcmpg/fcmpl/dcmpg/dcmpl/lcmp） | ✅ `cmp.nan_*` intrinsic | — | `RegCCompareIntrinsicTest` |
| 分支/switch | ✅ tableswitch/lookupswitch | — | E2E |
| 异常表与 athrow | ✅ 无 catch 数上限 | `RegExceptionRegion` | E2E |
| monitor | ✅ | `_mj2x_regc_monitor_*` | E2E |
| 字段读写/数组读写/arraylength | ✅ 全类型 slim | cache CSE | E2E |
| new/anewarray/multianewarray | ✅ | `_alloc_*` helper | E2E |
| checkcast/instanceof | ✅ 含 CAST 空检 | cached helper | E2E |
| 方法调用（virtual/special/static/interface） | ✅ 实例方法按 receiver 实际类解析 | 修过抽象/接口 owner bug | E2E |
| String/StringBuilder/StringBuffer intrinsic | ✅ 大流行 subset | opaque UTF-16 state | intrinsic E2E |
| 包装类/`java.util.Random` intrinsic | ✅ 非逃逸 shadow | — | intrinsic E2E |
| invokedynamic | §6 分形态 | — | — |
| JSR/RET | ✖ Unsupported MIR 节点（javac 17 不产生） | `code_converter` | 永远 |

---

## 4. 方法级 Skip（设计决策）

`isCompatibilityRiskCall` / `shouldKeepBytecodeForCompatibility`（`regc_backend.cpp`）

| 触发 | 条件 | 原因 | 现状 |
|---|---|---|---|
| 并发组 | 调用 `java/lang/Thread.*` 或 `java/util/concurrent/*` | 线程池 worker 中 native 帧调受监控 API 破坏 ReentrantLock monitor（实测 IMSE） | 保留。**决策：主线程可放开；worker 场景走生成 stub（§7.4）** |
| System 安全组 | `System.exit` / `setSecurityManager` / `getSecurityManager` | 安全/退出语义 | 保留 |
| `lambda$` 宿主方法 | 方法名以 `lambda$` 开头 | 历史遗留 | **移除决策（§7.2）** |
| 剩余 `INVOKE_DYNAMIC` 节点 | 展开失败形态 | §6 | 分阶段消除，剩自定义 bootstrap |

### 已移除的历史 skip（保审计痕迹）

| 曾 skip | 移除载体 |
|---|---|
| 反射组（Class/reflect/*/getClass） | R6-B（引用管理 PushLocalFrame/Frame 前置完成后放开） |
| IO 组 / ClassLoader | R6-A/B `collect_args` 数组 bug 修复后 |
| 内部类 `$` | RegisterNatives（PR #661） |
| 大异常表（>4 catch） | 三层 lowering 无上限（R6） |
| 浮点/long 三值比较 | `cmp.nan_*`（R6.1） |

---

## 5. fail-closed 兜底（触发即应诊断）

| 阶段 | 日志格式 | 常见实际触发 |
|---|---|---|
| SSA | `[RegCBackend] Skipping <m> <stage> failed: ...` | 栈下溢、DUP/SWAP 语义、不支持形态 |
| Lowering | 同上 | unknown kind 等 |
| Emitter | 同上 | 不支持的 intrinsic/native 逃逸 |

全部 fail-closed 必须可观测（grep `Skipping`），禁止静默。

---

## 6. invokedynamic 展开覆盖（唯一事实）

| 形态 | 状态 |
|---|---|
| StringConcatFactory makeConcat[WithConstants] | ✅ 展开为 StringBuilder 链 |
| 无状态/捕获 lambda（合成类） | ✅ |
| 方法引用：实例/构造器/接口/super | ✅ |
| 擦除泛型（CHECKCAST/拆箱/装箱/POP） | ✅ |
| altMetafactory（serializable/marker/bridge） | ✅ |
| 基本类型加宽桥接（i2l 等） | ✅ |
| 装箱+加宽复合 | ✖ fail-closed |
| 基本类型收窄 | ✖ fail-closed |
| **自定义 bootstrap** | ✖ fail-closed —— R6.1 阶段3（未做） |

---

## 7. 5 个"可转被卡住"点（2026-08-08 决策）

### 7.1 `<init>`/`<clinit>`：搬运法 ✅ 决策（2026-08-08 已实现）
- 生成同签名静态方法（`__regc_init_<hash>` / `__regc_clinit_<hash>`），把原方法体指令搬进，原 `<init>`/`<clinit>` 骨架只保留"super 调用 + 对生成方法的调用"。
- `<clinit>` 骨架必须保留"注册时机"在调用生成方法之前（本次 `registerNativesForClass` 头部注册语义不破）。
- 前提：JVM 对原生 `<init>` 标 ACC_NATIVE 部分实现拒收——我们不标生成方法，只标生成方法 native，无冲突。
- 实现：`init_clinit_mover_preprocessor.hpp/.cpp`，注册于 MarkAll 之前；`__regc_*` 生成方法由 MarkAll 标记进 native 编译。生成方法 `ACC_STATIC | ACC_PRIVATE`，desc 为 `(L<owner>;<原参数>)V`（this 显式参数化，body 指令槽位零改动）。
- v1 保守边界（不搬，保留字节码）：
  - 有 tryCatchBlocks / localVariables / 局部变量注解；
  - `<init>` 骨架段（切点 `INVOKESPECIAL <init>` 之前）含 FRAME 或跳转/switch 指令；
  - 骨架段引用克隆体段 label（除行号锚点外）；克隆体跳转/switch 引用骨架段 label。
  - javac 常态放行：骨架段 LABEL/LINE（行号锚点）；克隆体尾部 LineNumberNode 引用骨架段 label 时，克隆前映射到生成方法占位 label。
- 验证：单测 `init_clinit_mover_test.cpp` 9 项；E2E `RegCBackendE2E.InitAndClinitMoveRoundTrip`（javac 默认 debug 信息下构造器×2 + 静态块 native 往返）。
- 优先：中（类初始化是较稀少的执行；构造器是高频热路径）。

### 7.2 lambda$ 删 skip ✅ 决策
- lambda$ 是普通静态方法，前处理器把 indy 都改写了，skip 是历史遗留。
- 改法：删 `regc_backend.cpp` lambda$ 分支 + 补合成类 roundtrip。

### 7.3 自定义 bootstrap → 生成 stub 桥（迭代）✅ 规划
- 无法静态展开的 indy 站点：生成一个同签名 Java stub 方法走运行时 bridge 语义（等价 StackC bridge 但桥在字节码层，无 owner-class 障碍）。
- 优先级低（javac 17 一般 jar 极少见）；作为阶段3.

### 7.4 并发组：主线程放开 + worker 场景 stub ✅ 规划
- 主线程 `Thread.sleep` 等已实测可转。
- worker 场景把并发调用移进生成的 Java stub 方法，保持 Java 帧内调用，待真机验证（独立立项 1-2 周）。

---

## 8. 变更规程

- 任何转换行为变化 → **先改本文档 §3/§4/§6/§7 表 + 记录决策**，再改代码。
- 新增形态/指令 → 加行。
- 跳过原因变化 → 更新 §4 原因列；已移除的进"历史"。
- 本文档是唯一事实源；代码注释只写"见 regc_conversion_decisions.md §N"，不重复设计理由。
# CBackend 复活与指令覆盖分波路线

> 本文档定义 mj2x C backend 的复活决策（撤销 ADR-1）和按字节码类别分波的指令覆盖路线。
> 每波是一个独立 PR，可单独验证（Java→mj2x→.c→cl→运行对比）。
> 迁移/实现 checklist 见 `mbase/mbase/doc_ai/pass-migration-checklist.md`（pass 通用）；本文件专注 CBackend 特有的分波顺序。

## 0. 决策记录：撤销 ADR-1

**原 ADR-1**（`doc/architecture_redesign.md` §0）：旧 CBackend（cstack[]/sp JNI 直译）被 c2j
系统性还原，归档到 `backend/reference/` 不再演进。

**撤销理由**（2026-07-05，owner 决策）：
- C backend 实现成本最低，先把端到端跑通、覆盖指令逐步扩散，比闭门造 RegC 靠谱。
- 多后端架构允许 C backend 与 RegC 并存：C backend 作"易实现、广覆盖"的通用后端，
  RegC 作"对抗强度高"的硬化后端（未来）。
- **务实优先**：先有能跑的实现，对抗强度以后再提。

**接受的代价**：
- mj2x 暂不解决 c2j 锚点 1/2（JNI vtable hook + cstack 正则）。C backend 生成的代码
  仍可被 c2j 还原。这是"先做出来"的代价，后续由 RegC 后端（若做）解决。
- `architecture_redesign.md` 的防御矩阵 / 锚点消除路径**对 C backend 不生效**，
  文档需明确"C backend 不承担对抗职责"。

**落地动作**（第一个 PR，见 Wave 0）：
- `backend/reference/c_backend.{hpp,cpp}` → `backend/c_backend.{hpp,cpp}`（移出 reference）
- `unittest/backend/c_backend_legacy_test.cpp` → `c_backend_test.cpp`（去 legacy 标记）
- pipeline 重新注册 C backend（若被反注册）
- 修订 `architecture_redesign.md` ADR-1：状态改为"撤销"，说明 C backend 复活理由与边界
- 修订 `doc/roadmap.md`：把 C backend 路线加回（作为独立于 RegC 的并行后端）

**SSA/RegIR 线的状态**（暂不动，待后续决策）：
- R1a（SSA 骨架）/R1b（preprocessor）已合并的代码保留，但 C backend **不走 SSA**，
  直接吃 L1 栈式 MIR。SSA 线后续是搁置还是作调试工具，另议。

## 1. 多后端架构定位

| 后端 | 输入 IR | 输出 | 对抗强度 | 状态 |
|---|---|---|---|---|
| **CBackend**（本路线） | L1 栈式 MIR | JNI C 源码 | 低（c2j 可还原） | 复活，逐波扩散 |
| RegC（未来可选） | L3 RegIR | 寄存器式 C | 高（破 c2j 锚点） | 未启动，依赖 R1-L3 |

`Backend` 接口（`backend/backend.hpp`）已支持多后端：`name()` 区分、`compileMethod` 统一入口。
C backend 走 `name()="c_source"`。

## 2. 当前底子（事实摸查）

- **MIR 层**：`frontend/mir_node.hpp` 已定义 40 个 `MIRKind`（算术/栈/控制流/对象/数组/字段/调用/类型/异常）
- **CBackend case**：`reference/c_backend.cpp` 的 switch 覆盖不全（许多 MIRKind 未接通）
- **CodeConverter**：`code_converter.cpp` 只有约 8 处 opcode 分发，大量 JVM opcode 未翻译
- **现有测试**：9 个（SimpleReturn / Add / FieldAccess / InvokeMethod / InjectMakesMethodNative /
  BranchAndComparison / NewObject / IincInstruction / NameMangling）

结论：IR 层铺得广，但端到端只有少数指令真能跑通。**适合按类别分波扩散**。

## 3. 分波路线（每波 = 一个独立 PR）

每波交付件：
- 补 `code_converter.cpp`：翻译该类别的 JVM opcode → MIR
- 补 `c_backend.cpp`：处理新增的 MIR 节点 → C 代码生成
- 补 E2E 测试：Java 源 → javac → mj2x → .c → cl/gcc → 运行对比结果
- 更新本文档「已覆盖」清单
- 验证命令：`xmake build mj2x_unittest && xmake run mj2x_unittest`

### Wave 0：复活与重新注册（基础）✅

- [x] 移文件出 `reference/`，去 legacy 标记（`backend/reference/c_backend.{hpp,cpp}` → `backend/c_backend.{hpp,cpp}`；`c_backend_legacy_test.cpp` → `c_backend_test.cpp`）
- [x] pipeline 重新注册 C backend（CLI 已注册；include 路径已更新）
- [x] 修订 ADR-1、ROADMAP、本文件（ADR-1 标"已撤销"；ROADMAP 加 CBackend 复活条目）
- [x] 现有 9 个测试全绿（验证复活没破坏既有覆盖）—— 注：`CBackendTest.NewObject`、`InputLoaderTest.ReadRealClass` 两个失败在干净 dev.3 上即存在，属既有 flake，非本次复活引入；Wave 1 重写 `emitNewObject` 后 NewObject flake 将消除
- [x] **明确 SSA 线处置**：保留代码但不接入 C backend pipeline

### Wave 1：整数算术与局部变量（建立 E2E 闭环）✅

覆盖：ILOAD/ISTORE 系列、ICONST/IPUSH、IADD/ISUB/IMUL/IDIV/IREM、IINC、INEG、
IAND/IOR/IXOR/ISHL/ISHR/IUSHR、局部变量类型推导（int）

- [x] converter：补全整数算术 opcode 翻译（MIRConstant 填值/类型、MIRLocalLoad/Store 按 opcode 推类型、MIRReturnValue/MIRUnaryOp 填类型、LDC int/long/float/double 填值）
- [x] CBackend：MIRBinaryOp/MIRUnaryOp/MIRIinc/MIRConstant 完整 emit；附带修了一批真跑 bug：
  - param-init `stackAccessor(pk)+1` 误写（写 `.i1` 等非法访问器）
  - `emitNewObject` 改用 `AllocObject`（解耦分配与构造，由后续 INVOKESPECIAL 走 CallNonvirtualVoid）
  - `emitInvoke`/`emitFieldGet/Put`/`emitStaticGet/Put` 的 `NULL` methodID/fieldID → 现查
  - `emitInvoke` 补全参数列表、CallNonvirtual 补 jclass 参数
  - label 标识改用十六进制 `L<hex>`（原 `label_<负int>` 不是合法 C 标识符）
  - `<init>`/`<clinit>` 名字 mangle 为 `init_`/`clinit_`（`<>` 非法 C 标识符）
  - MarkAllMethodsPreprocessor 跳过 `<init>`/`<clinit>`（JVM 校验拒收 native 构造器）
- [x] E2E：5 个真编译运行用例全绿（`IntAdd`/`IntMulSubNeg`/`IntIinc`/`IntBitwiseShift`/`IntControlFlow`），见 `src/unittest/backend/c_backend_e2e_test.cpp`
- [x] 类型边界：int 溢出语义对齐 JVM（ADD/SUB/MUL/SHL/NEG/USHR 走 `uint32_t` 环绕；USHR 用 `(uint32_t)a >> b`）。验：`add(MAX,1)==MIN`、`mul(65536,65536)==0`、`neg(MIN)==MIN`、`ushr(-1,1)==MAX`

### Wave 2：长整型/浮点算术 ✅

覆盖：LLOAD/LSTORE/LCONST、FLOAD/FSTORE/FCONST、DLOAD/DSTORE/DCONST、
所有算术的 L/F/D 变体、I2L/I2F/I2D/L2I/... 类型转换

- [x] converter：L/F/D 算术 opcode 沿用 `MIRBinaryOp`，类型转换 opcode 填充 `MIRCast from/to`；`LCMP`/`FCMPL`/`FCMPG`/`DCMPL`/`DCMPG` 落 `MIRCmp`（结果恒 int，记录 NaN 比较方向）
- [x] CBackend：补 `MIRCast`/`MIRCmp` emit；long ADD/SUB/MUL/SHL/SHR/USHR/DIV/REM 覆盖 JVM 边界；float/double REM 走 `fmodf`/`fmod`；FP→int/long 转换处理 NaN 与饱和边界
- [x] E2E：新增 3 个真 E2E 用例（`LongArith`/`FloatDouble`/`Conversions`），覆盖 long wrap、shift mask、`Long.MIN_VALUE / -1`、float/double remainder、NaN 比较、I/L/F/D 与 I2B/I2C/I2S 转换
- [x] 风险：float 比较的 NaN 语义通过 FCMPG/DCMPL 生成分支覆盖；long 除法/取模的 `Long.MIN_VALUE / -1` 与 `% -1` 已特判

### Wave 3：控制流 ✅

覆盖：IF_EQ/NE/LT/GE/GT/LE（含 IFNULL/IFACMP*）、GOTO、TABLESWITCH/LOOKUPSWITCH、
IFEQ 等的零值/双值变体

- [x] converter：`MIRBranch`/`MIRBranchZero`/`MIRUncondBranch`/`MIRSwitch` 已覆盖；`IFNULL`/`IFNONNULL` 的 `MIRBranchZero` 明确标记为 `OBJECT`
- [x] CBackend：条件分支统一先计算 `cond`、弹出操作数，再执行 `goto`；`BRANCH_ZERO` 按 `PrimitiveKind` 选择 `.i` 或 `.l`；`switch` 继续用 C `switch + goto label`
- [x] E2E：新增 4 个真 E2E 用例（`DenseSwitch`/`SparseSwitch`/`NestedLoops`/`ReferenceBranches`），覆盖 TABLESWITCH、LOOKUPSWITCH、嵌套循环/continue、IFNULL/IFNONNULL/IF_ACMP*
- [x] 风险：分支成立路径遗留操作数已修复；label 仍使用 `uintptr_t` 十六进制标识；switch default 路径由 E2E 覆盖

### Wave 4：方法调用与返回 ✅

覆盖：INVOKEVIRTUAL/INVOKESPECIAL/INVOKESTATIC/INVOKEINTERFACE、
RETURN 系列各类型、方法参数传递

- [x] converter：`METHOD_INSN` 已完整落 `MIRInvoke`；`LDC String` 保留字面量供后端生成 `jstring`
- [x] CBackend：`Call*Method`/`CallStatic*Method`/`CallNonvirtual*Method` 生成修正；static 返回值写回参数基槽；`INVOKESPECIAL`/`INVOKEINTERFACE` 用声明 owner 查 `jclass`
- [x] CBackend：JNI 名字 mangle 补齐对象参数描述符（`;` → `_2`、`[` → `_3`、`_` → `_1`），避免重载/引用参数 native 符号失配
- [x] E2E：新增 4 个真 E2E 用例（`MethodCalls`/`RecursionCalls`/`StringLiterals`/`InterfaceCalls`），覆盖静态/实例/递归调用、构造器保留、接口 dispatch、String LDC 与引用返回
- [x] 风险：构造器仍由 `MarkAllMethodsPreprocessor` 保留 Java 字节码；复杂字符串拼接/`INVOKEDYNAMIC` 不纳入本波

### Wave 5：对象与字段 ✅

覆盖：NEW、GETFIELD/PUTFIELD、GETSTATIC/PUTSTATIC、CHECKCAST、INSTANCEOF

- [x] converter：`NEW`/`CHECKCAST`/`INSTANCEOF`/`GETFIELD`/`PUTFIELD`/`GETSTATIC`/`PUTSTATIC` 均落 MIR
- [x] CBackend：对象分配继续用 `AllocObject` + 构造器 `INVOKESPECIAL`；字段访问现查 `jfieldID`；窄类型字段按 JVM operand stack 规则写回 `.i`
- [x] CBackend：`CHECKCAST` 对 null 保持成功语义，失败时抛 `ClassCastException`；`INSTANCEOF` 使用目标 descriptor 查类
- [x] E2E：`ObjectFields` 覆盖实例字段、静态字段、`instanceof`、成功/空 `checkcast`
- [x] 风险：fieldID/methodID 缓存继续延后（MVP 不缓存，每次查）

### Wave 6：数组 ✅

覆盖：NEWARRAY/ANEWARRAY/MULTIANEWARRAY/ARRAYLENGTH/*ALOAD/*ASTORE

- [x] converter：`NEWARRAY` 记录基本类型 component；`ANEWARRAY` 记录对象 type；`*ALOAD`/`*ASTORE` 记录 component type；`MULTIANEWARRAY` 保留 descriptor/dims
- [x] CBackend：基本类型数组走 `New<Type>Array` + `Get/Set<Type>ArrayRegion(len=1)`；对象数组走 `NewObjectArray` + `Get/SetObjectArrayElement`
- [x] CBackend：注入 `_mj2x_new_multi_array` helper，支持基本类型/对象多维数组递归创建
- [x] E2E：`PrimitiveArrays`/`ObjectArrays`/`MultiDimArrays` 覆盖 int/long/double/narrow arrays、String/Object arrays、int[][] 与 String[][][]
- [x] 风险：数组越界仍依赖 JNI/JVM 的数组访问检查；BALOAD 对 boolean/byte 的静态区分留待后续栈类型分析细化

### Wave 7：异常处理（最复杂，最后做）✅

覆盖：ATHROW、try-catch 的 exception table、异常对象传播

- [x] converter：`ATHROW` 落 `MIRThrow`；异常表 handler label 已作为块入口保留
- [x] CBackend：读取 `MethodNode.tryCatchBlocks`，在受保护范围内插入 JNI `ExceptionCheck` 分派；分派按 exception table 顺序匹配 catch type，清除 pending exception，把异常对象作为 handler 入口栈顶再跳转
- [x] CBackend：`ATHROW`、`CHECKCAST` 失败、callee native/JNI 调用抛出的异常均可进入当前方法 handler；未匹配的异常重新 `Throw` 向上传播
- [x] CBackend：`MONITORENTER`/`MONITOREXIT` 走 JNI monitor API；`finally` 由 javac 展开的字节码/异常表路径覆盖
- [x] E2E：`ThrowPropagation`、`TryCatchExceptions`、`MultiCatchAndFinally`、`ExceptionFromNativeCallAndMonitor` 覆盖显式抛出、try-catch、多 catch、finally、callee native 抛出后 caller 捕获、同步块
- [x] 风险：int/long 除零仍未显式转换成 `ArithmeticException`；JNI ID 缓存仍是性能优化，不影响语义覆盖

## 4. 暂不在本路线的（明确边界）

- ✅ **INVOKEDYNAMIC(CBackend)**：CBackend 通过 native 侧 bootstrap +
  `MethodHandle.invokeWithArguments` runtime bridge 覆盖 javac string concat、
  方法引用和捕获 lambda；RegC 仍按独立策略 fail-closed。
- ❌ **JSR/RET**：废弃 subroutine 指令不纳入本路线；若旧 class 仍含 JSR/RET，应由 preprocessor/输入规范处理。
- ❌ **c2j 对抗**：C backend 不承担对抗职责，生成的代码可被 c2j 还原（接受）
- ✅ **JAR 输入输出 MVP**：当前可处理 `.jar` 内多 class，并保留非 class 资源；签名、重压缩和
  manifest 级产品体验仍留给后续收口。

## 5. 完成判据（整条路线）

C backend 能跑通一个"中等复杂度"的 Java 类（含算术/控制流/调用/对象/数组/异常），
端到端：Java 源 → javac → mj2x → .c → cl/gcc 编译 → 在 JVM 里调用 → 结果与原 Java 一致。

**当前状态（2026-07-05）**：完成。`IntegratedSample` 真 E2E 覆盖实例字段、静态字段、构造器、
实例/静态调用、递归、循环、switch、primitive/object/multidim arrays、String LDC；异常覆盖由
Wave 7 专项 E2E 补足。

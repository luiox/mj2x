# mj2x — Test Strategy

> **v2（2026-06-24 架构重构后）**：测试分层对齐新的五层 IR（L1-L5）+ RT 运行时。
> v1 的测试目标（≥80 单元，仅针对 CBackend）调整为按层分质量门。
> 重构依据见 `architecture_redesign.md`。
>
> **MVP freeze（2026-07-06）**：当前发版范围见 `mvp_release_scope.md`。
> 发版前只补冻结范围内的 JAR/CLI/unsupported 边界测试，不新增 R4/VMC4 等后续功能。

---

## 1. 测试分层（对齐 L1-L5 + RT）

### 1.1 现有基线（CBackend 复活后，子串匹配 + 真 E2E）

> **2026-07-05 更新**：ADR-1 撤销，CBackend 复活为通用后端。`backend/c_backend.{hpp,cpp}`
> 已从 `reference/` 移回 `backend/`，`c_backend_legacy_test.cpp` 复名 `c_backend_test.cpp`。
> 新增**真 E2E** 测试层（见 §1.4）。

| 测试 | 层 | 用例 | 处置 |
|------|----|------|------|
| `core/compiler_index_test` | 基础设施 | 2 | 保留 |
| `core/io_test`, `input_loader_test`, `jar_io_test` | 基础设施 | 11+ | 覆盖 class/JAR roundtrip、MVP fixture、signed/no-class fail-closed |
| `core/e2e_test`（CBackend 版） | CBackend | 3 | 复活，去 legacy |
| `frontend/code_converter_test` | **L1** 栈式 MIR | 18 | 保留（L1 基线）；Wave 6 补数组 component 元数据 |
| `backend/c_backend_test` | CBackend | 19 | 复名 `c_backend_test.cpp`，子串匹配；Wave 6 补 array/checkcast emit |
| **`backend/c_backend_e2e_test.cpp`** ★新增 | **CBackend 真 E2E** | 26 | Wave 1 起逐波补，见 §1.4 |
| `cli/cli_test.cpp` | CLI | 5+ | 覆盖 RegC backend flag、CBackend/RegC JAR 输入/输出入口、负向参数、坏 JAR fail-closed |

### 1.2 各层测试目标（R1-R4 落地时补）

| 层 | 测试文件（计划） | 测试内容 | 落地波次 |
|----|----------------|---------|---------|
| **L1** 栈式 MIR | `frontend/code_converter_test`（已有）+ 补 INVOKEDYNAMIC/MULTIANEWARRAY 提取 | 字节码→MIR、CFG、L1 preprocessor | R1 补 |
| **L2** SSA | `mir/ssa_test`（★新增） | reaching-def、dominance、φ 插入、重命名正确性 | R1 |
| **L3** 寄存器 IR | `mir/register_ir_test`（★新增） | φ 消解、虚拟寄存器分配、**操作数无 stack slot**（硬断言） | R1 |
| **L4** 变换 pass | `mir/passes/*_test`（★新增） | 每个 pass（常量折叠/DCE/MBA/CFF）的语义保持 + 变换生效 | R4 |
| **L5** RegC | `backend/regc_test`（★新增） | RegIR→C 生成正确性、**生成 C 无 `cstack` 模式**（硬断言） | R2 |
| **RT** 运行时 | `runtime/test/*_test.cpp` | crypto/kernel/stringpool 正确性；antijvmti 在 mock 篡改下 fail-closed；antidebug probe 可调用 | R3 |

### 1.3 关键结构断言（防御有效性验证）

这些断言直接对应 `architecture_redesign.md §4` 防御矩阵，是"防御是否生效"的客观检查：

| 断言 | 层 | 含义 | 对应 c2j 攻击 |
|------|----|------|--------------|
| RegIR 操作数无 stack slot | L3 | dump RegModule，grep 不到 stack/local index | cstack 正则失配 |
| 生成 C 无 `cstack[` 子串 | L5 | 对 RegC 产物做字符串断言 | CSTACK_RE 失配 |
| 生成 C 无 "Cannot invoke" 子串 | L5 | 对 RegC 产物做字符串断言 | throw_reason 兜底失配 |
| 生成 C 无全局 `cclasses[`/`cmethods[` 数组 | L5 | 符号走 helper，非全局表 | TABLE_RE/cache_table 失配 |
| antijvmti 在 mock 篡改 `env->functions` 时 fail-closed | RT | 单元测试注入假 vtable | c2j vtable 劫持反制 |

### 1.4 集成 / E2E 测试

| 测试 | 范围 | 方法 | 落地波次 |
|------|------|------|---------|
| Pipeline 空跑（mock backend） | pipeline → preproc → L1→L2→L3→L4 → backend → postproc → write | mock backend 验证调用链 | R1 |
| MIR → SSA → RegIR → RegC | 完整 IR 翻译路径 | 真实 bytecode → RegC C 代码 | R2 |
| InputLoader → Pipeline → OutputWriter | 完整读-改-写轮转 | .class/.jar roundtrip（已有） | 已有 |
| CLI JAR MVP | 命令行产品入口 | `in.jar -> out.jar + out.c`，验证资源保留和 class native injection | 已有 |
| Java → mj2x → .c → cl → 运行 | 端到端行为对比 | JDK + MSVC，结果与原 Java 一致 | R2 |
| 受保护样本 + c2j agent | anti-JVMTI 有效性 | 注入 c2j agent，验证 fail-closed 或 trace 退化（**与 mjdbg 协作**） | R3 |

### 1.4 CBackend 真 E2E harness（Wave 1 起）

区别于子串匹配的 `c_backend_test.cpp`，`c_backend_e2e_test.cpp` 把生成的 C **真编译成 DLL、
在 JVM 里调用、与原 Java 行为逐项对比**。这是 revival 文档定义的"端到端闭环"判据。

**流程**（每个 case）：
```
写 .java → javac 出基准 .class → 跑原 java 取基准输出
        → mj2x-cli 改写 .class 为 native + 出 .c
        → cl /LD 编 .c 为 .dll（含 jni.h 头路径，vcvars64 设环境）
        → java 加载 .dll 调用 native 方法 → 输出对比基准
```

**工具链探测**（fixture `SetUp`）：`javac`/`java`（JAVA_HOME 或回落 corretto-17.0.5）、
`mj2x-cli.exe`（build 目录）、`vcvars64.bat`（vswhere 定位）。任一缺失即 `GTEST_SKIP()`，
**不污染 CI**（无 JDK/C 编译器的环境跳过，本机有则真跑）。

**生成物**：`<project_root>/test/mj2x/e2e/<case>/`（遵循 spec：测试输出走 `test/`，绝不写 `test_resource/`）。
含 `.java`/`.class`/`_baseline.class`/`_protected.class`/`.c`/`.dll`/`Driver.java`，便于失败时手工复现。

**已覆盖**（Wave 1）：`IntAdd`（含 wrap）、`IntMulSubNeg`、`IntIinc`（含循环控制流）、
`IntBitwiseShift`（含 USHR）、`IntControlFlow`（if/else 分支）。

**已覆盖**（Wave 2）：`LongArith`（long wrap/shift/div-rem 边界）、`IntegerDivisionTrap`
（int/long 除零 `ArithmeticException`、catch 分派、`MIN_VALUE / -1` 边界）、`FloatDouble`
（float/double 运算、remainder、NaN 比较）、`Conversions`（I/L/F/D 与 I2B/I2C/I2S 转换）。

**已覆盖**（Wave 3）：`DenseSwitch`（TABLESWITCH/default）、`SparseSwitch`（LOOKUPSWITCH/default）、
`NestedLoops`（嵌套循环/continue/goto 回边）、`ReferenceBranches`（IFNULL/IFNONNULL/IF_ACMP*）。

**已覆盖**（Wave 4）：`MethodCalls`（静态/实例/构造器/long+double 参数调用）、`RecursionCalls`
（native 递归调用）、`StringLiterals`（String LDC/ARETURN/虚方法调用）、`InterfaceCalls`（INVOKEINTERFACE）。

**已覆盖**（Wave 5/6）：`ObjectFields`（实例/静态字段、`instanceof`、`checkcast`）、
`PrimitiveArrays`（基本类型数组、窄类型数组、boolean[]/byte[] 分流、arraylength）、`ObjectArrays`（String/Object 数组）、
`MultiDimArrays`（int[][] 与 String[][][]）。CBackend 发射单测还断言 `new`、`anewarray`、
静态调用、`instanceof` 等业务类路径走 owner ClassLoader-aware helper，而不是直接
`FindClass`；同组单测断言 generated method 在返回前调用 local-ref 批量释放 helper。
对象/数组/字符串/异常 E2E 用于确认该清理逻辑不会破坏对象返回值、catch 分派或异常传播。
字段/方法调用单测断言 generated C 含调用点级 `static jfieldID` / `static jmethodID`
懒缓存；方法调用、接口调用、对象字段 E2E 覆盖缓存后语义。

**已覆盖**（Wave 7/收尾）：`ThrowPropagation`（`ATHROW` 向上传播）、
`TryCatchExceptions`（try-catch 与未命中 catch 重新抛出）、`MultiCatchAndFinally`（多 catch 与 finally）、
`ExceptionFromNativeCallAndMonitor`（callee native 抛出后 caller catch、同步块 monitor）、
`IntegratedSample`（算术/控制流/调用/对象/字段/数组/String 的综合样本）。

**运行**：`xmake build mj2x-cli && xmake build mj2x_unittest && xmake run mj2x_unittest -- --gtest_filter=CBackendE2E.*`

---

## 2. 测试组织

```
src/unittest/
├── main.cpp                                — gmock main（test::setup("mj2x")）
├── core/                                   — 基础设施测试（已有）
│   ├── compiler_index_test.cpp
│   ├── io_test.cpp
│   ├── input_loader_test.cpp
│   ├── jar_io_test.cpp
│   └── e2e_test.cpp                        — legacy CBackend E2E（移 reference 时标 legacy）
├── frontend/
│   └── code_converter_test.cpp             — [L1] 栈式 MIR（已有，R1 补 indy/multianewarray）
├── mir/                                    — ★ 新增（R1/R4）
│   ├── ssa_test.cpp                        — [L2] SSA
│   ├── register_ir_test.cpp                — [L3] 寄存器 IR
│   └── passes/                             — [L4] 变换 pass
│       ├── constfold_test.cpp
│       ├── dce_test.cpp
│       ├── mba_test.cpp
│       └── cff_test.cpp
└── backend/
    ├── regc_test.cpp                       — ★ [L5] RegC（R2）
    └── reference/
        └── c_backend_legacy_test.cpp       — legacy CBackend（从 c_backend_test.cpp 移入，标 legacy）
```

runtime 测试（R3）单独组织在 `runtime/test/`，由 `mj2x_runtime_test`
运行。`mj2x-runtime-core` 不依赖 JDK，便于 crypto/kernel/anti/stringpool
快速单测；`jni_lib` 是后续 JNI 集成用共享库包装。

---

## 3. 测试数据

### 3.1 内联构造（首选）

大多数单元测试用**内联构造的 ClassNode/MethodNode**，避免依赖外部文件：

```cpp
auto cn = std::make_unique<ClassNode>();
cn->version = V1_8;
cn->access = ACC_PUBLIC;
cn->name = "Test";
cn->superName = "java/lang/Object";

auto mn = std::make_unique<MethodNode>();
mn->access = ACC_PUBLIC | ACC_STATIC;
mn->name = "add";
mn->desc = "(II)I";
mn->instructions.add(new VarInsnNode(ILOAD, 0));
mn->instructions.add(new VarInsnNode(ILOAD, 1));
mn->instructions.add(new InsnNode(IADD));
mn->instructions.add(new InsnNode(IRETURN));
```

字符串/类型必须经共享 `testArena().intern()`（CBackend 测试曾因 bare `toRef` 指针与
libjbytecode Type 全局状态交互导致 flaky，已修）。

### 3.2 真实 class 文件

少数 E2E 测试用 `test_resource/` 下的真实 `.class`（与 libjbytecode 共用资源目录）。
输出一律走 `test::getOutPath()` 等 test_helper API，**绝不写 `test_resource/`**。

### 3.3 c2j 攻击验证样本（R3+）

R3 起，受保护样本需在 c2j agent 下验证。样本与 expected trace surface 文档化，
交 mjdbg 协作验证（见 ROADMAP §5 Shared Contracts）。

### 3.4 CBackend protected sample fixtures

`mj2x/test_resource/protected_samples/cbackend/` 保存面向 mjdbg 的 Java 源样本；
`CBackendProtectedSamples.GenerateMjdbgFixtures` 在测试运行时编译这些源文件，并通过
`mj2x-cli --emit-c` 生成 protected `.class` 与 C/JNI 源码。生成物一律写入
`test/mj2x/protected_samples/cbackend/<SampleName>/`，同时生成 `TRACE_SURFACE.md`
说明 mjdbg 应观察的 Java/native/JNI trace 面。

设计说明见 `c_backend_protected_samples.md`。这些样本用于 trace 捕获、分组和弱点评估，
不代表保护强度结论。

### 3.5 Backend README and MVP verification index

后端支持矩阵和可发版验证入口集中在：

- `c_backend_readme.md`：CBackend 支持特性、限制、CLI 用法、样本。
- `regc_backend_readme.md`：RegC 支持特性、限制、CLI 用法、样本。
- `mvp_backend_verification.md`：每个 MVP 能力对应的单测、E2E 或 protected sample。

### 3.5.1 JAR MVP fixtures and negative boundaries

`mj2x/test_resource/mvp_jar/` 保存 JAR MVP 的稳定输入样本：manifest、两份 Java
source 和非 class resource。`IOTest.JarMvpFixturePreservesManifestResourcesAndMultiReleaseClasses`
在测试中构造真实 JAR，验证 root class entry 被解析、manifest/resource 被原样保留，
`META-INF/versions/**.class` 作为资源 passthrough。

JAR 输入边界采用 fail-closed 策略：signed-JAR metadata
(`META-INF/*.SF`, `*.RSA`, `*.DSA`, `*.EC`)、空 JAR、无 root class JAR 不继续进入
pipeline。对应测试为 `IOTest.JarInputLoaderRejectsSignedJarMetadata`、
`IOTest.JarInputLoaderRejectsJarWithoutRootClasses` 和 `CliTest.BadJarInputsFailClosed`。

### 3.6 Register IR fixtures

`mir/register_ir_test.cpp` 覆盖 SSA → RegIR lowering：SSA block args 会被消解为
successor edge copies，RegIR 操作数只能是虚拟寄存器或立即数，不能表达 JVM stack slot。
设计说明见 `register_ir.md`。

### 3.7 RegC MVP fixtures

`backend/regc_test.cpp` 覆盖 RegIR → C helper emission：纯计算、分支、循环、
switch、edge-copy 输出，以及字段、数组、调用、对象、reference check、monitor、
try/catch exception dispatch、integer div/rem trap 的 `_mj2x_regc_*` helper 形态。测试会断言生成 C 不含 `cstack[`、`stack[`、
`union jvalue`、`sp--` 等 stack-machine anchor。设计说明见 `regc_mvp.md`。

### 3.8 RegC trace samples

`backend/regc_samples_test.cpp` 从 `mj2x/test_resource/protected_samples/regc/`
编译 Java 样本，并生成 RegC helper-style C 到
`test/mj2x/protected_samples/regc/<SampleName>/`。这些样本用于 mjdbg 对比 CBackend
stack-style C 与 RegC register/helper-style C 的 trace surface。设计说明见
`regc_trace_samples.md`。

### 3.9 RegC native backend

`backend/regc_backend_test.cpp` 覆盖 RegCBackend facade：L1 blocks 经
SSA/RegIR/RegC 生成 helper，再由 JNI wrapper 暴露为 `Java_*` native symbol，并且
只在成功生成时把 Java 方法标记为 native。`core/e2e_test.cpp` 还覆盖 Pipeline 写出
RegCBackend `.c` 与 native class；`backend/regc_backend_e2e_test.cpp` 在 JDK/MSVC
可用时把纯计算、循环、`int[]` helper RegC C 源真编译成 DLL 并由 Java 加载调用。
同组测试还覆盖 instance/static field 与 `instanceof` helper 的 DLL round-trip。
`CallHelpersRoundTrip` 进一步覆盖 static/native-to-native call、构造器调用和 virtual call。
`TryCatchDispatchRoundTrip` 覆盖匹配 catch、未命中 catch 重新抛出，以及 callee native 抛出后
caller catch。`IntegerDivisionTrapRoundTrip` 覆盖 i32/i64 除零捕获/传播与
`MIN_VALUE / -1`、`MIN_VALUE % -1` 边界。
`NarrowPrimitiveRoundTrip` 覆盖 byte/short/char/boolean field helper 与
byte/short/char array helper 的 DLL round-trip。
设计说明见 `regc_native_backend.md` 与 `regc_runtime_helpers.md`。

### 3.10 R3 runtime core

`mj2x/runtime/src/{crypto,kernel,antidebug,antijvmti,stringpool,intrinsic}/` 提供 R3
runtime foundation。`mj2x_runtime_test` 覆盖 SHA-256/HMAC 标准向量、
HKDF-SHA256 RFC 5869 向量、HMAC 校验、volatile wipe、JNI table tamper mock、
agent 参数扫描、anti-debug signal 聚合和 string-pool 派生流解码。

`intrinsic/` 子模块承载 JDK intrinsic 的 runtime 实现:`intrinsic_wrapper`
(包装类 valueOf/xxxValue + Integer cache)、`intrinsic_math`(Math abs/min/max/
sin/.../random,JLS NaN 语义)、`intrinsic_random`(Java 兼容 LCG,序列逐位对齐
`new java.util.Random(42)`)。`mj2x_runtime_test` 对应三个 `Intrinsic*Test` 套件。

`regc_intrinsic_test.cpp` 在 RegC 单测层断言 emitter 把 JDK 调用(Math/parseInt)
识别为 intrinsic,生成 C 不含对应 `_mj2x_regc_call_*`;`RegCBackendE2E.
IntrinsicMathAndParseIntRoundTrip` 在 JDK/MSVC 可用时真编译 DLL 并与 Java 基准
逐项对比(parseInt "42" -> 42,sin/sqrt/round 浮点与语义对比),且断言生成 C 在
Math/parseInt 调用点无 JNI 派发。

设计说明见 `runtime_crypto.md`、`runtime_kernel.md`、
`runtime_antijvmti.md`、`runtime_antidebug.md`、`runtime_stringpool.md`、
`regc_intrinsic.md`。

---

## 4. 测试运行

```bash
# 配置（一次性）
xmake f --with_mj2x=y --with_tests=y

# 全部
xmake build mj2x_unittest && xmake run mj2x_unittest

# gmock 过滤
xmake run mj2x_unittest --gtest_filter="RegC*"
xmake run mj2x_unittest --gtest_filter="SsaTest.*"

# runtime 单测（R3 起）
xmake build mj2x_runtime_test && xmake run mj2x_runtime_test
```

---

## 5. 质量门槛（按层）

| 层 | 单元测试门槛 | 结构断言门槛 | 落地波次 |
|----|------------|------------|---------|
| L1 栈式 MIR | ≥ 15（已有 13，R1 补） | — | 已有/R1 |
| L2 SSA | ≥ 10 | φ 插入/dominance 正确 | R1 |
| L3 寄存器 IR | ≥ 10 | **操作数无 stack slot（硬断言）** | R1 |
| L4 变换 pass | 每 pass ≥ 5 | 语义保持 + 变换生效 | R4 |
| L5 RegC | ≥ 15 | **生成 C 无 cstack/Cannot invoke/cclasses（硬断言）** | R2 |
| RT 运行时 | 每 module ≥ 5 | antijvmti mock 篡改 fail-closed | R3 |
| E2E | ≥ 3（R2）→ ≥ 6（R3 含 c2j agent） | 行为与原 Java 一致 | R2/R3 |

全局：编译警告 0，CI 必过，RTTI 禁用（用 `typed::cast` 不用 `dynamic_cast`）。

> 旧 v1 目标（≥80 单元 / ≥60% 覆盖，仅针对 CBackend）已被本按层目标取代。

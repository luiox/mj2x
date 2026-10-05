# mj2x — Test Strategy

> **v2（2026-06-24 架构重构后）**：测试分层对齐新的五层 IR（L1-L5）+ RT 运行时。
> v1 的测试目标（≥80 单元，仅针对 StackC）调整为按层分质量门。
> 架构概览见 `../doc/architecture.md`。
>
> 当前发版范围见 `../doc/roadmap.md` 与 `../dev_plan.md`。

---

## 1. 测试分层（对齐 L1-L5 + RT）

### 1.1 现有基线（StackC 复活后，子串匹配 + 真 E2E）

> **2026-07-05 更新**：ADR-1 撤销，StackC 复活为通用后端。`backend/stackc/stackc_backend.{hpp,cpp}`
> 已从 `reference/` 移回 `backend/stackc/`。2026-07-08 起正式更名为 StackC。
> 新增**真 E2E** 测试层（见 §1.4）。

| 测试 | 层 | 用例 | 处置 |
|------|----|------|------|
| `core/compiler_index_test` | 基础设施 | 2 | 保留 |
| `core/io_test`, `input_loader_test`, `jar_io_test` | 基础设施 | 11+ | 覆盖 class/JAR roundtrip、MVP fixture、signed/no-class fail-closed |
| `core/e2e_test`（StackC 版） | StackC | 3 | 复活，去 legacy |
| `frontend/code_converter_test` | **L1** 栈式 MIR | 18 | 保留（L1 基线）；Wave 6 补数组 component 元数据 |
| `backend/stackc_backend_test.cpp` | StackC | 19 | 子串匹配；Wave 6 补 array/checkcast emit |
| **`backend/stackc_backend_e2e_test.cpp`** ★新增 | **StackC 真 E2E** | 30 | 历史 inline case，见 §1.4 |
| **`integration/mtest_set_native_matrix_test.cpp`** | **双后端真 E2E** | 36 | 18 个中性 fixture x StackC/RegC |
| `cli/cli_test.cpp` | CLI | 5+ | 覆盖 RegC backend flag、StackC/RegC JAR 输入/输出入口、CMake 工程输出、负向参数、坏 JAR fail-closed |

### 1.2 各层测试目标

| 层 | 测试文件（计划） | 测试内容 | 落地波次 |
|----|----------------|---------|---------|
| **L1** 栈式 MIR | `frontend/code_converter_test`（已有）+ 补 INVOKEDYNAMIC/MULTIANEWARRAY 提取 | 字节码→MIR、CFG、L1 preprocessor | R1 补 |
| **L2** SSA | `mir/ssa_test`（★新增） | reaching-def、dominance、φ 插入、重命名正确性 | R1 |
| **L3** 寄存器 IR | `mir/register_ir_test`（★新增） | φ 消解、虚拟寄存器分配、**操作数无 stack slot**（硬断言） | R1 |
| **L4** 变换 pass | `mir/passes/*_test`（★新增） | 每个 pass（常量折叠/DCE/MBA/CFF）的语义保持 + 变换生效 | R4 |
| **L5** RegC | `backend/regc_test`（★新增） | RegIR→C 生成正确性、**生成 C 无 `cstack` 模式**（硬断言） | R2 |
| **RT** 运行时 | `mj2x-rt/test/*_test.c` | crypto/kernel/stringpool 正确性；antijvmti 在 mock 篡改下 fail-closed；antidebug probe 可调用 | R3 |

### 1.3 关键结构断言（防御有效性验证）

这些断言对应当前架构中 RegC 消除 stack-machine anchor 的目标，是"防御是否生效"的客观检查：

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

### 1.4 StackC 真 E2E harness（历史 inline case）

区别于子串匹配的 `stackc_backend_test.cpp`，`stackc_backend_e2e_test.cpp` 把生成的 C **真编译成 DLL、
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
`InvokeDynamicStringConcat` 与 `InvokeDynamicLambda` 覆盖 StackC native 侧
bootstrap + `MethodHandle.invokeWithArguments` 路径，包括 javac string concat、方法引用和捕获 lambda。

**已覆盖**（Wave 5/6）：`ObjectFields`（实例/静态字段、`instanceof`、`checkcast`）、
`PrimitiveArrays`（基本类型数组、窄类型数组、boolean[]/byte[] 分流、arraylength）、`ObjectArrays`（String/Object 数组）、
`MultiDimArrays`（int[][] 与 String[][][]）。StackC 发射单测还断言 `new`、`anewarray`、
静态调用、`instanceof` 等业务类路径走 owner ClassLoader-aware helper，而不是直接
`FindClass`；同组单测断言 generated method 在返回前调用 local-ref 批量释放 helper。
对象/数组/字符串/异常 E2E 用于确认该清理逻辑不会破坏对象返回值、catch 分派或异常传播。
字段/方法调用单测断言 generated C 含调用点级 `static jfieldID` / `static jmethodID`
懒缓存；方法调用、接口调用、对象字段 E2E 覆盖缓存后语义。
`StringConstantEmission` 断言重复 LDC 字符串复用同一个 `_mj2x_string_pool` offset，
`StackCBackendE2E.StringLiterals` 覆盖 pool 物化后的运行时行为。
`CliTest.GenerateCMakeProjectEmitsStandaloneFiles` 覆盖 `--generate-cmake` 输出
`native.c`、`CMakeLists.txt`、`.gitignore` 和 README。

**已覆盖**（Wave 7/收尾）：`ThrowPropagation`（`ATHROW` 向上传播）、
`TryCatchExceptions`（try-catch 与未命中 catch 重新抛出）、`MultiCatchAndFinally`（多 catch 与 finally）、
`ExceptionFromNativeCallAndMonitor`（callee native 抛出后 caller catch、同步块 monitor）、
`IntegratedSample`（算术/控制流/调用/对象/字段/数组/String 的综合样本）。

**运行**：`xmake build mj2x-cli && xmake build mj2x_unittest && xmake run mj2x_unittest -- --gtest_filter=StackCBackendE2E.*`

### 1.5 mtest_set 双后端功能集成矩阵

`mj2x_integration_test` 与 unit target 分离，消费
`mtest_set/src/manifest/semantic_fixture_manifest.hpp`。每个 fixture 依次生成 class bundle、
编译 Driver、运行 JVM baseline，然后分别调用真实 `mj2x-cli --backend stackc|regc --compile`，
加载 native library 并执行 `java -Xverify:all`。最终要求 manifest expected stdout、baseline、
StackC、RegC 四者一致。

当前覆盖 5 个 baseline fixture、Wave 1 的 G3/G6/G7 和 Wave 2 的 G9/G10/G11，
共 18 个 fixture，即 36 个后端 case。多 class `g6.virtual_interface` 同时验证 package、
interface dispatch 和 sidecar classpath 位置；`g10.monitor` 覆盖异常 handler 的 local
传递与 monitor 释放。
生成物位于 `test/mj2x/integration/<backend>/<case>/`。

**运行**：`xmake build mj2x_integration_test && xmake run mj2x_integration_test`

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
│   └── e2e_test.cpp                        — legacy StackC E2E（移 reference 时标 legacy）
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
        └── stackc_backend_legacy_test.cpp  — legacy StackC（仅历史计划；当前未使用）
```

runtime 纯逻辑单测随 `mj2x_rt_test` 运行（如
`mj2x_rt_protect_probe_test.c` 的探测解码器用例）；原 `mj2x-runtime`
独立测试树已随该库退役。

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

字符串/类型必须经共享 `testArena().intern()`（StackC 测试曾因 bare `toRef` 指针与
libjbytecode Type 全局状态交互导致 flaky，已修）。

### 3.2 真实 class 文件

少数 E2E 测试用 `test_resource/` 下的真实 `.class`（与 libjbytecode 共用资源目录）。
输出一律走 `test::getOutPath()` 等 test_helper API，**绝不写 `test_resource/`**。

### 3.3 c2j 攻击验证样本

受保护样本需在 c2j agent 下验证。样本与 expected trace surface 文档化，
交 mjdbg 协作验证（见 ROADMAP §5 Shared Contracts）。

### 3.4 StackC protected sample fixtures

`mj2x/test_resource/protected_samples/stackc/` 保存面向 mjdbg 的 Java 源样本；
`StackCBackendProtectedSamples.GenerateMjdbgFixtures` 在测试运行时编译这些源文件，并通过
`mj2x-cli --emit-c` 生成 protected `.class` 与 C/JNI 源码。生成物一律写入
`test/mj2x/protected_samples/stackc/<SampleName>/`，同时生成 `TRACE_SURFACE.md`
说明 mjdbg 应观察的 Java/native/JNI trace 面。

设计说明见 `samples/stackc_protected_samples.md`。这些样本用于 trace 捕获、分组和弱点评估，
不代表保护强度结论。

### 3.5 Backend README and MVP verification index

后端支持矩阵和可发版验证入口集中在：

- `stackc_backend_readme.md`：StackC 支持特性、限制、CLI 用法、样本。
- `regc_backend_readme.md`：RegC 支持特性、限制、CLI 用法、样本。
- `capabilities.md`：每个能力对应的单测、E2E 或 protected sample（能力→验证映射）。

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
设计说明见 `ir_design.md` §13。

### 3.7 RegC MVP fixtures

`backend/regc_test.cpp` 覆盖 RegIR → C helper emission：纯计算、分支、循环、
switch、edge-copy 输出，以及字段、数组、调用、对象、reference check、monitor、
try/catch exception dispatch、integer div/rem trap 的 `_mj2x_regc_*` helper 形态。测试会断言生成 C 不含 `cstack[`、`stack[`、
`union jvalue`、`sp--` 等 stack-machine anchor。设计说明见 `regc_codegen.md`。

### 3.8 RegC trace samples

`backend/regc_samples_test.cpp` 从 `mj2x/test_resource/protected_samples/regc/`
编译 Java 样本，并生成 RegC helper-style C 到
`test/mj2x/protected_samples/regc/<SampleName>/`。这些样本用于 mjdbg 对比 StackC
stack-style C 与 RegC register/helper-style C 的 trace surface。设计说明见
`samples/regc_trace_samples.md`。

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
设计说明见 `regc_codegen.md`（RegIR→C 生成规则、转换边界与运行时 ABI 总览，代码生成审查入口）与 `codegen_style.md`（命名/注释规范）。

### 3.10 运行时 C 层（mj2x-rt）

runtime C 源在 `mj2x/mj2x-rt/src/`，随 `mj2x_rt_test` 运行（C 级语义、
protect 探针、字符串族测试）。JDK intrinsic 识别在 RegC 单测层断言
（生成 C 无对应 JNI 派发）；exec 级与 Java 基准对拍在有 JDK+编译器的机器随
`mj2x_unittest` 的 CliTest E2E 进行。设计说明见 `regc_intrinsic.md`。
原 `mj2x-runtime` 整树已退役并入 mj2x-rt。

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

# C 运行时单测
xmake build mj2x_rt_test && xmake run mj2x_rt_test
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

> 旧 v1 目标（≥80 单元 / ≥60% 覆盖，仅针对 StackC）已被本按层目标取代。

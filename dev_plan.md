# mj2x — MVP 开发现状与接手指南

> ## 当前阶段：RegC/R3 闭环前加固期（2026-07-06 起）
>
> mj2x 已完成 CBackend Wave 0-7、SSA/RegIR、RegC native backend、RegC helper
> semantics、try/catch dispatch、integer traps、narrow primitive helpers, and
> the first R3 runtime core.
>
> **mj2x 进入架构重构期**，新主路线：栈式 MIR → SSA → 寄存器 IR → 变换 → 寄存器式 C/C++（RegC）
> + native 运行时。详见：
>
> - **`doc/ARCHITECTURE_REDESIGN.md`**（重构权威设计，最高优先级）
> - **`doc/ROADMAP.md`**（R0-R5 波次，替换旧 B0-B3）
> - **`doc/ARCHITECTURE.md`** v0.2
>
> CBackend 作为广覆盖/兼容性后端保留；RegC 是保护导向后端，后续工作围绕
> SSA readiness、R3 runtime ABI、mjdbg/c2j 反馈和 L4 transform passes 展开。
> 本文 §1-§8 是 CBackend 的实现记录与已知限制，仍作为该后端的参考。

---

## 0. 架构重构期工作（当前，2026-06-24 起）

2026-07-06 更新：CBackend 已按 `doc/CBACKEND_REVIVAL_AND_WAVES.md` 完成 Wave 0-7
复活与覆盖扩展；RegC/SSA/R3 已完成 MVP 闭环，下一步进入输入面加固、runtime ABI
落地和 mjdbg 反馈闭环。
当前进度：

| 波次 | 内容 | 状态 |
|------|------|------|
| **R0** | 架构重构设计落地（6 份文档） | ✅ 完成 |
| **R1** | L2 SSA + L3 寄存器 IR 骨架 | ✅ 完成 |
| **R2** | L5 RegC 寄存器式 C 生成 + E2E | ✅ MVP 完成 |
| **R3** | RT 运行时最小可用 | ✅ core 完成，待 RegC ABI/live 验证 |
| **R4** | L4 变换 pass 系统 | ⬜ 待开始 |
| **R5+** | mjdbg 闭环 + 可选 VMC4 | ⬜ 待 RegC/R3 样本反馈 |

权威定义见 `doc/ROADMAP.md`。当前下一步不是重做 R1/R2，而是按
`doc/REGC_NEXT_STEPS.md` 做善后与加固：先修正文档/注释漂移，再补 SSA/RegC
常见 javac 输入形状，然后把 inline RegC helper 逐步迁到 R3 runtime ABI。

### 0.1 已完成加固包（feat/mj2x/regc-followup-foundation）

该分支已通过 PR #396 合并到 `dev.3`，用于记录 RegC 输入面基础加固结果。

| 项目 | 状态 | 验收 |
|------|------|------|
| 文档善后：修正 R1/R2/R3 和 try/catch 过时状态 | ✅ 完成 | `REGC_NEXT_STEPS.md`、本文、相关设计文档同步 |
| 公开注释补齐：RegCBackend/RegCEmitter 与 multianewarray 边界 | ✅ 完成 | Doxygen 注释与当前 helper-backed 实现一致 |
| 后续计划：明确 Phase 1-4 顺序 | ✅ 完成 | `doc/REGC_FOLLOWUP_PLAN.md` |
| javac 窄化转换：`i2b`/`i2s`/`i2c` | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| javac boolean ternary StackMap 形状 | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| null field/call/array protected handler 形状 | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| 最终验证与提交整理 | ✅ 完成 | 提交信息使用 `[mj2x] 中文描述`，PR 标题同规范 |

### 0.2 当前总线开发清单（feat/mj2x/regc-r3-runtime-abi）

本轮以“阶段分支逐步开发，最终整理一个总 PR 到 `dev.3`”为交付方式。每个阶段完成后
在本清单打勾；提交和 PR 标题使用 `[mj2x] 中文描述`，并遵循 `spec/git-workflow-convention.md`。

| 阶段 | 状态 | 验收 |
|------|------|------|
| Phase A：R3 Runtime ABI 设计落地 | ✅ 完成 | `doc/REGC_R3_RUNTIME_ABI.md` + `runtime/src/regc/regc_runtime_abi.hpp` Doxygen 明确上下文/符号边界 |
| Phase B：RegC 生成 runtime-backed helper 调用 | ✅ 完成 | div/rem/barrier 走 R3 ABI；生成 C 仍无 `cstack`/`sp` |
| Phase C：RegC/R3 native E2E | ✅ 完成 | `RegCBackendE2E.IntegerDivisionTrapRoundTrip` 链接 runtime ABI，Java baseline 与 protected DLL 一致 |
| Phase D：RegC/R3 protected samples | ✅ 完成 | `RegCRuntimeSurfaceSample` 产出 mjdbg 可消费样本，文档说明 register/runtime trace surface |
| Phase E：invokedynamic 策略预案 | ✅ 完成 | `doc/REGC_INVOKEDYNAMIC_STRATEGY.md` 明确 fail-closed 边界和 string concat/lambda 展开路线 |
| Phase F：最终验证与提交整理 | ✅ 完成 | `mj2x-core`/`mj2x-cli`/`mj2x_unittest` 构建与目标测试通过，提交历史中文规范 |

### 0.3 MVP 发版收口清单（当前）

本轮目标是把 mj2x 从“功能已能跑”收口到“内部 MVP 可发版”：必须有 backend 级 README
说明支持矩阵，必须能说明每项能力对应哪些样本/测试验证，并补上急需的 JAR 级输入/输出。

| 阶段 | 状态 | 验收 |
|------|------|------|
| Phase A：backend README 能力矩阵 | ✅ 完成 | `doc/C_BACKEND_README.md` / `doc/REGC_BACKEND_README.md` 写清支持特性、限制、CLI 用法、验证样本 |
| Phase B：样本验证索引 | ✅ 完成 | `doc/MVP_BACKEND_VERIFICATION.md` 能从每项能力追到 E2E/单测/protected sample |
| Phase C：JAR 输入/输出 MVP | ✅ 完成 | `mj2x-cli -i in.jar -o out.jar --emit-c out.c` 能处理 jar 内多 class 并保留非 class 资源 |
| Phase D：JAR Pipeline/CLI 测试 | ✅ 完成 | 单测构造 jar，验证 class 修改、资源 passthrough、输出 jar 可重新读取 |
| Phase E：最终验证与提交整理 | ✅ 完成 | `mj2x-core`/`mj2x-cli`/`mj2x_unittest` 构建与目标测试通过，中文规范提交 |

### 0.4 MVP 功能冻结（当前）

2026-07-06 冻结 mj2x 0.1 内部 MVP 范围，权威文档为 `doc/MVP_RELEASE_SCOPE.md`。
后续到 MVP 发版前不再扩新后端能力，只允许在冻结范围内做可靠性加强、测试修复和文档清理。

| 项目 | 状态 | 说明 |
|------|------|------|
| 冻结输入/输出范围 | ✅ 完成 | `.class` / `.jar` 输入输出，CBackend / RegC CLI 入口 |
| 冻结后端范围 | ✅ 完成 | CBackend 广覆盖正确性；RegC 保护导向 MVP；R3 runtime 仅冻结当前 div/rem/barrier ABI |
| 冻结延期项 | ✅ 完成 | R4 pass、VMC4、signed JAR 重签、完整 invokedynamic、JNI ID cache 等均 MVP 后处理 |
| 发版前补强范围 | ✅ 完成 | JAR fixture、签名 metadata 处理、CLI negative tests、unsupported fail-closed tests、旧文档清理 |

### 0.5 MVP 冻结后加固清单（feat/mj2x/mvp-hardening-tests）

本阶段只在 `doc/MVP_RELEASE_SCOPE.md` 冻结范围内做测试、修补和功能补全，不新增 R4/VMC4
等后续能力。每个阶段完成后按中文规范提交，最终开 PR 到 `dev.3`。

| 阶段 | 状态 | 验收 |
|------|------|------|
| Phase A：JAR fixture 加固 | ✅ 完成 | committed Java/资源样本覆盖 manifest、多 class、资源 passthrough、多 release class passthrough |
| Phase B：signed JAR metadata 边界 | ✅ 完成 | `META-INF/*.SF`、`*.RSA`、`*.DSA`、`*.EC` 触发 fail-closed，并有 InputLoader/CLI 测试 |
| Phase C：CLI negative tests | ✅ 完成 | 坏参数、未知 backend、缺 input、空 JAR、无 class JAR等路径有自动化覆盖 |
| Phase D：unsupported fail-closed tests | ✅ 完成 | `invokedynamic` 既有覆盖；JSR/RET、RegC unsupported lowering 不静默生成错误 native |
| Phase E：release gate 复跑 | ✅ 完成 | `mj2x-core`/`mj2x-cli`/`mj2x_unittest`/`mj2x_runtime_test` 构建与完整测试通过 |

---

## 1. 项目是什么

mj2x (Morpher Java Bytecode to X) 是一个 JVM 字节码 AOT 编译器，将 `.class` 文件中的 Java 方法翻译为 C 源码（未来可扩展到 LLVM IR 等后端）。通过 JNI 实现 Java 运行时互操作。

定位：代码混淆/硬化工具链的一部分，与 morpher 生态集成。

参考项目：[masxinlingvonta](https://github.com/superblaubeere27/masxinlingvonta)（Java→LLVM IR，单后端）

---

## 2. CBackend MVP 实现状态

### 2.1 已实现 ✅

| 模块 | 功能 | 文件 |
|------|------|------|
| Pipeline | Load → Preprocess → Convert(Core/MIR) → Backend(CBackend) → Postprocess → Write | `src/mj2x/core/pipeline.{hpp,cpp}` |
| CompilerIndex | 类/方法/字段索引 | `src/mj2x/core/compiler_index.{hpp,cpp}` |
| InputLoader | `.class` / `.jar` 文件读取（libjbytecode + libjzip） | `src/mj2x/io/input_loader.{hpp,cpp}` |
| OutputWriter | 写回修改后的 `.class` / `.jar` | `src/mj2x/io/output_writer.{hpp,cpp}` |
| MIR | 覆盖算术/控制流/调用/对象/数组/异常的后端无关 MIR 节点 | `src/mj2x/frontend/mir_node.hpp` |
| Block | 基本块 + CFG + 异常处理器 | `src/mj2x/frontend/block.hpp` |
| CodeConverter | 字节码 → MIR 转换 | `src/mj2x/frontend/code_converter.{hpp,cpp}` |
| MethodStack | 参数/局部变量类型推导 | `src/mj2x/frontend/method_stack.{hpp,cpp}` |
| OpcodeUtils | opcode → 枚举映射 | `src/mj2x/frontend/opcode_utils.{hpp,cpp}` |
| TypeUtils | JVM 类型描述符工具 | `src/mj2x/frontend/type_utils.{hpp,cpp}` |
| Backend 接口 | compileMethod / injectIntoClass / finalize | `src/mj2x/backend/backend.hpp` |
| CBackend | MIR → JNI C 代码生成（stack/sp 模型） | `src/mj2x/backend/c_backend.{hpp,cpp}` |
| Preprocessor 接口 | 可插拔 | `src/mj2x/preprocessor/preprocessor.hpp` |
| MarkAllMethods | MVP 预处理器：全部方法标记编译 | `src/mj2x/preprocessor/mark_all_preprocessor.hpp` |
| Postprocessor 接口 | 可插拔 | `src/mj2x/postprocessor/postprocessor.hpp` |
| CLI | `mj2x-cli -i in.class -o out.class --emit-c out.c` | `src/mj2x-cli/main.cpp` |

### 2.2 单元测试 ✅

当前测试覆盖 CompilerIndex / IO / Pipeline / TypeUtils / OpcodeUtils / MethodStack / CodeConverter /
CBackend 子串测试 / CBackend 真 E2E。CBackend 真 E2E 现有 25 个用例，覆盖 Wave 1-7 与综合样本。

### 2.3 已知的限制（CBackend）

| 限制 | 说明 |
|------|------|
| JAR 输出仍是 MVP | 保留 entry 顺序和非 class 资源，输出 stored ZIP；签名 JAR 改写后不保证原签名有效 |
| JNI ID 无缓存 | 每次调用都 GetMethodID |
| 栈全用 union jvalue 数组 | 性能差，后续可优化 |
| `INVOKEDYNAMIC` fail-closed | 生成 `UnsupportedOperationException`，不直译 lambda/方法句柄 |
| JSR/RET 不纳入 | 废弃字节码由 preprocessor/输入规范处理 |
| int/long DIV/REM 除零未显式转异常 | C 层仍不是完整 JVM trap 模型 |

---

## 3. 构建与运行

```bash
# 构建
xmake build mj2x-core          # 核心静态库
xmake build mj2x-cli           # CLI 工具
xmake build mj2x_unittest      # 单元测试

# 运行
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c --dump-mir  # MIR 调试
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter="CBackend*"  # 过滤
```

测试真实混淆 class：
```bash
jar xf ref_samples/SnakeGame-obf-grunt.jar b.class
xmake run mj2x-cli -i b.class -o b_out.class --emit-c b_out.c
```

---

## 4. CBackend 关键设计决策

- **MIR 层**：后端无关中间表示，所有 34 种节点定义在 `mir_node.hpp`，`CodeConverter` 将字节码指令翻译为 MIR，`CBackend` 将 MIR 翻译为 C 代码。多后端分离的关键抽象。
- **Stack + SP 模型**：C 中使用 `union jvalue stack[]` + `int sp` 模拟 JVM 操作数栈，实现简单但性能低于局部变量优化方案。
- **全 JNI 互操作**：字段读写、方法调用、对象创建全部走 JNI C API（`(*env)->Call*Method` 等），不需要运行时环境。
- **无 RTTI**：遵循 morpher 项目约定，使用 `typed::cast<T>`（声明在 `libtyped`）替代 `dynamic_cast`。
- **所有 MIR 节点用 `unique_ptr` 管理**：存储在 `Block::nodes` 的 `vector` 中，`CodeConverter` 必须比 `Block*` 活得更久。

文件树与依赖关系详见 `doc/ARCHITECTURE.md`。

---

## 5. 已知问题

### 5.1 加载失败（4 个混淆 class）

`ref_samples/SnakeGame-obf-grunt.jar` 中 4/14 的 class 加载时抛 `readInt exceeds buffer`（a/h/k/n.class）。调试方法：在 `libjbytecode` 的 `readInt` 函数中添加 `printf` 输出调用栈或文件名+行号，重新运行确定具体溢出位置。

### 5.2 libjbytecode ClassReader 已知问题

- [#227](https://github.com/luiox/morpher/issues/227) ✅ 已修复 (PR #228)：thisClass/superClass 越界校验
- [#229](https://github.com/luiox/morpher/issues/229) ✅ 已修复 (PR #230)：属性长度截断守卫

### 5.3 CBackend 生成代码限制

> **2026-07-05 更新**（Wave 0-7 复活）：整数/长整型/浮点、转换、控制流、调用、
> 对象/字段、数组、`ATHROW`/try-catch/finally、monitor 块均已有真 E2E 覆盖。下表为**当前**
> 剩余限制。

| 问题 | 影响 | 优先级 | 处置波次 |
|------|------|--------|---------|
| JNI field/method ID 未缓存（每次现查 GetMethodID/GetFieldID） | 性能差 | 中 | 后续优化 |
| JAR 输出仍是 MVP | entry 顺序和资源保留，签名/重压缩/manifest 产品体验未完整化 | 高 | 当前 MVP 已接入，后续产品化 |
| `<init>`/`<clinit>` 不被 MarkAll 标记（保留原字节码） | 构造器不走 native | 中 | Wave 4 完整支持时重评 |
| int DIV/REM 除零：JVM 抛 ArithmeticException，C 是 UB | 极端输入行为差异 | 低 | 接受（Wave 1 限制） |
| `INVOKEDYNAMIC` 仅 fail-closed | lambda/字符串拼接等 indy 形态不直译 | 中 | 后续 preprocessor/RegC |
| LDC Class/MethodType/Handle 仍非完整对象语义 | 反射/方法句柄场景受限 | 中 | 后续 preprocessor |
| 生成的 `stack` 类型声明 `union jvalue` 被传入函数形参时有潜在兼容问题 | MSVC 兼容 | 低 | — |

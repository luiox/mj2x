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

### 0.1 当前分支交付清单（feat/mj2x/regc-followup-foundation）

本分支只做 RegC 后续基础包，不把 R3 runtime ABI 迁移和 R4 pass 系统混入同一个 PR。
完成后再开 PR 到 `dev.3`。

| 项目 | 状态 | 验收 |
|------|------|------|
| 文档善后：修正 R1/R2/R3 和 try/catch 过时状态 | ✅ 完成 | `REGC_NEXT_STEPS.md`、本文、相关设计文档同步 |
| 公开注释补齐：RegCBackend/RegCEmitter 与 multianewarray 边界 | ✅ 完成 | Doxygen 注释与当前 helper-backed 实现一致 |
| 后续计划：明确 Phase 1-4 顺序 | ✅ 完成 | `doc/REGC_FOLLOWUP_PLAN.md` |
| javac 窄化转换：`i2b`/`i2s`/`i2c` | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| javac boolean ternary StackMap 形状 | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| null field/call/array protected handler 形状 | ✅ 完成 | SSA Java sample + RegC DLL E2E |
| 最终验证与提交整理 | ✅ 完成 | 提交信息使用 `[mj2x] 中文描述`，PR 标题同规范 |

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
| InputLoader | 单 `.class` 文件读取（libjbytecode） | `src/mj2x/io/input_loader.{hpp,cpp}` |
| OutputWriter | 写回修改后的 `.class` | `src/mj2x/io/output_writer.{hpp,cpp}` |
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
| 只支持单 `.class` 输入 | 无 JAR 解析 |
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
| JAR 输入/输出不支持 | 只能处理单 class 文件 | 高 | 后续独立波次 |
| `<init>`/`<clinit>` 不被 MarkAll 标记（保留原字节码） | 构造器不走 native | 中 | Wave 4 完整支持时重评 |
| int DIV/REM 除零：JVM 抛 ArithmeticException，C 是 UB | 极端输入行为差异 | 低 | 接受（Wave 1 限制） |
| `INVOKEDYNAMIC` 仅 fail-closed | lambda/字符串拼接等 indy 形态不直译 | 中 | 后续 preprocessor/RegC |
| LDC Class/MethodType/Handle 仍非完整对象语义 | 反射/方法句柄场景受限 | 中 | 后续 preprocessor |
| 生成的 `stack` 类型声明 `union jvalue` 被传入函数形参时有潜在兼容问题 | MSVC 兼容 | 低 | — |

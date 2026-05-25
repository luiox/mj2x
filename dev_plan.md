# mj2x — MVP 开发现状与接手指南

> 最后更新: 2026-05-25
> 分支: `feat/mj2x-mvp`（基于 `main`，已 rebase 到最新）

---

## 1. 项目是什么

mj2x (Morpher Java Bytecode to X) 是一个 JVM 字节码 AOT 编译器，将 `.class` 文件中的 Java 方法翻译为 C 源码（未来可扩展到 LLVM IR 等后端）。通过 JNI 实现 Java 运行时互操作。

定位：代码混淆/硬化工具链的一部分，与 morpher-native 生态集成。

参考项目：[masxinlingvonta](https://github.com/superblaubeere27/masxinlingvonta)（Java→LLVM IR，单后端）

---

## 2. MVP 完成状态

### 2.1 已实现 ✅

| 模块 | 功能 | 文件 |
|------|------|------|
| Pipeline | Load → Preprocess → Convert(Core/MIR) → Backend(CBackend) → Postprocess → Write | `src/mj2x/core/pipeline.{hpp,cpp}` |
| CompilerIndex | 类/方法/字段索引 | `src/mj2x/core/compiler_index.{hpp,cpp}` |
| InputLoader | 单 `.class` 文件读取（libjbytecode） | `src/mj2x/io/input_loader.{hpp,cpp}` |
| OutputWriter | 写回修改后的 `.class` | `src/mj2x/io/output_writer.{hpp,cpp}` |
| MIR | 34 种后端无关中间表示节点 | `src/mj2x/frontend/mir_node.hpp` |
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

### 2.2 单元测试 ✅ 39 passing

| 测试套件 | 用例数 | 覆盖 |
|---------|--------|------|
| CompilerIndexTest | 2 | 增删查 |
| IOTest | 2 | ClassReader/Writer roundtrip |
| InputLoaderTest | 1 | 真实 class 读入 |
| PipelineTest | 2 | 非法路径 / roundtrip |
| TypeUtilsTest | 3 | JVM 类型映射 |
| OpcodeUtilsTest | 3 | opcode 到枚举映射 |
| MethodStackTest | 3 | 参数类型推导 |
| CodeConverterTest | 12 | 指令翻译 / CFG / dump |
| CBackendTest | 9 | 各 MIR 节点 C 翻译 |
| E2ETest | 2 | 完整 Pipeline + C 输出 |

### 2.3 已知的限制

| 限制 | 说明 | Issue |
|------|------|-------|
| 只支持单 `.class` 输入 | 无 JAR 解析 | — |
| `synchronized` 块抛异常 → 锁泄漏 | `MONITOREXIT` 跳过 | 文档标注 |
| `ATHROW` 不中断 C 控制流 | 需手动 `return` | 文档标注 |
| `ACC_SYNCHRONIZED` 方法拒绝 | C 不能模拟 JVM monitor | 文档标注 |
| JNI ID 无缓存 | 每次调用都 GetMethodID | Phase 5 计划 |
| 栈全用 union jvalue 数组 | 性能差，后续可优化 | Phase 5 计划 |
| 不支持 `INVOKEDYNAMIC` | 需 preprocessor 提取 | Phase 5 计划 |

---

## 3. 构建与运行

### 3.1 前置条件

- Windows x64（其他平台未测试）
- MSVC 2022（Visual Studio 2022）
- xmake >= 2.9.3
- JDK 17+（仅编译测试用）

### 3.2 构建

```bash
# 从 morpher 根目录运行
xmake build mj2x-core          # 核心静态库
xmake build mj2x-cli           # CLI 工具
xmake build mj2x_unittest      # 单元测试
```

### 3.3 运行

```bash
# 处理单个 class 文件
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c

# 带 MIR 调试输出
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c --dump-mir

# 运行测试
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter="CBackend*"  # 过滤
```

### 3.4 测试真实混淆 class

```bash
# 从 SnakeGame 样本提取 class
jar xf ref_samples/SnakeGame-obf-grunt.jar b.class

# 处理
xmake run mj2x-cli -i b.class -o b_out.class --emit-c b_out.c

# 验证输出
cat b_out.c | head -30
```

---

## 4. 架构速览

### 4.1 Pipeline 流程

```
input.class
    │
    ▼
[InputLoader]  →  libjbytecode::ClassReader → ClassNode
    │
    ▼
[CompilerIndex]  →  classes / methods / fields 索引
    │
    ▼
[Preprocessors]  →  标记待编译方法、提取复杂指令
    │
    ▼
[CodeConverter]  →  遍历 InsnList → 分析 CFG → 构建 Block[] + MIRNode[]
    │
    ▼
[Backend]        →  MIR → 目标代码（CBackend 生成 JNI C 函数）
    │
    ▼
[Postprocessors] →  设 ACC_NATIVE、注入加载器
    │
    ▼
[OutputWriter]   →  libjbytecode::ClassWriter → output.class + output.c
```

### 4.2 关键设计决策

- **MIR 层**: 后端无关中间表示，所有 34 种节点定义在 `mir_node.hpp`，`CodeConverter` 将字节码指令翻译为 MIR，`CBackend` 将 MIR 翻译为 C 代码。多后端分离的关键抽象。
- **Stack + SP 模型**: C 中使用 `union jvalue stack[]` + `int sp` 模拟 JVM 操作数栈，实现简单但性能低于局部变量优化方案。
- **全 JNI 互操作**: 字段读写、方法调用、对象创建全部走 JNI C API（`(*env)->Call*Method` 等），不需要运行时环境。
- **无 RTTI**: 遵循 morpher-native 项目约定，使用 `typed::cast<T>`（声明在 `libtyped`）替代 `dynamic_cast`。
- **所有 MIR 节点用 `unique_ptr` 管理**: 存储在 `Block::nodes` 的 `vector` 中，`CodeConverter` 必须比 `Block*` 活得更久。

### 4.3 文件树

```
src/mj2x/
├── mj2x.hpp                        # 聚合头文件
├── core/
│   ├── pipeline.{hpp,cpp}          # Pipeline 编排
│   └── compiler_index.{hpp,cpp}    # CompilerIndex
├── frontend/
│   ├── mir_node.hpp                # 34 种 MIR 节点定义
│   ├── block.hpp                   # Block + ExceptionHandler
│   ├── code_converter.{hpp,cpp}    # 字节码 → MIR 转换
│   ├── method_stack.{hpp,cpp}      # 局部变量类型推导
│   ├── type_utils.{hpp,cpp}        # JVM 类型工具
│   └── opcode_utils.{hpp,cpp}      # Opcode 映射
├── backend/
│   ├── backend_result.hpp          # 后端结果结构体
│   ├── backend.hpp                 # Backend 抽象接口
│   └── c_backend.{hpp,cpp}         # C 源码后端
├── preprocessor/
│   ├── preprocessor.hpp            # Preprocessor 接口
│   └── mark_all_preprocessor.hpp   # 全部标记预处理器
├── postprocessor/
│   └── postprocessor.hpp           # Postprocessor 接口
├── io/
│   ├── input_loader.{hpp,cpp}      # .class 文件读取
│   └── output_writer.{hpp,cpp}     # .class 文件写入
src/mj2x-cli/
│   └── main.cpp                    # CLI 入口
src/unittest/
├── main.cpp                        # gmock main
├── core/
│   ├── compiler_index_test.cpp
│   ├── io_test.cpp
│   ├── e2e_test.cpp
│   └── input_loader_test.cpp
├── frontend/
│   └── code_converter_test.cpp
└── backend/
    └── c_backend_test.cpp
```

---

## 5. 依赖关系

```
mj2x-cli ──▶ mj2x-core (static lib)
                   │
                   ├── libjbytecode (class 读写)
                   │     ├── libca_core (基础类型 u8/i32)
                   │     ├── libtyped (RTTI 替代)
                   │     └── libcrypto (SHA-1)
                   │
                   └── libjstr (字符串工具)
mj2x_unittest ──▶ mj2x-core + gmock
```

所有依赖均为 morpher-native 内部库，无新增第三方依赖。

---

## 6. 已知问题与待修复

### 6.1 加载失败（4 个混淆 class）

`ref_samples/SnakeGame-obf-grunt.jar` 中 4/14 的 class 加载时抛 `readInt exceeds buffer`：

| Class | 大小 | 错误位置 | 可能原因 |
|-------|------|---------|---------|
| a.class | 9800 | readInt at 9800 | 深层 attribute 解析溢出（PR #228/#230 未覆盖的更底层读越界） |
| h.class | 4827 | readInt at 4824 | 同上 |
| k.class | 1619 | readInt at 1616 | 同上 |
| n.class | 56264 | readInt at 56264 | 同上 |

**建议调试方法**：在 `libjbytecode` 的 `readInt` 函数中添加 `printf` 输出调用栈或文件名+行号，重新运行确定具体溢出位置。

### 6.2 libjbytecode ClassReader 已知问题

| Issue | 状态 | 说明 |
|-------|------|------|
| [#227](https://github.com/luiox/morpher/issues/227) | ✅ 已修复 (PR #228) | thisClass/superClass 越界校验 |
| [#229](https://github.com/luiox/morpher/issues/229) | ✅ 已修复 (PR #230) | 属性长度截断守卫 |

### 6.3 CBackend 生成代码限制

| 问题 | 影响 | 优先级 |
|------|------|--------|
| JNI field/method ID 未缓存 | 每次访问都查 ID，性能差 | 中 |
| `ATHROW` 后无 `ExceptionCheck` 桩 | MVP 文档接受此限制 | 低 |
| `MONITORENTER/EXIT` 块内异常 → 锁泄漏 | MVP 文档接受此限制 | 低 |
| JAR 输入/输出不支持 | 只能处理单 class 文件 | 高 |
| 缺失 `ConstantInstruction` 适配器的 MIRConstant 节点 | LDC 指令未正确翻译常量值 | 低 |
| 生成的 `stack` 类型声明 `union jvalue` 被传入函数形参时有潜在兼容问题 | MSVC 兼容 | 低 |

---

## 7. 下一步开发优先级

### P0（必须先修）

- [ ] **JAR 输入输出支持**：当前 InputLoader 只读单 `.class`，OutputWriter 只写单 `.class`。需要基于 `libjzip` 或 `morpher-nbase` 的 jar 工具实现多文件 JAR 读写。
- [ ] **修复 4 个混淆 class 加载失败**：确定 `readInt exceeds buffer` 的具体溢出位置并修复。

### P1（核心增强）

- [ ] **JNI ID 缓存**：在生成的 C 代码中用静态变量缓存 `jmethodID` / `jfieldID`，在 `JNI_OnLoad` 或首次使用时初始化。
- [ ] **INVOKEDYNAMIC 提取 Preprocessor**：参考 masxinlingvonta 的 `InstructionExtractor`，把 `invokedynamic` 指令提取到合成方法。
- [ ] **`ConstantInstruction` 类型完善**：让 MIRConstant 正确区分 int/long/float/double/string 常量值。

### P2（质量提升）

- [ ] **C 编译验证 E2E 测试**：用 javac 编译一个测试 Java 类，mj2x 翻译成 C，再用 MSVC `cl /c` 验证生成的 C 可编译。
- [ ] **行覆盖测试**：补充 Phase 1-4 未覆盖的组件测试（Block、MethodStack、Preprocessor 等），目标 ≥ 80 测试用例。
- [ ] **README 文档**：构建说明、使用示例、架构概览。

---

## 8. 如何调试

### 8.1 MIR 调试

```bash
xmake run mj2x-cli -i input.class -o out.class --emit-c out.c --dump-mir
```

`--dump-mir` 打印每个方法翻译后的 MIR 基本块和指令序列。

### 8.2 libjbytecode ClassReader 调试

如果某个 class 文件加载失败，可以：

```bash
# 快速验证
xmake run libjbytecode_unittest --gtest_filter="ClassReaderFix*"
```

要定位具体的 `readInt` 溢出位置，在 `class_reader.cpp` 的 `readInt` 函数中添加上下文信息：

```cpp
ca::i32 ClassReader::readInt(ca::i32 offset) const {
    if (offset < 0 || offset + 4 > static_cast<ca::i32>(len)) {
        printf("readInt at %d exceeds buffer (%d) | cpEnd=%d cpCount=%d\n",
               offset, len, cpEnd, cpCount);
        throw std::out_of_range(...);
    }
    ...
}
```

### 8.3 测试单个组件

```bash
xmake run mj2x_unittest --gtest_filter="CBackendTest.AddMethod"
xmake run mj2x_unittest --gtest_filter="CodeConverterTest.*"
```

---

## 9. 分支策略

```
main
  └── feat/mj2x-mvp        ← 当前开发分支（已 rebase 到最新 main）
  └── fix/libjbytecode-*    ← libjbytecode 修复分支（PR → main → rebase mvp）

feat/mj2x-mvp 的 7 个 commit:
  485c581f  feat(mj2x): Phase 4 - E2E integration
  6f0c72d9  feat(mj2x): Phase 3 - C backend
  a27eb360  feat(mj2x): Phase 2 - MIR + CodeConverter
  42acc564  feat(mj2x): Phase 1 - project skeleton
  511edbb7  docs(mj2x): address review - ATHROW/MONITOR semantics
  b3d71ad0  docs(mj2x): architecture, deps, MVP plan, test strategy
  5ee840d6  Merge PR #228 (ClassReader fix 1)
```

修复分支从 `main` 切出，PR 合并到 `main` 后，`feat/mj2x-mvp` rebase 到最新 `main` 以获取修复。

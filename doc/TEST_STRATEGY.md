# mj2x — Test Strategy

---

## 1. Test Levels

### 1.1 Unit Tests (GoogleMock)

覆盖所有核心组件，测试框架已就绪 (`mj2x_unittest`)。

| 组件 | 测试内容 | 预估用例数 |
|------|---------|-----------|
| `CompilerIndex` | addClass / getClass / index method/field | ~10 |
| `MIRNode` | 各子类构造/析构 | ~10 |
| `Block` | 节点追加/清空/CFG 连接 | ~10 |
| `CodeConverter` | 单条指令 → MIR (每个指令类型) | ~30 |
| `CodeConverter` | 方法 → Block[] 分解 | ~15 |
| `CodeConverter` | CFG 构建 (分支目标/异常处理) | ~10 |
| `MethodStack` | 参数类型推导 / local type | ~10 |
| `TypeUtils` | Sort → PrimitiveKind 映射 | ~10 |
| `OpcodeUtils` | opcode → BinaryOp/UnaryOp 映射 | ~10 |
| `Backend` (mock) | compile/inject/finalize 调用 | ~5 |
| `CBackend` | 每个 MIRKind → C 代码片段 | ~30 |
| `CBackend` | JNI 名称修饰 | ~10 |
| `CBackend` | 完整方法 → C 函数字符串 | ~10 |
| `InputLoader` | 读 class → CompilerIndex | ~5 |
| `OutputWriter` | ClassNode → byte[] roundtrip | ~5 |
| **总计** | | **~170** |

### 1.2 Integration Tests

测试组件间的交互。

| 测试 | 范围 | 方法 |
|------|------|------|
| Pipeline 空跑 | pipeline → preproc → convert → backend → postproc → write | mock backend 验证调用链 |
| CodeConverter → CBackend | MIR 完整翻译路径 | 真实 bytecode → C 代码 |
| InputLoader → Pipeline → OutputWriter | 完整读-改-写轮转 | .class roundtrip |

### 1.3 End-to-End Tests

| 测试 | 环境要求 | 验收标准 |
|------|---------|---------|
| 简单计算 | JDK + MSVC | Java 方法 → C 函数 → cl 编译通过 |
| 字段读写 | JDK + MSVC | 生成正确 JNI 调用 |
| 方法调用 | JDK + MSVC | 生成正确 JNI 调用 |
| 控制流 | JDK + MSVC | 分支/循环正确翻译 |

---

## 2. Test Organization

```
src/unittest/
├── main.cpp                                — gmock main
├── core/
│   ├── compiler_index_test.cpp             — CompilerIndex 测试
│   └── pipeline_test.cpp                   — Pipeline 测试
├── frontend/
│   ├── mir_node_test.cpp                   — MIR 节点测试
│   ├── block_test.cpp                      — Block 测试
│   ├── code_converter_test.cpp             — CodeConverter 测试
│   ├── method_stack_test.cpp               — MethodStack 测试
│   ├── type_utils_test.cpp                 — TypeUtils 测试
│   └── opcode_utils_test.cpp               — OpcodeUtils 测试
└── backend/
    ├── backend_test.cpp                    — Backend 接口 (mock)
    └── c_backend_test.cpp                  — CBackend 测试
```

---

## 3. 测试数据

### 3.1 内联字节码

大多数测试应使用 **内联构造的 bytecode 数组**，避免依赖外部文件：

```cpp
// 示例: 手动构造一个 ClassNode 用于测试
auto cn = std::make_unique<ClassNode>();
cn->version = V1_8;
cn->access = ACC_PUBLIC;
cn->name = "Test";
cn->superName = "java/lang/Object";

auto mn = std::make_unique<MethodNode>();
mn->access = ACC_PUBLIC | ACC_STATIC;
mn->name = "add";
mn->desc = "(II)I";
mn->maxStack = 2;
mn->maxLocals = 2;
mn->instructions.add(new VarInsnNode(ILOAD, 0));
mn->instructions.add(new VarInsnNode(ILOAD, 1));
mn->instructions.add(new InsnNode(IADD));
mn->instructions.add(new InsnNode(IRETURN));
```

### 3.2 真实 class 文件

少数 E2E 测试使用 `test_resource/` 下的真实 `.class` 文件（与 libjbytecode 共用资源目录）。

---

## 4. 测试运行

```bash
# 运行全部测试
xmake run mj2x_unittest

# gmock 过滤
xmake run mj2x_unittest --gtest_filter="CBackend*"

# 带输出
xmake run mj2x_unittest --gtest_print_time=1
```

---

## 5. 质量门槛

| 指标 | MVP 门槛 | 生产门槛 |
|------|---------|---------|
| 单元测试数 | ≥ 80 | ≥ 150 |
| 代码行覆盖率 | ≥ 60% | ≥ 80% |
| E2E 测试 | ≥ 3 | ≥ 10 |
| 编译警告 | 0 | 0 |
| CI 构建 | 必过 | 必过 |

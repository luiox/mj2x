# mj2x — Architecture Design

> Morpher Java Bytecode to X
> 多后端 JVM 字节码 AOT 编译器 / C 翻译器
> 设计版本: v0.1 (draft)

---

## 1. Overview

### 1.1 目标

将 JVM 字节码（.class）翻译为目标高级语言/IR，面向代码混淆和硬化场景。通过可插拔后端架构支持多种输出，MVP 优先实现 **C 源码后端**。

### 1.2 非目标

- 通用 Java AOT 编译器（不处理 GC、多线程等完整运行时）
- 字节码验证（由 libjbytecode analysis 层覆盖）
- 支持所有 JVM 指令的 100% 语义兼容（特定边界行为有差异，同 masxinlingvonta）
- JAR/ZIP 操作（由 morpher-nbase 的 jar 工具覆盖）

### 1.3 参考项目

- [masxinlingvonta](https://github.com/superblaubeere27/masxinlingvonta) — Java Bytecode → LLVM IR，单后端，全 JNI 策略
- libjbytecode — OW2 ASM 的 C++ 移植，class 读写基础

---

## 2. Architecture

### 2.1 Pipeline

```
          ┌──────────────────────────────────────────┐
          │               Pipeline                    │
          │                                           │
  .class ──▶  InputLoader                             │
  / .jar  │       │                                   │
          │       ▼                                   │
          │  CompilerIndex                            │
          │       │                                   │
          │       ▼                                   │
          │  Preprocessors (插拔式)                    │
          │       │                                   │
          │       ▼                                   │
          │  Bytecode → MIR (CodeConverter)           │
          │       │                                   │
          │       ▼                                   │
          │  Backend (插拔式)                          │
          │       │  MIR → C code                     │
          │       │  MIR → LLVM IR (future)           │
          │       │                                   │
          │       ▼                                   │
          │  Postprocessors (插拔式)                   │
          │       │                                   │
          │       ▼                                   │
          │  OutputWriter                             │
          │                                           │
          └─────────────────┬────────────────────────┘
                            │
              ┌─────────────┴─────────────┐
              │                           │
        Hybrid JAR                  .c / .ll / .o
        (native stubs)
```

### 2.2 阶段说明

| 阶段 | 输入 | 输出 | 职责 |
|------|------|------|------|
| **Load** | .class / .jar | `CompilerIndex` | 解析字节码，建类/方法/字段索引 |
| **Preprocess** | `CompilerIndex` | 修改后 `CompilerIndex` | 标记待编译方法、提取复杂指令、lambda 预编译 |
| **Convert** | `MethodNode` | `Block[]` + `MIRNode[]` | 字节码 → 后端无关中间表示 |
| **Backend** | `Block[]` | `BackendResult` | MIR → 目标代码（C/LLVM/...） |
| **Postprocess** | `CompilerIndex` + 结果 | 修改后 `CompilerIndex` | 设 ACC_NATIVE、注入加载器 |
| **Write** | `CompilerIndex` | JAR / .c / .dll | 序列化输出 |

### 2.3 核心数据流

```
.class bytes
    │
    ▼
libjbytecode::ClassReader
    │
    ▼
libjbytecode::ClassNode  (tree API)
    │
    ▼
CodeConverter::convert(MethodNode)
    │  ├── 遍历 InsnList
    │  ├── 分析控制流 (label targets)
    │  ├── 构建基本块
    │  └── 每条指令 → MIRNode
    │
    ▼
vector<Block*>
    │  └── Block::nodes: vector<unique_ptr<MIRNode>>
    │
    ▼
Backend::compileMethod(method, blocks)
    │  └── 遍历 blocks → 遍历 MIRNodes → 发射目标代码
    │
    ▼
BackendResult  { .text = "C source code", .kind = C_SOURCE }
```

---

## 3. Core Data Structures

### 3.1 CompilerIndex

```
CompilerIndex (全局索引)
  ├── classes_:  map<string owner, CompilerClass>
  ├── methodIndex_:  map<MethodID, CompilerMethod*>
  └── fieldIndex_:  map<FieldID, CompilerField*>

CompilerClass
  ├── classNode: ClassNode*         (libjbytecode tree)
  ├── methods: vector<CompilerMethod>
  ├── fields:  vector<CompilerField>
  ├── modified: bool
  └── isLibrary: bool

CompilerMethod
  ├── methodNode: MethodNode*       (libjbytecode tree)
  ├── markedForCompile: bool
  ├── compiled: bool
  └── backendResult: any

CompilerField
  └── fieldNode: FieldNode*

MethodID = { owner, name, desc }    // 三元组，可哈希
FieldID  = { owner, name, desc }
```

### 3.2 MIR (Morpher Intermediate Representation)

MIR 是后端无关指令树，定义在 `mir_node.hpp`。核心设计原则：

- **只含数据，不含代码生成逻辑** — 每个 `MIRNode` 子类仅携带操作数
- **后端按需遍历** — `Backend` 自行决定如何翻译每种节点
- **覆盖全部 JVM 指令** — 通过组合方式表达

```
MIRNode  (所有节点基类，含 MIRKind kind)
  │
  ├── MIRLabel          — 基本块标签
  ├── MIRConstant       — 常量推送 (int64/double/float/null)
  ├── MIRLocalLoad      — load 局部变量 (varIndex, type)
  ├── MIRLocalStore     — store 局部变量
  ├── MIRStackOp        — dup/swap 等栈操作
  ├── MIRBinaryOp       — 二元运算 (ADD/SUB/DIV...)
  ├── MIRUnaryOp        — 一元运算 (NEG)
  ├── MIRCast           — 类型转换
  ├── MIRIinc           — iinc
  ├── MIRBranch         — 条件跳转 (if_icmp* / if_acmp*)
  ├── MIRBranchZero     — 零值跳转 (ifeq/ifnull/iflt...)
  ├── MIRUncondBranch   — goto
  ├── MIRSwitch         — tableswitch / lookupswitch
  ├── MIRReturn         — void return
  ├── MIRReturnValue    — 有返回值 return
  ├── MIRNewObject      — new
  ├── MIRNewArray       — newarray / anewarray
  ├── MIRMultiNewArray  — multianewarray (preprocessor 提取)
  ├── MIRArrayLength    — arraylength
  ├── MIRArrayLoad      — *aload
  ├── MIRArrayStore     — *astore
  ├── MIRFieldGet       — getfield
  ├── MIRFieldPut       — putfield
  ├── MIRStaticGet      — getstatic
  ├── MIRStaticPut      — putstatic
  ├── MIRInvoke         — invokevirtual/special/static/interface
  ├── MIRInvokeDynamic  — invokedynamic (preprocessor 提取)
  ├── MIRCheckCast      — checkcast
  ├── MIRInstanceOf     — instanceof
  ├── MIRMonitor        — monitorenter/monitorexit
  ├── MIRThrow          — athrow
  └── MIRFrame          — stack map frame (元数据)
```

### 3.3 Block (基本块)

```
Block
  ├── id: int                — 对应 label bytecodeOffset
  ├── nodes: vector<unique_ptr<MIRNode>>
  ├── successors: vector<Block*>
  ├── predecessors: vector<Block*>
  └── exceptionHandler: ExceptionHandler*

ExceptionHandler
  ├── startLabel, endLabel: int
  ├── handlerLabel: int
  └── catchType: string       // 空 = finally
```

---

## 4. Backend Interface

```cpp
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string_view name() const = 0;

    // 编译方法：MIR blocks → 后端产物
    virtual BackendResult compileMethod(
        CompilerMethod& method,
        const std::vector<Block*>& blocks
    ) = 0;

    // 将编译产物注入 ClassNode（如修改方法体为 native 存根）
    virtual void injectIntoClass(
        CompilerClass& clazz,
        CompilerMethod& method,
        const BackendResult& result
    ) = 0;

    // 最终化（多方法编译后回调，如输出完整 .c 文件）
    virtual void finalize(const CompilerIndex& index) = 0;
};

struct BackendResult {
    enum class Kind { NONE, C_SOURCE, LLVM_IR, BINARY };
    Kind kind = Kind::NONE;
    std::vector<u8> data;        // 二进制数据
    std::string text;            // 文本输出 (C/LLVM IR)
    std::vector<std::string> metadata;  // 辅助数据（符号名等）
};
```

---

## 5. C Backend 详细设计

### 5.1 策略

**全 JNI 互操作**，同 masxinlingvonta 的基本思路：

| Java 操作 | C 实现 |
|-----------|--------|
| 字段读写 | `(*env)->Get*Field` / `Set*Field` |
| 方法调用 | `(*env)->Call*Method` |
| 对象创建 | `(*env)->NewObject` |
| 数组操作 | `(*env)->GetArrayLength`, `Get*ArrayElements`, `Set*ArrayRegion` |
| 类型检查 | `(*env)->IsInstanceOf`, `(*env)->CheckCast` |
| 异常 | `(*env)->ExceptionCheck`, `(*env)->Throw` |
| 同步 | `(*env)->MonitorEnter` / `MonitorExit` |

### 5.2 代码结构

每个编译方法生成一个 C 函数：

```c
// JNI 导出函数
JNIEXPORT <return_type> JNICALL
Java_<mangled_owner>_<mangled_method>(JNIEnv* env, jclass clazz, ...args) {
    // [可选] try-catch 框架
    // 局部变量声明（模拟 JVM 栈和 locals）
    union jvalue stack[<max_stack>];
    union jvalue locals[<max_locals>];
    
    // 初始化参数到 locals
    locals[0] = clazz;  // 或 this
    
    // 控制流：每个基本块用 label + goto
    // 块内顺序翻译 MIR 节点
    goto block_entry;
    
block_entry:
    // ... 翻译后的 C 代码 ...
    
block_1:
    // ... 翻译后的 C 代码 ...
    goto block_2;
    
block_2:
    // ... 翻译后的 C 代码 ...
    
    // JNI 局部引用释放
    return result;
}
```

### 5.3 虚拟栈实现

```
union jvalue {
    jint z; jbyte b; jchar c; jshort s;
    jint i; jlong j; jfloat f; jdouble d;
    jobject l;
};

union jvalue stack[MAX_STACK];  // 栈数组
int sp = -1;                     // 栈指针

// 翻译示例:
// IADD → stack[sp-1].i = stack[sp-1].i + stack[sp].i; sp--;
// GETFIELD → stack[++sp].l = (*env)->GetObjectField(env, stack[sp-1].l, fieldID);
```

**关键设计决策**：简化策略 vs 高效策略

| 策略 | 优点 | 缺点 |
|------|------|------|
| **stack 数组 + sp** | 实现简单，与 JVM 模型一致 | 每次访问都需类型 union 操作 |
| **C 局部变量** | 性能好，编译器优化空间大 | 需要分析每个变量的生存期 |
| **混合** | 常用值用局部变量，栈操作用数组 | 实现复杂，MVP 不应做 |

→ **MVP 选 stack 数组 + sp**，后续可优化。

### 5.4 控制流翻译

| MIRKind | C 翻译 |
|---------|--------|
| `UNCOND_BRANCH` → target | `goto label_<target>;` |
| `BRANCH` (eq/ne/lt/ge/gt/le) | `if (lhs op rhs) goto label_<target>;` |
| `BRANCH_ZERO` | `if (val == 0 / != 0 / < 0 / ...) goto label_<target>;` |
| `SWITCH` | `switch (key) { case v0: goto label_0; ... }` |
| `RETURN` | `return;` |
| `RETURN_VALUE` | `return stack[sp--].? ;` |

### 5.5 JNI 名称修饰

沿用 JDK `javah` 的 JNI 命名规范：

```
Java_<fully_qualified_class>_<methodName>__<signature_hash>
```

- 类名中 `.` → `_`
- 包分隔符 `/` → `_`
- 重载方法通过 `__` + 参数签名追加区分

### 5.6 输出格式

每个类输出一个 `.c` 文件，包含：

```c
#include <jni.h>

// 辅助宏 (可选，减少样板代码)
#define JNI_CALL_INT(env, obj, mid)  (*env)->CallIntMethod(env, obj, mid)
// ...

// 类初始化用的静态字段 ID / 方法 ID 缓存 (static variables)
static jmethodID mid_Example_compute = NULL;
static jfieldID fid_Example_value = NULL;

JNIEXPORT void JNICALL Java_Example_initIds(JNIEnv* env, jclass clazz) {
    // 缓存 MethodID / FieldID
}

// 每个编译方法一个函数
JNIEXPORT jint JNICALL
Java_com_example_Example_compute__III(JNIEnv* env, jobject this, jint a, jint b, jint c) {
    // ...
}
```

---

## 6. Module Layout

```
morpher-native/mj2x/
├── xmake.lua                    — 构建入口
├── doc/
│   ├── ARCHITECTURE.md          — 本文档
│   ├── DEPENDENCIES.md          — 依赖分析
│   ├── MVP_PLAN.md              — MVP 计划
│   └── TEST_STRATEGY.md         — 测试策略
├── src/
│   ├── mj2x/                    — 核心库
│   │   ├── mj2x.hpp             — 聚合头文件
│   │   ├── core/                — Pipeline 编排
│   │   ├── frontend/            — MIR / CodeConverter / Block
│   │   ├── backend/             — 后端接口
│   │   │   ├── backend.hpp
│   │   │   └── c_backend/       — C 源码后端
│   │   ├── preprocessor/        — 预处理器
│   │   ├── postprocessor/       — 后处理器
│   │   └── io/                  — 输入输出
│   ├── mj2x-cli/                — CLI 入口
│   └── unittest/                — 单元测试
│       ├── core/                — Pipeline 测试
│       ├── frontend/            — MIR / Block / CodeConverter 测试
│       └── backend/             — 后端测试
└── dev_plan.md                  — 宏观开发计划
```

---

## 7. Build Integration

```lua
-- morpher-native/mj2x/xmake.lua
target("mj2x-core")
    set_kind("static")
    add_deps("libjbytecode", "libjstr")
    add_files("src/mj2x/**.cpp")
    add_headerfiles("src/mj2x/**.hpp")
    add_includedirs("src", {public = true})
    -- /utf-8 on MSVC

target("mj2x-cli")
    set_kind("binary")
    add_deps("mj2x-core")
    add_files("src/mj2x-cli/*.cpp")

target("mj2x_unittest")
    set_kind("binary")
    add_deps("mj2x-core", "gmock")
    add_files("src/unittest/**_test.cpp")
    set_rundir("$(projectdir)")

-- morpher-native/xmake.lua 中添加
includes("mj2x")
```

### 7.1 依赖图

```
mj2x-cli ──▶ mj2x-core ──▶ libjbytecode ──▶ libca_core
                                      ├──▶ libtyped
                                      └──▶ libcrypto
                         ──▶ libjstr
mj2x_unittest ──▶ mj2x-core + gmock
```

---

## 8. JVM Interop 策略

### 8.1 C 后端 — JNI

所有 Java 互操作走 JNI C API：

| JNI 调用 | 频率 | 优化方向 |
|----------|------|---------|
| `FindClass` | 类首次引用 | 缓存到 static 变量 |
| `GetMethodID` / `GetFieldID` | 方法首次调用 | `JNI_OnLoad` 预缓存 |
| `Call*Method` | 每次调用 | — |
| `Get/Set*Field` | 每次访问 | — |
| `NewObject` | 每次创建 | — |
| `New*Array` | 每次创建 | — |

### 8.2 未来优化方向

- **Direct linking**: 对纯计算密集方法，通过 JNI `RegisterNatives` 直接注册函数指针
- **JNI call caching**: 批量获取 MethodID/FieldID

---

## 9. 与 masxinlingvonta 的关键差异

| 方面 | masxinlingvonta | mj2x |
|------|----------------|------|
| 语言 | Java 11 | C++17 |
| 字节码库 | OW2 ASM 9.1 | libjbytecode (C++, no RTTI) |
| 后端 | 单后端 LLVM | 多后端可插拔 |
| 中间表示 | 无 | MIR |
| MVP 后端 | LLVM | C 源码 |
| 测试 | 无 | GoogleMock |
| 构建 | Maven | Xmake |

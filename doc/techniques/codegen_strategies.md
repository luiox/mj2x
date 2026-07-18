# 原生代码生成策略

从编译器 IR 生成原生代码（通常是 C 源码或直接汇编）的通用策略。

## 问题

编译器 IR 是抽象的中间表示，需要 lower 到目标平台可执行的代码。对于 JVM → 原生编译，目标语言通常是 C（可移植性好、编译器优化成熟），特殊场景下可能直接生成汇编。

两个主要的代码生成模式：

| 模式 | 描述 | 适用场景 |
|---|---|---|
| 栈式代码生成 | 模拟 JVM 操作数栈，运行时维护 sp 指针 | 快速实现、全面覆盖 |
| 寄存器式代码生成 | 将 IR 变量映射到虚拟寄存器，经过分配后到物理寄存器 | 更高性能、更接近原生 |

## 栈式代码生成

### 模型

维护一个运行时栈（`jvalue[]` 或 `int64_t[]`），用 sp（stack pointer）指针操作：

```c
// 生成的 C 代码示例
int64_t stack[256];
int sp = -1;

// iconst_1
stack[++sp] = 1;

// iload v0
stack[++sp] = locals[0];

// iadd
int64_t b = stack[sp--];
int64_t a = stack[sp--];
stack[++sp] = a + b;

// istore v0
locals[0] = stack[sp--];
```

### 优劣

| 优势 | 劣势 |
|---|---|
| 实现简单，一个翻译器即可覆盖大部分字节码 | 大量冗余的 push/pop 操作 |
| 不需要寄存器分配 | C 编译器难以优化掉 sp 操作 |
| 异常恢复时栈状态清晰 | 每条指令额外增加栈指针偏移计算 |

### 栈复用优化

栈机代码的性能瓶颈在 sp 的重复读写。优化方式：

- **常量折叠** — 连续的 push + 运算直接求值，减少栈操作
- **窥孔优化** — 识别 push/pop 对并消除
- **本地变量提升** — 栈上的短期值直接映射到 C 本地变量

```c
// 优化前
stack[++sp] = a + b;
int result = stack[sp--];

// 优化后
int result = a + b;
```

## 寄存器式代码生成

### 虚拟寄存器

IR 中的每个 SSA 变量对应一个虚拟寄存器（VReg）。虚拟寄存器数量没有硬件限制：

```
SSA IR:
    v1 = const 1
    v2 = const 2
    v3 = add(v1, v2)
    store v3, locals[0]

→
    int r1 = 1;    // 虚拟寄存器
    int r2 = 2;
    int r3 = r1 + r2;
    locals[0] = r3;
```

### 寄存器分配

虚拟寄存器数量远多于物理寄存器，需要分配。三种策略：

| 策略 | 方法 | 质量 |
|---|---|---|
| 贪心 | 遇到变量就分配新寄存器，用完释放 | 最差 |
| 线性扫描 | 一次遍历，根据活跃区间分配 | 中等 |
| 图着色 | 构建干扰图，K 着色 | 最优 |

对于 JVM → 原生编译的典型 IR 规模（方法体通常小），线性扫描通常足够。

### 边复制

当通过 φ 节点的值所在的虚拟寄存器在不同基本块中不同时，需要在边上插入复制指令：

```c
// entry → L_true: r1 = 1
// entry → L_false: r2 = 2
// L_phi: r3 = φ(r1, r2)

// 在 entry → L_true 边上插入：
// r3_candidate = r1
// goto L_phi
// 在 entry → L_false 边上插入：
// r3_candidate = r2
// goto L_phi
// L_phi: r3 = r3_candidate
```

通过关键边分裂（Critical Edge Splitting）将复制放在新插入的基本块中。

## C 代码生成模式

### 表达式生成

IR 节点树 → C 表达式：

| IR 节点 | C 输出 |
|---|---|
| `const(42)` | `42` |
| `add(a, b)` | `(a + b)` |
| `call(method, args)` | `method_name(args)` |
| `load_field(obj, field)` | `obj->field` |
| `store_field(obj, field, val)` | `obj->field = val` |

### 控制流生成

| IR 节点 | C 输出 |
|---|---|
| `br(label)` | `goto label;` |
| `br_cond(label, cond)` | `if (cond) goto label;` |
| `switch(value, cases)` | `switch (value) { ... }` |
| `ret(value)` | `return value;` |

### ABI 集成

生成的 C 函数需要遵循目标语言的 ABI：

```c
// JNI ABI 示例
JNIEXPORT jint JNICALL Java_com_example_Foo_nativeMethod(
    JNIEnv* env, jobject this, jint a, jint b
) {
    // 生成的代码体
    return a + b;
}
```

ABI 集成包括：
- 参数映射（JNI 描述符 → C 函数签名）
- 返回值类型转换（JVM 类型 → C 类型）
- 异常处理（`ExceptionCheck` 插入）
- 线程局部存储（`JNIEnv` 指针传递）

## Lowering 策略

### 分阶段 Lowering

从 SSA IR 到下 C 代码的分阶段下降：

```
SSA IR → 寄存器 IR → C IR → C 源码
  (φ消除)  (寄存器分配)  (代码生成)
```

每阶段降低一层抽象：

1. **寄存器 IR** — φ 消除、虚拟寄存器分配、显式控制流
2. **C IR** — 平台无关的 C 操作节点（类型系统对应 C 类型）
3. **代码生成** — C IR → C 文本输出

### 指令选择

每个 IR 节点选择一个或多个 C 表达式/语句：

```c
Value* genAdd(AddNode* node, CodeGen& cg) {
    Value* lhs = cg.gen(node->lhs);
    Value* rhs = cg.gen(node->rhs);
    return cg.emit("%s + %s", lhs->cstr(), rhs->cstr());
}
```

## 失败封闭

对于无法生成原生代码的构造，策略是优雅降级而非崩溃：

```c
bool canGen(Insn* insn) {
    switch (insn->opcode) {
    case INVOKEDYNAMIC:
        return hasIntrinsic(insn) || canJniBridge(insn);
    case MULTIANEWARRAY:
        return false;  // 不支持多级数组
    // ...
    }
}

Value* gen(Insn* insn) {
    if (!canGen(insn)) failGracefully(insn);
    // 正常生成
}
```

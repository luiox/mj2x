# mj2x — MVP Plan

> MVP 目标：完整 Pipeline 跑通，能将一个简单 .class 方法翻译为可读 C 源码
> 预计工期：4-6 周 (单人)

---

## 1. MVP Scope

### 1.1 包含

- **功能**:
  - 输入：单个 .class 文件
  - 输出：单个 .c 文件 + 修改后的 .class
  - Pipeline 全流程：Load → Preprocess → Convert → Backend(C) → Postprocess → Write
  - 支持的字节码子集：常用指令 (见下方)
  - JNI 互操作：字段读写、方法调用、对象创建
  - 方法标记：CLI 参数或注解扫描指定编译哪些方法

- **质量**:
  - 单元测试覆盖核心组件
  - 端到端测试：编译一个简单 Java 方法，生成的 C 代码可被 MSVC 编译
  - CI 可构建

### 1.2 排除 (MVP 后)

- JAR/ZIP 多文件输入输出
- `INVOKEDYNAMIC` / lambda 处理
- `MULTIANEWARRAY`
- 异常处理 (try-catch)
- JSR/RET 子程序
- `DUP2_X1` / `DUP2_X2` (double/long 特殊栈操作)
- 栈帧计算 (COMPUTE_FRAMES)
- 性能优化
- 多后端 (LLVM 等)

---

## 2. 支持的字节码指令集 (MVP)

### 2.1 常量
`NOP`, `ACONST_NULL`, `ICONST_M1`–`ICONST_5`, `LCONST_0`–`DCONST_1`, `BIPUSH`, `SIPUSH`, `LDC` (int/long/float/double/string)

### 2.2 局部变量
`ILOAD`–`ALOAD`, `ISTORE`–`ASTORE`, `IINC`

### 2.3 数组
`IALOAD`–`SALOAD`, `IASTORE`–`SASTORE`, `ARRAYLENGTH`, `NEWARRAY`, `ANEWARRAY`

### 2.4 栈操作
`POP`, `POP2`, `DUP`, `DUP_X1`, `DUP2`, `SWAP`

### 2.5 数学/转换
`IADD`–`DREM`, `INEG`–`DNEG`, `ISHL`–`LXOR`, `I2L`–`D2I`, `I2B`–`I2S`, `LCMP`, `FCMPL`–`DCMPG`

### 2.6 控制流
`IFEQ`–`IFLE`, `IF_ICMPEQ`–`IF_ICMPLE`, `IF_ACMPEQ`/`IF_ACMPNE`, `IFNULL`/`IFNONNULL`, `GOTO`, `TABLESWITCH`, `LOOKUPSWITCH`

### 2.7 返回
`IRETURN`–`ARETURN`, `RETURN`

### 2.8 字段/方法
`GETSTATIC`, `PUTSTATIC`, `GETFIELD`, `PUTFIELD`, `INVOKEVIRTUAL`, `INVOKESPECIAL`, `INVOKESTATIC`, `INVOKEINTERFACE`

### 2.9 对象/类型
`NEW`, `CHECKCAST`, `INSTANCEOF`

### 2.10 其他
`MONITORENTER`, `MONITOREXIT`, `ATHROW`, `NEWARRAY`, `ANEWARRAY`, `ARRAYLENGTH`

---

## 3. Phase 分解

### Phase 1: 项目骨架 (1 周)

| 任务 | 产出 | 验收标准 |
|------|------|---------|
| 创建分支 `feat/mj2x-init` | 分支就绪 | git push 确认 |
| 设计文档 + 审阅 | ARCHITECTURE / DEPENDENCIES / MVP_PLAN / TEST_STRATEGY | 团队 review 通过 |
| xmake 构建配置 | `mj2x-core` static lib | `xmake build mj2x-core` 通过 |
| `CompilerIndex` + IO | 可读 class、建索引、写回 | 单 class 文件 roundtrip 成功 |
| Empty Pipeline | 空 Pipeline 跑通 | log 输出各阶段 |
| CLI 入口 | `mj2x-cli -i in.class -o out.class` | 命令行可用 |
| 测试框架 | `mj2x_unittest` | `xmake run mj2x_unittest` 通过 |

### Phase 2: MIR + CodeConverter (1.5 周)

| 任务 | 产出 | 验收标准 |
|------|------|---------|
| MIRNode 完整定义 | `mir_node.hpp` 全部 MIRKind | 编译通过 |
| CodeConverter 框架 | bytecode label targets → blocks | 测试覆盖 |
| 指令翻译: 常量/局部变量 | MIRConstant / MIRLocalLoad/Store | 测试覆盖 |
| 指令翻译: 数学/转换 | MIRBinaryOp / MIRCast | 测试覆盖 |
| 指令翻译: 栈操作 | MIRStackOp (dup/swap) | 测试覆盖 |
| 指令翻译: 控制流 | MIRBranch / MIRSwitch / MIRReturn | 测试覆盖 |
| 指令翻译: 字段/方法 | MIRFieldGet/Put / MIRInvoke | 测试覆盖 |
| 指令翻译: 对象/类型 | MIRNewObject / MIRCheckCast / MIRInstanceOf | 测试覆盖 |
| Block 构建 + CFG | 基本块分割，跳转边连接 | 测试覆盖 |
| MethodStack | 栈/局部变量类型推导 | 测试覆盖 |
| MIR 调试输出 | `dumpMIR()` 可读打印 | 目视可读 |

### Phase 3: C Backend (1.5 周)

| 任务 | 产出 | 验收标准 |
|------|------|---------|
| `Backend` 接口定型 | `backend.hpp` | 编译通过 |
| `CBackend` 实现框架 | `.c` 文件头部生成 (JNI include + 函数签名) | 生成合法 C |
| JNI 名称修饰 | `Mangle::mangleMethod()` | 测试覆盖 |
| JNI ID 缓存 | static variable 声明 + init 函数 | 正确性验证 |
| 常量/MIRLocal → C | stack 数组 + sp 操作 | 测试覆盖 |
| MIRBinaryOp/MIRCast → C | C 表达式生成 | 测试覆盖 |
| MIRFieldGet/Put → C | JNI `Get*Field` / `Set*Field` 调用 | 测试覆盖 |
| MIRInvoke → C | JNI `Call*Method` 调用 | 测试覆盖 |
| MIRBranch/MIRSwitch → C | `goto` / `if` / `switch` | 测试覆盖 |
| MIRReturn → C | `return` 语句 | 测试覆盖 |
| MIRNewObject → C | JNI `NewObject` 调用 | 测试覆盖 |
| `CBackend::injectIntoClass` | 设置 ACC_NATIVE，清空指令 | 验证 class 修改 |
| `CBackend::finalize` | 输出 `.c` 文件 | 文件生成 |

### Phase 4: 端到端验证 (1 周)

| 任务 | 产出 | 验收标准 |
|------|------|---------|
| Pipeline 集成 | 完整流程单命令跑通 | CLI 输入 class → 输出 .c + .class |
| 简单方法 E2E 测试 | 编译一个加法/字符串拼接方法 | 生成 C 代码可读、结构正确 |
| JNI C 编译验证 | 生成的 .c 文件被 MSVC 编译通过 | `cl /c output.c` 成功 |
| 单元测试完善 | 全部核心组件 > 80% 行覆盖 | `xmake run mj2x_unittest` |
| README / 使用文档 | 构建和使用说明 | 新的贡献者可依文档运行 |

---

## 4. 里程碑

| 里程碑 | 时间 | 验收 |
|--------|------|------|
| **M1: 骨架完成** | Phase 1 结束 | `xmake build mj2x-core && xmake run mj2x_unittest` |
| **M2: MIR 完成** | Phase 2 结束 | CodeConverter 可处理完整指令集，测试 > 50 个 |
| **M3: C 后端完成** | Phase 3 结束 | 简单 .class → 合法 .c 文件 |
| **M4: MVP 交付** | Phase 4 结束 | E2E 通过，CI 集成 |

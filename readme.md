# mj2x — Bytecode to Native Compiler

把 Java 字节码(`.class` / `.jar`)翻译为 C 源码,并将原方法标记为 `native`。
通过 JNI 实现 Java 运行时互操作。是 morpher 字节码工具平台的一条产品线,定位为代码混淆 / 硬化工具链。

参考项目:[masxinlingvonta](https://github.com/superblaabeere27/masxinlingvonta)(Java → LLVM IR,单后端)。

## 后端

| 后端 | 定位 | 输出风格 |
|---|---|---|
| **StackC** | 广覆盖正确性基线 | `union jvalue stack[]` + `sp` 操作(stack-style) |
| **RegC** | 保护导向,抗 c2j 分析 | 虚拟寄存器 + block label,无栈锚点 |

详细支持矩阵和限制见 [doc_ai/capabilities.md](doc_ai/capabilities.md)。

## 模块组成

- `mj2x-cli/` — 命令行入口、参数解析、native 编译器调用
- `mj2x-core/src/mj2x/core/` — Pipeline / CompilerIndex
- `mj2x-core/src/mj2x/io/` — InputLoader / OutputWriter(`.class` / `.jar`)
- `mj2x-core/src/mj2x/frontend/` — MIR / Block / CodeConverter / MethodStack
- `mj2x-core/src/mj2x/backend/` — StackCBackend / RegCBackend
- `mj2x-core/src/mj2x/preprocessor/` — 可插拔预处理器(LDC / invokedynamic / multianewarray / MarkAll / NativeAnnotation)
- `mj2x-annotations/` — 用户可引入的 `@mj2x.Native` 注解源码
- `mj2x-runtime/` — RegC R3 runtime(crypto / kernel / stringpool / antidebug / antijvmti)

## 构建

```bash
xmake f --with_mj2x=y
xmake build mj2x-cli
```

需要 JDK(`JAVA_HOME` 含 `jni.h`)。

## 文档

- **[doc/user_guide.md](doc/user_guide.md)** — 怎么用(命令、参数、示例、FAQ)
- **[doc_ai/capabilities.md](doc_ai/capabilities.md)** — 已实现功能清单 + MVP 边界
- [doc/roadmap.md](doc/roadmap.md) — R0-R5 路线图
- [doc/architecture_redesign.md](doc/architecture_redesign.md) — 架构权威设计
- [doc/stackc_backend_readme.md](doc/stackc_backend_readme.md) / [doc/regc_backend_readme.md](doc/regc_backend_readme.md) — 各后端支持矩阵
- [doc/mvp_release_scope.md](doc/mvp_release_scope.md) — MVP 功能冻结范围

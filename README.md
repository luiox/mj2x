# mj2x — Bytecode to Native Compiler

把 Java 字节码(`.class` / `.jar`)翻译为 C 源码,并将原方法标记为 `native`。
通过 JNI 实现 Java 运行时互操作。是 morpher 字节码工具平台的一条产品线,定位为代码混淆 / 硬化工具链。

参考项目:[masxinlingvonta](https://github.com/superblaabeere27/masxinlingvonta)(Java → LLVM IR,单后端)。

## 后端

| 后端 | 定位 | 输出风格 |
|---|---|---|
| **StackC** | 广覆盖正确性基线 | `union jvalue stack[]` + `sp` 操作(stack-style) |
| **RegC** | 高性能 native 生成器（默认零混淆；`--regc-obfuscate` 显式开启混淆，`--regc-*` 增强档见 [doc/user_guide.md](doc/user_guide.md)） | 虚拟寄存器 + block label,无栈锚点(默认走 JNI 调用保 JVM 栈帧语义,`--regc-direct-calls` 可开启直调) |
| **VMC4** | VM 虚拟化(保护导向,性能非关键) | 表驱动解释器,编译期随机化 + 执行期 dispatch drift(`--vmc4-drift`) |

## 用户自定义 intrinsic（#818）

三个后端（StackC / RegC / VMC4）都支持把任意 Java 静态调用点降级为对用户
native symbol 的直接调用（`--intrinsic-config <file>` + `--intrinsic-src <file>`）。
配置格式、ABI 契约与示例见 [doc/user_intrinsic.md](doc/user_intrinsic.md)。

## 模块组成

- `mj2x-cli/` — 命令行入口、参数解析、native 编译器调用(xmake / CMake / pipe)
- `mj2x-core/src/mj2x/core/` — Pipeline / CompilerIndex
- `mj2x-core/src/mj2x/io/` — InputLoader / OutputWriter(`.class` / `.jar`)
- `mj2x-core/src/mj2x/frontend/` — MIR / Block / CodeConverter / MethodStack
- `mj2x-core/src/mj2x/backend/` — StackCBackend / RegCBackend / Vmc4Backend / LlvmIrBackend(标量子集,默认开启;`with_mj2x_llvm=n` 可关闭)
- `mj2x-core/src/mj2x/preprocessor/` — 可插拔预处理器(LDC / invokedynamic / multianewarray / MarkAll / NativeAnnotation)
- `mj2x-annotations/` — 用户可引入的 `@mj2x.Native` 注解源码

- `mj2x-rt/` — C 运行时(regc/vmc4 生成工程内嵌源,含 safe-env、rt_protect 反侦测)
- `unittest/` — 单元测试 + E2E(backend / cli / codegen / core)
- `benchmark/` — 性能基准(jvm-obf-tester 13-probe vs os30,JNIC 对比)

## 构建

```bash
xmake f --with_mj2x=y            # 启用 mj2x(可选 --with_tests=y)
xmake build mj2x-cli             # CLI
xmake build mj2x_unittest        # 单元测试
```

需要 JDK(`JAVA_HOME` 含 `jni.h`)。`--with_tests=y` 时才拉取 gtest(见根目录
`doc/build-system.md`「gtest 的获取」)。

**单后端构建**(issue #886):默认全后端;`xmake f --with_mj2x=y --with_mj2x_backend=stackc|regc|vmc4|llvm`
只把所选后端编进可执行文件——被排除后端的源码不参与编译、CLI 对 `--backend <被排除>`
及被裁默认值 fail-closed(报可用清单)、mj2x-rt 嵌入按分发面收缩(stackc-only 产物不含
regc CRT 等;llvm 仍收编 regc intrinsic registry 供 JDK intrinsic 直调)。需要全量
mj2x-core 的测试 target(`mj2x_unittest`/`mj2x_integration_test`/`str_alloc_baseline`)
在单后端构建下自动禁用,全量回归用默认配置跑。

## 运行

```bash
# 单 class：翻译 + 生成 C（方法标 native）
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c

# 完整 jar：整体翻译 + 编译 DLL 并内嵌（运行命令不变）
mj2x-cli -i app.jar -o app_protected.jar --emit-c app.c --compile
java -cp app_protected.jar app.Main

# 测试
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1

# 界面语言:默认简体中文;--lang 或 MJ2X_LANG 切换(文案单源表
# mj2x-cli/translations/*.lang,构建期嵌入二进制,见 libca i18n 模块 libca_i18n)
mj2x-cli --lang en_US --help
```

详细用法见 [doc/user_guide.md](doc/user_guide.md)。

## 文档

### 使用

- **[doc/user_guide.md](doc/user_guide.md)** — 怎么用(命令、参数、示例、FAQ)
- [doc/config_file.md](doc/config_file.md) — JSON 配置文件
- [doc/compile_system_tech.md](doc/compile_system_tech.md) — 编译系统(pipe / xmake / cmake)技术说明
- [doc/dependencies.md](doc/dependencies.md) — 依赖

### 混淆与保护

- [doc_ai/cloakwork_research.md](doc_ai/cloakwork_research.md) — Cloakwork 借鉴项评估（issue #829）
- [doc_ai/regc_safe_env.md](doc_ai/regc_safe_env.md) — safe-env(JNI 函数表恢复)
- 产物 DLL 加壳（sepacker 扩展）需求见根 [doc_ai/dll_packer_requirements.md](../doc_ai/dll_packer_requirements.md)

### 架构与设计

- [doc/architecture.md](doc/architecture.md) — 当前架构概览
- [doc/roadmap.md](doc/roadmap.md) — v2.2 能力路线图与优先级
- [doc_ai/ir_design.md](doc_ai/ir_design.md) / [doc_ai/ssa_algorithm.md](doc_ai/ssa_algorithm.md) — MIR/SSA
- [doc_ai/regc_codegen.md](doc_ai/regc_codegen.md) / [doc_ai/regc_intrinsic.md](doc_ai/regc_intrinsic.md) — RegC 生成与 intrinsic
- [doc_ai/vmc4_multi_vm_design.md](doc_ai/vmc4_multi_vm_design.md) / [doc_ai/vmc4_red_team_contract.md](doc_ai/vmc4_red_team_contract.md) — VMC4 设计与红队契约
- [doc_ai/capabilities.md](doc_ai/capabilities.md) — 已实现功能清单 + 边界
- doc_ai/ 完整索引见 [doc_ai/README.md](doc_ai/README.md)

### 后端

- [doc/stackc_backend_readme.md](doc/stackc_backend_readme.md) / [doc/regc_backend_readme.md](doc/regc_backend_readme.md) — 各后端支持矩阵
- [doc_ai/regc_template_engine.md](doc_ai/regc_template_engine.md) — 模板化源码生成引擎

### 测试与验证

- [doc_ai/test_strategy.md](doc_ai/test_strategy.md) / [doc/regc_test_strategy.md](doc/regc_test_strategy.md)
- [doc_ai/performance_evaluation_spec.md](doc_ai/performance_evaluation_spec.md) — 性能评测方法学
- [benchmark/README.md](benchmark/README.md) — 基准脚本与结果

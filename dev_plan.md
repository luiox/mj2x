# mj2x — dev_plan

> 本文档**只放未完成项**。已完成功能见 [doc_ai/capabilities.md](doc_ai/capabilities.md);路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7):任务完成 → 从本文件删除 → 加到 CAPABILITIES。本文件不允许已完成勾选项长期存在。

---

## 当前阻塞

无。

## 待办

- [ ] **Native 后端与产物扩展**:在 --emit-cpp reserved CLI 入口之后,按小切片扩展 C++17 和可选 LLVM IR 输出,不改变 StackC/RegC 的既有承诺。
  - 产物契约:把 BackendResult::C_SOURCE、--emit-c、.c 路径和 compileCSource 中的语言假设抽成显式 artifact/source-language contract,保留现有 C99 默认行为和 fail-closed 诊断。
  - C++17 输出:复用 L1 MIR -> SSA -> RegIR 和 RegC lowering,只新增 C++ emitter/dialect、.cpp 工程模板以及 MSVC/clang++/g++ 编译路径;继续通过 mj2x-crt C ABI 交互,不复制一套 JNI 语义。
  - LLVM IR 实验后端:从 RegIR 发射 textual .ll,通过显式 opt-in 的外部 clang/llc 工具链编译,不把 LLVM 作为默认构建依赖;第一批只覆盖纯计算和控制流,对象/JNI/异常/monitor 在有设计和 E2E 前保持 fail closed。
  - 验收:每种产物必须有 CLI 负路径、结构断言、native E2E、Java baseline 对照和跨工具链记录;保护收益由 mjdbg weakness report 验证,不能仅凭输出语言宣称增强。
- [ ] **mj2x 持续优化专项(大计划)**:围绕 #467/#468/#471 继续按“小批次、可结构断言、可 native E2E、可 benchmark”的节奏推进,避免一次性扩大语义面。
  - 阶段 A:基线收口。以 #472 合入后的 String intrinsic 为起点,保持 discarded-return、空串 fast path、String read hot path、embedded xmake CRT 资源和现有 RegC cache pool 回归稳定;每轮都跑 `mj2x_unittest`/`mj2x_runtime_test`/CLI 生成器 smoke。
  - 阶段 B:JNI cache CSE 深化(#467)。已完成 TU 级 method/field/class/static/instance cache 与固定异常类、String intrinsic class、编译期 allocation descriptor cache、multi-array 子 descriptor cache,并删除 emitter 内嵌 StringBuilder helper;下一步只评估受限动态 descriptor cache,并用 cache symbol 数、生成 C 体积、C 编译耗时记录收益。
  - 阶段 C:Java intrinsic 扩面(#468)。继续按 benchmark 命中率补 String 纯读和其他低风险签名;新增 shadow object 必须同时定义 materialize 或可证明正确的 Java fallback。
  - 阶段 E:错误修复与可诊断性。把 unsupported reason、异常路由、local-ref 生命周期、descriptor/class lookup 语义和 native 编译错误提示作为固定修复池;每个 bug 需要最小 class/JAR fixture 或结构测试防回归。
  - 阶段 F:性能与红队闭环。补齐可提交 benchmark/protected sample corpus,记录 baseline vs RegC、JNI helper 调用面、cache declaration 数、生成/编译耗时;再交给 mjdbg/c2j 观察 trace surface,用 weakness report 反推下一批 intrinsic 或变换 pass。
- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:保持现有 intrinsic(Math/wrapper parse/toString/compare/value-unbox/String.valueOf/Random/String UTF-16 hot paths)在统一 registry 内稳定;剩余工作优先是 Random escape 边界、unsupported reason、benchmark 和产品入口回归。#439 Math/Random 已有 runtime/emitter/sample 验证,可按验证结果关闭。
- [ ] **RegC JNI cache CSE(后续)**:已完成 generated C TU 级 static/instance method、static/instance field、class cache interning,且 String intrinsic 的 `java/lang/String` class cache、固定异常类(`NullPointerException`/`ClassCastException`/`OutOfMemoryError`/`StringIndexOutOfBoundsException`/`UnsupportedOperationException`)、编译期 descriptor 的 `new`/reference-array allocation 和 multi-array 子 descriptor 已纳入统一 class cache pool;剩余评估动态 `_mj2x_regc_find_class`/runtime `class_name` 回调是否继续优化,并补 benchmark 对比。

## 下一步验证命令

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter=CliTest.*
xmake run mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

`CliTest.*` 覆盖 CLI 注解筛选、`--compile` 错误提示和可用工具链 smoke；本机缺少 `JAVA_HOME` 或没有可用 C 编译器时，native library 成功编译 smoke 会按环境跳过。

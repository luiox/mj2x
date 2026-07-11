# mj2x — dev_plan

> 本文档**只放未完成项**。已完成功能见 [doc_ai/capabilities.md](doc_ai/capabilities.md);路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`doc_ai/documentation_governance.md` §7):任务完成 → 从本文件删除 → 加到 CAPABILITIES。本文件不允许已完成勾选项长期存在。

---

## 当前阻塞

无。

## 待办

- [ ] **mj2x 持续优化专项(大计划)**:围绕 #467/#468/#471 继续按“小批次、可结构断言、可 native E2E、可 benchmark”的节奏推进,避免一次性扩大语义面。
  - 阶段 A:基线收口。以 #472 合入后的 String intrinsic 为起点,保持 discarded-return、空串 fast path、String read hot path、embedded xmake CRT 资源和现有 RegC cache pool 回归稳定;每轮都跑 `mj2x_unittest`/`mj2x_runtime_test`/CLI 生成器 smoke。
  - 阶段 B:JNI cache CSE 深化(#467)。已完成 TU 级 method/field/class/static/instance cache 与固定异常类、String intrinsic class、编译期 allocation descriptor cache、multi-array 子 descriptor cache,并删除 emitter 内嵌 StringBuilder helper;下一步只评估受限动态 descriptor cache,并用 cache symbol 数、生成 C 体积、C 编译耗时记录收益。
  - 阶段 C:Java intrinsic 扩面(#468)。优先补 benchmark 高命中的低风险签名:更多 StringBuilder range/capacity 重载、StringBuffer 和 String 纯读补全;新增 shadow object 必须同时定义 materialize 或可证明正确的 Java fallback。
  - 阶段 D:StringBuilder/UTF-16 语义升级。已完成 CRT opaque 动态 UTF-16 state、统一 cleanup、常用 append/toString、中文/surrogate 语义,并覆盖返回、未知调用、字段、数组和 CFG alias 的 escape-aware materialize;逃逸后继续修改的链从创建点整体回退 Java。后续只保留 StringBuilder range/capacity API、更多重载、StringBuffer 同步语义和 `String.split`。
  - 阶段 E:错误修复与可诊断性。把 unsupported reason、异常路由、local-ref 生命周期、descriptor/class lookup 语义和 native 编译错误提示作为固定修复池;每个 bug 需要最小 class/JAR fixture 或结构测试防回归。
  - 阶段 F:性能与红队闭环。补齐可提交 benchmark/protected sample corpus,记录 baseline vs RegC、JNI helper 调用面、cache declaration 数、生成/编译耗时;再交给 mjdbg/c2j 观察 trace surface,用 weakness report 反推下一批 intrinsic 或变换 pass。
- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:保持现有 intrinsic(Math/wrapper parse/toString/compare/value-unbox/String.valueOf/Random/String UTF-16 hot paths)在统一 registry 内稳定;剩余工作优先是 Random escape 边界、unsupported reason、benchmark 和产品入口回归。#439 Math/Random 已有 runtime/emitter/sample 验证,可按验证结果关闭。
- [ ] **RegC JNI cache CSE(后续)**:已完成 generated C TU 级 static/instance method、static/instance field、class cache interning,且 String intrinsic 的 `java/lang/String` class cache、固定异常类(`NullPointerException`/`ClassCastException`/`OutOfMemoryError`/`StringIndexOutOfBoundsException`/`UnsupportedOperationException`)、编译期 descriptor 的 `new`/reference-array allocation 和 multi-array 子 descriptor 已纳入统一 class cache pool;剩余评估动态 `_mj2x_regc_find_class`/runtime `class_name` 回调是否继续优化,并补 benchmark 对比。
- [ ] **运行时反侦测体系(issue #411,剩余集成)**:mockable core 已覆盖四类威胁的第二切片——IDE/JDWP 参数、java agent 参数与模块名、JVMTI dump 模块名与 capability 快照、native debugger API 与软件断点字节扫描。剩余工作是接入真实运行时枚举/查询:进程模块枚举、JVMTI capability live query、PEB/NtQuery/硬件断点/代码段扫描、策略层 fail-closed 编排。
- [ ] **String/StringBuilder intrinsic(issue #440)**:已完成 String read hot paths、`concat`/`substring`/`compareTo`/`indexOf` overload/`replace(char,char)`/`trim`,以及 StringBuilder opaque CRT state、动态 UTF-16 ownership、常用 append/toString、统一 cleanup 和 escape-aware materialize/fallback。`contains(CharSequence)` 对非 String rhs 仍通过缓存 Java methodID 保持语义。剩余工作是 StringBuilder range/capacity API 与更多重载、StringBuffer 同步语义、`String.split`;#440 不能关闭。
- [ ] **xmake 项目生成器重构**(`feat/mj2x/xmake-project-generator` 分支):当前 emit 后字符串改写,迁到 codegen 阶段直出。前置:建立 golden test 固化当前输出。

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

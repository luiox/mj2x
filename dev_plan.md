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
  - 阶段 B:JNI cache CSE 深化(#467)。已完成 TU 级 method/field/class/static/instance cache 与固定异常类、String intrinsic class、编译期 allocation descriptor cache;下一步只评估 multi-array 子 descriptor、受限动态 descriptor cache、lazy/conditional helper emission,并用 cache symbol 数、生成 C 体积、C 编译耗时记录收益。
  - 阶段 C:Java intrinsic 扩面(#468)。优先补 benchmark 高命中的低风险签名:更多 StringBuilder `append` 重载、wrapper 常见重载、String 纯读补全;所有 shadow object 继续 fail-closed,直到 materialize/fallback 能证明正确。
  - 阶段 D:StringBuilder/UTF-16 语义升级。已从非逃逸链扩到 `append(boolean|char|float|double|Object|CharSequence)` 简单重载,其中 `append(Object|CharSequence)` 通过缓存的 `Object.toString` methodID 保持虚分派语义,`append(float|double)` 通过缓存的 `String.valueOf(F/D)` 保持 Java 浮点文本语义;后续做更多重载、容量增长、UTF-16 ownership、surrogate/非 ASCII、一旦逃逸时 materialize 回 Java 对象;未完成前不声称完整 JDK StringBuilder 语义。
  - 阶段 E:错误修复与可诊断性。把 unsupported reason、异常路由、local-ref 生命周期、descriptor/class lookup 语义和 native 编译错误提示作为固定修复池;每个 bug 需要最小 class/JAR fixture 或结构测试防回归。
  - 阶段 F:性能与红队闭环。补齐可提交 benchmark/protected sample corpus,记录 baseline vs RegC、JNI helper 调用面、cache declaration 数、生成/编译耗时;再交给 mjdbg/c2j 观察 trace surface,用 weakness report 反推下一批 intrinsic 或变换 pass。
- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:保持现有 intrinsic(Math/parseInt/parseLong/wrapper toString/compare/value-unbox/String.valueOf/Random/String read hot paths)在统一 registry 内稳定;剩余工作优先是 escape-aware fallback、unsupported reason 和产品入口回归。#439 Math/Random 已有 runtime/emitter/sample 验证,可按验证结果关闭;#438 已覆盖 wrapper value/unbox、`Integer.parseInt`、`Long.parseLong`、wrapper `toString`、wrapper `compare` 和 `String.valueOf` 常见重载的低风险切片,剩余是更完整的包装类重载/异常语义/逃逸边界验证。
- [ ] **RegC JNI cache CSE(后续)**:已完成 generated C TU 级 static/instance method、static/instance field、class cache interning,且 String intrinsic 的 `java/lang/String` class cache、固定异常类(`NullPointerException`/`ClassCastException`/`OutOfMemoryError`/`StringIndexOutOfBoundsException`/`UnsupportedOperationException`)和编译期 descriptor 的 `new`/reference-array allocation 已纳入统一 class cache pool;剩余评估 multi-array 子 descriptor、动态 `_mj2x_regc_find_class`/runtime `class_name` 回调是否继续优化,并补 benchmark 对比。
- [ ] **运行时反侦测体系(issue #411,剩余集成)**:mockable core 已覆盖四类威胁的第二切片——IDE/JDWP 参数、java agent 参数与模块名、JVMTI dump 模块名与 capability 快照、native debugger API 与软件断点字节扫描。剩余工作是接入真实运行时枚举/查询:进程模块枚举、JVMTI capability live query、PEB/NtQuery/硬件断点/代码段扫描、策略层 fail-closed 编排。
- [ ] **String/StringBuilder intrinsic(issue #440)**:已完成设计、runtime 最小 UTF-16 view 垫片(`length`/`charAt`/status)、RegC String read hot paths(`equals`/`length`/`charAt`/`hashCode`/`isEmpty`/`startsWith`/`endsWith`/`indexOf`/`contains`)及 StringBuilder 非逃逸第一片(`new`/`<init>`/`append(String|Object|CharSequence|int|long|boolean|char|float|double)`/`toString`)。`contains(CharSequence)` 对 String rhs 走 native `indexOf` 热路径,非 String `CharSequence` 通过缓存 `String.contains` methodID 回退以保持语义。剩余工作是 UTF-16 ownership、escape-aware fallback、jobject materialize、StringBuilder 扩容/更多重载/逃逸 materialize 边界;#440 不能关闭。
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

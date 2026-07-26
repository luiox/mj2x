# mj2x — dev_plan

> 本文档只记录未完成工作。已支持能力见 [doc_ai/capabilities.md](doc_ai/capabilities.md)，路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7)：任务完成后从本文件删除，并更新 CAPABILITIES。

## 后续待办

- [ ] **R5：mjdbg 闭环验证**：使用 mjdbg/c2j 验证 RegC 保护强度并产出量化结论。当前阻塞：仓库未提供 c2j CLI；本机 mjdbg protected-sample 与 native trace smoke 均在 adopt 后的 `trace start` CLI 请求超时。待 trace CLI 恢复并提供 c2j agent 后复验。
- [ ] **R6：RegC 覆盖率提升到接近 100%（仿 JNIC/native-obfuscator 全转策略）**。当前 `TEST.jar` 业务方法覆盖率从 82% 提升到 ~90%（IO/类加载/内部类黑名单已放开，详见 [regc_native_coverage_analysis.md](doc_ai/regc_native_coverage_analysis.md) §13）。仅剩并发组（有意保留）与 invokedynamic（R6.1）。
  - [x] **R6-A：IO 组放开**（2026-07-25）：移除 `java/io/InputStream`/`ByteArrayInputStream` 黑名单。附带修复 `_mj2x_regc_collect_args` 的数组参数 bug（`[X` 被当元素类型读，导致 `ByteArrayInputStream(byte[])` 构造错乱 crash）。e2e `IOGroupRoundTrip` 通过。详见覆盖率文档 §13。
  - [x] **R6-B：类加载组放开**（2026-07-25）：移除 `java/lang/ClassLoader` 黑名单。`loadClass(String)`/`getResourceAsStream(String)`/`getSystemClassLoader()` 在 native 帧端到端通过，引用管理吸收 Class/InputStream ref 返回。e2e `ClassLoaderGroupRoundTrip` 通过。
  - [x] **R6-C：内部类访问路径验证**（2026-07-25）：内部类 `$` skip 已随 RegisterNatives（PR #661）撤销，本批补端到端验证——非静态内部类 `this$0` 合成字段访问 + 调用外部类实例方法（`Outer.this.method()`）在 native 帧走通。e2e `InnerClassFieldAccessRoundTrip` 通过。附带修复实例方法分派的抽象 owner bug（见 §13）。
  - [x] **R6-D：并发组根因调研 → 决策保留**（2026-07-25）：实测 `Thread.sleep(long)` 在主线程 native 帧调用**不**抛 `IllegalMonitorStateException`（与 dev_plan 旧描述不符），仅是调用结果与 baseline 存在调度抖动差异。原描述的 IMSE 只在线程池工作线程场景成立（`ReentrantLock` monitor 与 native 帧交互）。**决策**：保留并发组黑名单（`Thread`/`java.util.concurrent`）——线程池/锁语义在 native 帧不可靠，放开风险高且收益低（并发 API 本就稀疏）。详情见 §13。
  - **R6.1：invokedynamic 支持**（分阶段，优先 pre-SSA expansion 而非照搬 StackC runtime bridge——RegC 缺 owner-class 使 bridge 成本高，详见 [regc_invokedynamic_strategy.md](doc_ai/regc_invokedynamic_strategy.md)）：
    - [x] **阶段1：string concat 展开**（2026-07-25）：`InvokeDynamicPreprocessor` 把 `StringConcatFactory.makeConcat[WithConstants]` 改写成 `new StringBuilder`/`append` 链/`toString` 字节码（栈参数先 xSTORE 到新局部槽，按 recipe 展开），RegC 已有 StringBuilder intrinsic 故转 native 无需 native 新代码、绕开 owner-class 障碍。e2e `InvokeDynamicStringConcatRoundTrip` + 单元 `InvokeDynamicPreprocessorTest.*` 通过。详见覆盖率文档 §14。
    - [x] **阶段2：无状态 + 捕获 lambda 展开**（2026-07-25）：`InvokeDynamicPreprocessor` 为每个 `LambdaMetafactory.metafactory` 站点合成实现 functional interface 的类（SAM `invokestatic` 转调 impl，impl 形参=captured++sam）。无状态=空 `<init>`；捕获=N 个 private final 字段 + 参数化 `<init>`（站点栈上捕获值先 xSTORE 暂存、再 new/dup/顺序 xLOAD/invokespecial），SAM 先 getfield 取回捕获值。私有目标提权避免 IllegalAccessError；`OutputWriter::isGeneratedRuntimeClass` 认 `mj2x/lambda/*` 前缀。阶段2 MVP 覆盖 impl tag==`H_INVOKESTATIC`、无装箱/桥接。e2e `InvokeDynamicStatelessLambdaRoundTrip`/`InvokeDynamicCapturingLambdaRoundTrip` + 单元 `ExpandsStatelessLambda`/`ExpandsCapturingLambda` 通过。详见覆盖率文档 §15。
    - [x] **阶段2c：实例 + 构造器方法引用**（2026-07-26）：`buildLambdaClassNode` 按 impl 的 method-handle tag 发射 SAM 体——`H_INVOKEVIRTUAL`/`H_INVOKEINTERFACE`（实例引用，receiver=fullArgs 首元：bound 来自捕获字段、unbound 来自 SAM 首参）、`H_INVOKESPECIAL`（私有实例引用，提权后按 INVOKEVIRTUAL 发射）、`H_NEWINVOKESPECIAL`（构造器引用，`NEW`/`DUP`/args/`INVOKESPECIAL <init>`/`ARETURN`）。`expandLambdaSite` 改为 tag 感知校验（实例 receiver-first、返回类别一致性守卫拒装箱/桥接），de-privatize 扩展为「用户类非 public 目标（含私有实例方法、包私有构造器）一律提权 public」。站点改写与 tag 无关，复用阶段2b 捕获逻辑。e2e `InvokeDynamicMethodRefRoundTrip`（String::length/charAt、base::concat、String::new）+ 单元 `ExpandsUnboundInstanceRef`/`ExpandsBoundInstanceRef`/`ExpandsInterfaceMethodRef`/`ExpandsConstructorRef`/`LeavesUnsupportedHandleUnexpanded` 通过。装箱/桥接（samMethodType≠implDesc）+ altMetafactory 留后续。
    - [ ] **阶段3：自定义 bootstrap**：混淆器 bootstrap 不在编译期执行任意代码，走 fail-closed 诊断或后续受保护运行时 call-site helper（需 mjdbg 反馈）。备选：若需全形态覆盖，评估照搬 StackC runtime bridge（`stackc_backend.cpp:2732-3208`，477 行），但须先给 RegC native 路径引入 owner-class 获取机制。
  - [x] **R6.2：native 库打包/加密技术 wiki**（草稿，知识储备）：已整理 [native_library_packaging.md](doc_ai/native_library_packaging.md)。具体解密算法待实现时逆向补充。

## 下一步验证命令

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter=CliTest.*
xmake run mj2x_unittest --gtest_filter=RegCCompareIntrinsicTest.*
xmake run mj2x_integration_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

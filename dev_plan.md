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
  - [ ] **R6.1：invokedynamic 搬迁**（独立大工程）：StackC 有完整 477 行 native 实现（`stackc_backend.cpp:2729`），RegC 三层全无。方案：RegIR 保留单条 InvokeDynamic 指令（不在 SSA 层展开），emitter 层照搬 StackC，机械替换 `stack[]`→寄存器表达式。
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

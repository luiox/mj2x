# mj2x — dev_plan

> 本文档只记录未完成工作。已支持能力见 [doc_ai/capabilities.md](doc_ai/capabilities.md)，路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7)：任务完成后从本文件删除，并更新 CAPABILITIES。

## 后续待办

- [ ] **R5：mjdbg 闭环验证**：使用 mjdbg/c2j 验证 RegC 保护强度并产出量化结论。当前阻塞：仓库未提供 c2j CLI；本机 mjdbg protected-sample 与 native trace smoke 均在 adopt 后的 `trace start` CLI 请求超时。待 trace CLI 恢复并提供 c2j agent 后复验。
- [ ] **R6：RegC 覆盖率提升到接近 100%（仿 JNIC/native-obfuscator 全转策略）**。实测发现：仅放宽异常表阈值（`tryCatchBlocks>4`）安全有效（`pack.Main` 14 个 catch 能正确转 native，全测试通过，已合入）。但取消 high-risk 黑名单 / 内部类 skip 会破坏语义——根因是 RegC 的 `_mj2x_regc_call_ref` 返回的 JNI local ref 不做 `NewLocalRef`/`DeleteLocalRef` 管理，反射返回的 Class/Method/Field 对象生命周期与 Java 不一致，且内部类 `this$0` 字段/外部类方法调用路径不完善。实测表现：反射测试（Class.*/reflect.*）全失败、内部类测试失败、Thread.sleep 破坏线程池 monitor。**前置条件**：先完善 RegC call 路径的引用管理（local ref 追踪/释放，参考 StackC 的 `trackLocalRef`），再分项放开黑名单（反射→类加载→IO→并发）与内部类 skip。详见 [regc_native_coverage_analysis.md](doc_ai/regc_native_coverage_analysis.md) §11。
  - [ ] **R6.1：invokedynamic 搬迁**（独立大工程）：StackC 有完整 477 行 native 实现（`stackc_backend.cpp:2729`），RegC 三层全无。方案：RegIR 保留单条 InvokeDynamic 指令（不在 SSA 层展开），emitter 层照搬 StackC，机械替换 `stack[]`→寄存器表达式。
  - [ ] **R6.2：native 库打包/加密技术 wiki**（知识储备，不实现）：调研整理 `doc_ai/native_library_packaging.md`，记录明文资源/压缩归档（`CompressDataTool`）/加密解密流（JNIC `JNICLoader` extends InputStream）/xor 变种四种方式。

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

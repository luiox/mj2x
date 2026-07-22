# mj2x — dev_plan

> 本文档只记录未完成工作。已支持能力见 [doc_ai/capabilities.md](doc_ai/capabilities.md)，路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7)：任务完成后从本文件删除，并更新 CAPABILITIES。

## 后续待办

- [ ] **R4：L4 变换 pass 系统**：实现 MBA、CFF、opaque predicate。前置：RegC/R3 live 验证。
- [ ] **R5：mjdbg 闭环验证**：使用 mjdbg/c2j 验证 RegC 保护强度并产出量化结论。前置：R3 RegC ABI live 验证与样本反馈。
- [ ] **CI 接入 mtest_set 与 mj2x**：评估 Linux CI 的 JDK/MSVC 可用性，将 `mtest_set_unittest` 与 `mj2x_unittest` 纳入 CI 矩阵；工具链缺失时使用 GTEST_SKIP。

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

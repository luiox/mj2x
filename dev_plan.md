# mj2x — dev_plan

> 本文档只记录未完成工作。已支持能力见 [doc_ai/capabilities.md](doc_ai/capabilities.md)，路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7)：任务完成后从本文件删除，并更新 CAPABILITIES。

## 后续待办

- [ ] **RegC 后续优化与回归修复**：围绕新发现的性能或正确性数据，按可结构断言、可 native E2E、可 benchmark 的小批次推进。
  - 维持 String intrinsic、discarded-return、空串 fast path、String read hot path、embedded xmake CRT 资源和 RegC cache pool 的回归稳定。
  - 评估受限动态 descriptor cache，记录 cache symbol 数、生成 C 体积与 C 编译耗时。
  - 按 benchmark 命中率扩展低风险 Java intrinsic；新增 shadow object 必须定义 materialize 或可证明正确的 Java fallback。
  - 将 unsupported reason、异常路由、local-ref 生命周期、descriptor/class lookup 语义和 native 编译错误提示纳入固定修复池。
  - 建立 baseline vs RegC、JNI helper 调用面、cache declaration 数和生成/编译耗时的可提交 benchmark，并用 mjdbg/c2j weakness report 驱动下一批工作。
- [ ] **R4：L4 变换 pass 系统**：实现 MBA、CFF、opaque predicate。前置：RegC/R3 live 验证。
- [ ] **R5：mjdbg 闭环验证**：使用 mjdbg/c2j 验证 RegC 保护强度并产出量化结论。前置：R3 RegC ABI live 验证与样本反馈。
- [ ] **RegC JNI cache CSE 后续**：评估动态 `_mj2x_regc_find_class` 和 runtime `class_name` 回调是否值得继续优化，并补 benchmark 对比。
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

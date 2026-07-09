# mj2x — dev_plan

> 本文档**只放未完成项**。已完成功能见 [doc_ai/capabilities.md](doc_ai/capabilities.md);路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`doc_ai/documentation_governance.md` §7):任务完成 → 从本文件删除 → 加到 CAPABILITIES。本文件不允许已完成勾选项长期存在。

---

## 当前阻塞

无。

## 待办

- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:保持现有 intrinsic(Math/parseInt/包装类/Random)在统一 registry 内稳定;剩余工作优先是 escape-aware fallback、unsupported reason 和产品入口回归,不是新增 String/StringBuilder 覆盖。
- [ ] **运行时反侦测体系(issue #411)**:四类威胁——反 jvmtp/IDE 调试、反 java agent(arthas 类,不能光靠启动参数)、反 jvmti dump agent(mjt-dump 类)、反 native 调试器(x64dbg 类)。现有 antidebug/antijvmti 是初版切片(IsDebuggerPresent + 参数扫描 + JNI table 篡改),需纵向加深到运行时 transformer 枚举、JVMTI 能力查询、PEB/NtQuery/断点扫描。
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

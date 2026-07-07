# mj2x — dev_plan

> 本文档**只放未完成项**。已完成功能见 [doc_ai/capabilities.md](doc_ai/capabilities.md);路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`doc_ai/documentation_governance.md` §7):任务完成 → 从本文件删除 → 加到 CAPABILITIES。本文件不允许 `[x]` 长期存在。

---

## 当前阻塞

无。

## 待办

- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:intrinsic(Math/parseInt)已 live 验证(E2E 通过);剩余把 inline RegC helper 逐步迁到 R3 runtime ABI、补 SSA/RegC 常见 javac 输入形状。
- [ ] **JDK intrinsic 覆盖扩展**:把已交付的 runtime intrinsic(包装类 valueOf/intValue、Random nextInt/nextLong/...)接入 RegC emitter。当前阻塞点:包装类对象与 Random 对象在 native 侧需有逃逸表示(mj2x_int struct ↔ mj2x_ref jobject 边界转换),Math/parseInt 因无对象逃逸已通。
- [ ] **xmake 项目生成器重构**(`feat/mj2x/xmake-project-generator` 分支):当前 emit 后字符串改写,迁到 codegen 阶段直出。前置:建立 golden test 固化当前输出。

## 下一步验证命令

```bash
xmake build mj2x-core mj2x-cli mj2x_unittest mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
```

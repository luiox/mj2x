# mj2x — 开发计划

> 顶层路线图，详细计划见各 doc
>
> - [架构设计](doc/ARCHITECTURE.md)
> - [依赖分析](doc/DEPENDENCIES.md)
> - [MVP 计划](doc/MVP_PLAN.md)
> - [测试策略](doc/TEST_STRATEGY.md)

---

## 路线图

```
Phase 1: 骨架 ──▶  Phase 2: MIR ──▶  Phase 3: C 后端 ──▶  Phase 4: E2E
   1 周                1.5 周             1.5 周               1 周
```

### Phase 1: 项目骨架

- [ ] 设计文档审阅通过
- [ ] xmake 构建配置 + core lib 编译
- [ ] CompilerIndex + IO
- [ ] CLI 入口
- [ ] 测试框架就绪

### Phase 2: MIR + CodeConverter

- [ ] MIRNode 完整定义 (全部 MIRKind)
- [ ] CodeConverter 指令翻译 (所有 MVP 指令)
- [ ] Block 构建 + CFG
- [ ] MethodStack 栈类型推导

### Phase 3: C 后端

- [ ] Backend 接口定型
- [ ] CBackend: 每个 MIRKind → C 代码
- [ ] JNI 名称修饰 + ID 缓存
- [ ] injectIntoClass + finalize

### Phase 4: 端到端验证

- [ ] Pipeline 完整集成
- [ ] 简单 Java 方法 → C 编译通过
- [ ] 测试覆盖 ≥ 80
- [ ] 文档完善

---

## 分支策略

```
main
  └── feat/mj2x-init          (当前，设计阶段)
       └── feat/mj2x-phase1   (实现阶段，从 init 分出)
            └── feat/mj2x-phase2
                 └── ...
```

- 每个 Phase 从上一 Phase 分支分出
- Phase 完成后 squash merge 回 `feat/mj2x-init`
- MVP 完成后 PR merge 到 `main`

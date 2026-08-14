# mj2x doc_ai — 设计文档索引

> 本目录为 mj2x 的 AI/工程设计文档集。导航顺序：先读 [capabilities.md](capabilities.md)
> 了解已实现边界，再按需查阅下列分类。所有文档与主分支同步演进，过时项会标注状态。

## 总览与能力

| 文档 | 内容 |
|---|---|
| [capabilities.md](capabilities.md) | 已实现功能清单（三后端支持矩阵、runtime、验证入口） |
| [test_strategy.md](test_strategy.md) | 测试分层策略（单测 / e2e / mtest_set / 覆盖率审计） |
| [config_system_design.md](config_system_design.md) | 配置系统设计（参数注册表单源 + JSON Schema 导出，GUI 工具路径；P1 未实现） |
| [competitor_config_reference.md](competitor_config_reference.md) | 竞品配置系统调研（jvmtp / skidoni，配置设计输入依据） |

## IR 与前端

| 文档 | 内容 |
|---|---|
| [ir_design.md](ir_design.md) | 五层 IR 路线（L1 MIR → SSA → RegIR → L4 → 后端）与架构决策 |
| [ssa_algorithm.md](ssa_algorithm.md) | SSA 构建算法（Phi / 支配树 / 消解） |
| [preprocessor.md](preprocessor.md) | 可插拔预处理器（LDC / invokedynamic / multianewarray 等） |

## RegC 后端

| 文档 | 内容 |
|---|---|
| [regc_codegen.md](regc_codegen.md) | RegC 生成器设计（寄存器分配、块布局、异常分发） |
| [regc_intrinsic.md](regc_intrinsic.md) | intrinsic 框架与注册表（Math/wrapper/位操作/arraycopy 等） |
| [regc_template_engine.md](regc_template_engine.md) | 模板化源码生成引擎（CExpr/CodeOptions/CodeTemplate 五层模型） |
| [codegen_style.md](codegen_style.md) | 生成代码风格规范（CSourceStream / 缩进 / 注释） |
| [regc_safe_env.md](regc_safe_env.md) | safe-env：JNI 函数表快照 + 方法入口恢复（阻断 JNI hook，issue #748，PR #754） |
| [regc_invokedynamic_strategy.md](regc_invokedynamic_strategy.md) | invokedynamic / LambdaMetafactory 处理策略 |
| [regc_native_coverage_analysis.md](regc_native_coverage_analysis.md) | 真实样本 native 覆盖率分析 |

## VMC4 后端

| 文档 | 内容 |
|---|---|
| [vmc4_multi_vm_design.md](vmc4_multi_vm_design.md) | VMC4 多 VM 设计（dispatcher / 随机化 / drift / 语义 family） |
| [vmc4_red_team_contract.md](vmc4_red_team_contract.md) | 红队对抗契约（强度模型、观测面） |
| [vmc4_checkcast_support.md](vmc4_checkcast_support.md) | CHECKCAST intrinsic（转化率第一瓶颈消除） |

## 逆向参考（jnic / 竞品）

| 文档 | 内容 |
|---|---|
| [jnic_codegen_reference.md](jnic_codegen_reference.md) | jnic370 代码生成参考 |
| [jnic370_reverse_notes.md](jnic370_reverse_notes.md) | jnic370 逆向笔记 |
| [phantom_shield_codegen_notes.md](phantom_shield_codegen_notes.md) | PhantomShield 混淆器逆向笔记 |
| [native_obfuscator_codegen_notes.md](native_obfuscator_codegen_notes.md) | 其他 native 混淆器逆向笔记 |

## 运行时 / 产物

| 文档 | 内容 |
|---|---|
| [native_library_packaging.md](native_library_packaging.md) | native 库打包（JAR 内嵌 DLL/SO 加载） |
| [logging.md](logging.md) | 日志门面（libca log / SimpleLogBackend / --quiet / --verbose） |

## 性能

| 文档 | 内容 |
|---|---|
| [performance_evaluation_spec.md](performance_evaluation_spec.md) | 性能评测方法学（覆盖率审计强制项） |

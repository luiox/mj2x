# dev_plan：RegC 模板化源码生成引擎（feat/regc-template-engine）

> 依据 doc_ai/regc_template_engine.md。分步迁移，每步语义等价（13-probe + 快测试组守门）。

## 状态总览

| Step | 内容 | 状态 |
|---|---|---|
| 0 | 引擎核心（CExpr/CodeOptions/CodeTemplate/CSourceStream 补充）+ 单测 | 完成（24 个引擎单测 + 422 全量） |
| 1 | L2 表达式层迁移 | 完成 |
| 2+3 | L1 语句层 + L3 结构层迁移 | 完成 |
| 4 | RegCEmitter 构造参数收敛 CodeOptions + CLI 接入 | 完成（`--regc-comment-level none\|summary\|detailed`） |
| 5 | 全量验证 + CodeTemplate 提取评估 | 完成（422 单测 + 13-probe 全过；提取留待 3+ 次重复触发） |

## 结论

本分支（feat/regc-template-engine）模板引擎改造全部完成。CodeTemplate 提取不强行做
（emitter 中重复片段已是独立函数如 emitEdgeCopies/emitRegCStatusCheck），机制已由
引擎单测验证，出现 3+ 次真实重复时再提取。

## 验证命令

- 引擎单测：`xmake run mj2x_unittest --gtest_filter="CSourceStreamTest.*:CodegenExprTest.*:CodegenTemplateTest.*:CodegenOptionsTest.*"`
- 快测试组：`xmake run mj2x_unittest --gtest_filter="RegCTest.*:RegCIntrinsicTest.*:RegCIntrinsicRegistryTest.*"`
- 探针：`D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar` reconvert → 编译 → 运行（13-probe）
- 注意：`xmake f -P <子项目>` 会破坏 root 配置（issue #782），统一在脚本内每次 `f -c`

## 日志

- 2026-08-11：开分支；设计文档完成；待实现引擎核心

# libjbc 迁移笔记（批 0 产物）

> 配套计划：[plan_libjbc_migration.md](plan_libjbc_migration.md)。本文件记录
> 覆盖面核对证据与迁移中的实现决策，按批追加。

## 批 0：覆盖面确认清单（2026-10-26，libjbc @ 0.4.15，9f1a13e）

| 确认项 | 结论 | 证据 |
|---|---|---|
| Runtime{Visible,Invisible}Annotations | ✅ typed 建模（`AttrKind::Annotations`，共模型） | `core/attribute.hpp:38` |
| Runtime{Visible,Invisible}ParameterAnnotations | ✅ `AttrKind::ParameterAnnotations` | `core/attribute.hpp:57` |
| AnnotationDefault | ✅ `AttrKind::AnnotationDefault` | `core/attribute.hpp:56` |
| TypeAnnotations（JVMS 4.7.20） | ✅ `ir/type_annotation_attr` 结构化建模 | `core/attribute.hpp:41` |
| BootstrapMethods | ✅ typed `BootstrapMethodsAttr` | `core/attribute.hpp:206` |
| indy/condy CP 条目 | ✅ `InvokeDynamic`/`Dynamic` kind + `addInvokeDynamic`/`addDynamic` intern（bsm 下标锚点校验） | `core/constant_pool.hpp:199-202` |
| MethodParameters | ✅ `AttrKind::MethodParameters` | `core/attribute.hpp:58` |
| Signature/EnclosingMethod/InnerClasses/Exceptions | ✅ typed | `core/attribute.hpp:40-46` |
| Nest/Record/PermittedSubclasses/Module 族 | ✅ typed（0.4.14/0.4.15 批次） | `core/attribute.hpp:47-53` |
| StackMapTable 读回 | ✅ `analysis::smt_read`（读侧语义帧视图，与计算帧同构，供对拍/预检） | `analysis/smt_read.hpp` |
| StackMapTable 生成 | ✅ `analysis::smt_build`（显式 API；Strict 域陈述口径，与 javac「公共前缀逐槽一致」对拍） | `analysis/smt_build.hpp` |
| maxStack/maxLocals 重算 | ✅ 帧引擎观测值 + 参数槽地板修正（`analysis/frame.hpp`）；**显式采用**（J7 落地前 lower/write 不自动） | `analysis/frame.hpp:266-268` |
| 成员级增删/adopt | ✅ 0.4.12 起；`MethodEditor::addMethodWithBody` 从零建方法体 | `ir/method_editor.hpp` |
| CP 压缩 | ✅ 独立 API（`ir/cp_compact`），**非 writeClass 默认行为**——迁移期不调用，行为与旧栈对齐 | `ir/cp_compact.hpp` |
| 保真写回 | ✅ 属性级四态（Untouched/Decoded/Modified/New）：未触碰 copy 源 span | `core/attribute.hpp:89-92` |
| 错误语义 | ✅ 全线 `Result` 值语义（`ca::core::Result`），不抛异常 | README + 头文件 |

**缺口：无。** mj2x 已知使用形态（注解选择、indy 改写、SMT 重算、合成类构建、
元属性直通）全部有对位 API。

## 包接线决策（批 0）

- 内联包定义 `xmake/packages/libjbc.lua`：复制自 mjt 同名文件（0.4.15 条目，
  含 Windows `/utf-8` on_load 修正——GBK ACP 探针修复），`add_versions` 锁
  commit 9f1a13ec3df96f3493703ee3c6fbb7c5186187bf。
- `add_requireconfs("libjbc.libca", …)` 钉回家族 libca 统一超集
  （LUIOX_LIBCA_VERSION/LUIOX_LIBCA_MODULES 双单源）。
- 包沙箱自解析的 libca 与消费图钉定版本/模块必须完全一致，否则双实例
  （D33 同族根因，见 root xmake.lua 注记）。

## 批 2 实证发现：旧栈 ClassWriter COMPUTE 模式有树/arena 侧副作用（2026-10-26）

**现象**：syncTreeToDom 以 COMPUTE_MAXS/COMPUTE_FRAMES 序列化后，
`LlvmSamples.GenerateMjdbgFixtures` 稳定失败（native 编译产物坏），且
`--dump-mir` 路径段错误；flags=0（plain）序列化则全部正常，MIR dump
逐字节一致（label 身份差异除外）。

**结论**：COMPUTE 模式的帧/maxs 计算在旧栈 writer 上留下了超出「产出字节」
的副作用（树侧 label 状态 / arena 状态）；同树二次序列化（output_writer）
与后续编译读到的状态因此漂移。基线里 preprocessors 内的既有 ClassWriter
全用 flags=0（cloader/indy 两处），从未触发此面。

**迁移决策**：syncTreeToDom 旗标恒 plain——同步通道只求「如实序列化当前
tree 状态」，帧重算归 output_writer（过渡期）/批 6 的 `smt_build`。
批 6 摘除旧栈 writer 后此副作用面整体消失；保真路径必须用 libjbc 的
writeClass（fidelity copy-through，无此副作用类）重验。

# PR 草稿：libjbytecode → libjbc 全栈迁移

> 状态：终态（批 6g 收口，2026-10-28）。按本稿开 PR（单分支
> `feat/libjbc-migration`，开完停手等合并，家族约定同 PR #20/#33）。

建议标题：`feat: libjbytecode → libjbc 全栈迁移——DOM 权威 + 保真直出，产品面零 libjbytecode`

## 目标与范围

mj2x 是旧栈 libjbytecode（ASM 风格 visitor/tree 模型）最后的活跃产品消费方；
家族（mjbase / mjt）已全线消费 libjbc。本 PR 完成 mj2x 对 libjbc 的整体切换：

- **模型更替**：node-first 惰性 DOM（骨架 eager、属性按需解码、保真写回）
  取代 tree 载体。阶段边界字节往返同步（免适配层）→ 逐批切消费方 → 树载体
  整体退役；未改写类字节原样直出（round-trip 恒等），旧 tree writer 不可
  复现的 verifier 敏感元数据不再失真。
- **错误语义对齐**：libjbc 全线 `Result` 值语义，摘除旧栈抛异常面（含 J-6
  空描述符越界读一类库层缺陷的根治面——解析面自批 1 起走 jbc 门面）。
- **产品面清零**：mj2x-core / mj2x-cli 摘除 libjbytecode 包与链接闭包（含
  libgarena 传递依赖），产品面（core/cli/rt）零 libjbytecode。
- **能力对位**：SMT 显式生成 + maxStack/maxLocals 重算（帧引擎）、成员级
  增删/adopt、MethodEditor/CodeEditor 编辑载体、indy/condy/注解族 typed
  建模——覆盖 mj2x 全部已知使用形态（盘点见 plan §三）。

规模：16 个提交，107 文件 +7512/−3963（mj2x-core / mj2x-cli / xmake 接线
/ test_helper / unittest / doc）。

## 分批清单

| 批次 | 内容 | 提交 |
|---|---|---|
| 批 0 | libjbc 0.4.15 内联包接线 + 覆盖面盘点（缺口：无） | 7ea4c96 |
| 批 1+2 | JvmType 描述符门面 / Opcodes 镜像 + tree→DOM 阶段边界同步 | 0923add + adb2b6f |
| 批 3a | MIR 常量载荷切 mj2x::Type 门面 + 同步 plain 化（修正：树侧载荷构造点限定旧栈类型） | 93eef88（修正 6e77aa8） |
| 批 3b | code_converter 读路径切 libjbc DOM | 53cadb0 |
| 批 4a | pre/post 改写载体切 DOM（6 pass + 管线双载体拆段） | 8bf3f79 |
| 批 4b | invokedynamic preprocessor 切 DOM（树载体收尾） | aefc14b |
| 批 5 | 四后端 injectIntoClass 切 DOM，编译后同步点消失 | fb80c26 |
| 批 6a | output 切 writeClass 直出 + modified 类帧重算垫片 | c2c9725 |
| 批 6d | 树载体退役：CompilerMethod 快照化 + 全消费方切 DOM | 1037818 |
| 批 6e | Type/Handle 门面收口，产品面零 libjbytecode | 805c330 |
| 批 6f | 测试件迁移（test_helper / unittest / mtest_set 夹具） | 9016e99 + 70d8c7d |
| 批 6g | 文档收口 + 全量回归终验 | 本提交 |

批 6 子批号按实际推进取用（6b/6c 未产生独立提交）。批次设计与风险预案见
[plan_libjbc_migration.md](plan_libjbc_migration.md)，逐批实证见
[libjbc_migration_notes.md](libjbc_migration_notes.md)。

## 验证现状

- **全量回归终验（批 6g 时点，第 5 轮）**：919 可跑 908 过 / 0 败 / 11 工具链
  门控跳过 / 9 disabled（总量 928），exit 0——对齐批 6a 基线。
- **回归数字口径**：早期勾账的 921/919 系总量笔误，真实总量 928（919 可跑
  + 9 disabled）；读 gtest 汇总核对 ran = 过 + 败 + 跳 恒等式。
- **批 6f 验证揪出并修复的产品缺陷**：stackc 分支发射标签错位（C2094 悬垂
  goto）、native loader 前向分支锚定拒发（见 notes 批 6f 实证节）。
- 逐批全量门历史：批 3b/4a/4b/5 时点均为 919 可跑全绿（908 过 / 11 跳）。

## 产品面零 libjbytecode 说明

- mj2x-core `add_packages` 摘 libjbytecode（剩 libca / libjbc / libjstr /
  libjzip）；mj2x-cli 链接闭包摘 `libjbytecode_{util,commons,analysis,tree,
  core}` 与 libgarena（libjbytecode 的传递依赖），cli 重链通过。
- 产品源码 grep 零 libjbytecode 引用（jvm_opcodes / block / indy_dispatch /
  reaching_def 保留 4 处注释性历史提及）。
- 仓内剩余消费方仅测试件：test_helper / unittest / mtest_set 夹具 + 根
  xmake.lua 的 `add_requires("libjbytecode 0.0.2")`。批 6f 迁移产品消费面的
  测试引用后按测试件收口口径处理（见下），届时根包声明一并退役。

## 遗留与后续

- **frame_recompute 换装点**：`PortedCommonSuperResolver`（旧栈
  DefaultCommonSuperClassProvider 硬编码父类链移植，行为对拍基线）+
  `recomputeSessionFrames` 是权宜实现；**mjbase 帧计算就绪后换装 mjbase 包
  实现**（已拍板后置，不阻塞本 PR）。
- **测试件 jbytecode 夹具保留建模用途**：批 6f 迁移测试件的产品消费面
  （class_dom_sync 引用、三明治、树夹具喂入改字节/DOM）；树建模类夹具按
  用途保留，不为迁移而强行清零测试面 libjbytecode 依赖。
- **T1 小卡未动**：dev_plan 账目校准 + BigSwitch/regfile 形态收编 E2E、
  finalize 错误通道统一、RegC intrinsic 判定热点、性能遗留三件等，按
  plan §五 在迁移合并后推进；终验收（§六）不在本 PR 范围。
- **帧引擎权宜实现**：frame_recompute（PortedCommonSuperResolver +
  recomputeSessionFrames）在 mjbase 帧计算就绪后换装，接口已收敛。

# 长任务计划：libjbytecode → libjbc 切换 + 功能完善 + 测试完善

> 立项日期 2026-10-26。本文件是跨会话执行账本：每批完成回写状态；与
> [dev_plan.md](../dev_plan.md)（滚动待办池）联动，本文只管**迁移主线与排期**。
> 基线快照：main @ 2653d10（CI linux+windows 全绿，27m43s）。

---

## 一、现状基线（2026-10-26 调研）

- 仓内测试体量：`mj2x_unittest` 850 例（#20 合并时点 840 过 / 9 skip / 0 fail，
  skip 属环境性）+ `mj2x_integration_test` + java_corpus 三后端矩阵
  （exclusions 已清零）+ JDK17 现代特性语料批（10 用例，2026-10-26 修复收官）。
- PR #20（CLI 后端接线单元化）已合并。
- **dev_plan.md 账目滞后**：J-12（regfile 并行拷贝环）、J-13（computed jumps
  大 switch）、J-15（regfile 区间计算）实际已修（e68d46f / f6d39b8 / e170a92），
  条目未销。修后动作（BigSwitch / regfile 形态收编 E2E、语料 obf 档
  `--vmc4-regfile` / `--regc-computed-jumps` 复用验证）待做 → 本计划 §5 T1。
- 挂账跨界缺陷：`jbytecode::core::Type::getArgumentTypes("")` 空描述符越界读
  → `IndyBootstrapPlanTest.WalkEdgeShapes` 非确定性翻车（J-6）。mj2x 侧只做了
  守卫，根治在库层——**切到 `jbc::type` 即根治**（迁移的直接收益之一）。

## 二、为什么切 libjbc（动机与对齐）

1. **家族方向已定**：mjbase（mbase 重写线）与 mjt 均已消费 libjbc 0.4.15；
   mjbase 迁移结论「旧栈 visitor 协议层语义已被 libjbc codec 吸收」。
   mj2x 是旧栈（libjbytecode，ASM 风格 visitor/tree 模型）最后的活跃产品消费方。
2. **模型更替**：libjbc 是 node-first 惰性 DOM——骨架 eager、属性按需解码、
   **保真写回**（未触碰字节原样 copy，触碰才重编码）。对 mj2x 的「被 glob
   剔除类原样保留在输出 JAR」「未改写成员不漂移」场景是净收益。
3. **错误语义对齐**：libjbc 全线 `Result` 值语义不抛异常，与 mj2x 仓约定一致；
   旧栈抛异常（含 J-6 的越界读漂移）。
4. **能力对位**：StackMapTable 显式生成（`analysis/smt_build`）+ maxStack/
   maxLocals 重算（`analysis/frame`）、成员级增删/adopt、`MethodEditor`/
   `CodeEditor` 编辑载体、Module/Nest/Record/注解族 typed 建模、condy
   （Dynamic）建模——覆盖 mj2x 全部已知使用形态（§三 映射表）。

## 三、迁移面盘点与 API 映射

### 3.1 使用面数据

- include 面：55 文件聚合头 `jbytecode/jbytecode.hpp` + 4 文件具体头，约 85 文件
  （mj2x-core 全后端 / io / frontend / pre·postprocessor、mtest_set、
  test_helper、unittest）。
- 符号密度：`core::Opcodes`(67)、`core::Label`(44)、`core::Type`(36)、
  `tree::MethodNode`(18)、`tree::AbstractInsnNode`(18)、`core::Handle`(13)、
  `tree::ClassNode`(10) + 指令节点家族（MethodInsn/TypeInsn/VarInsn/Ldc/
  InvokeDynamic/Insn/InsnList/FieldInsn/FieldNode/AnnotationNode）。
- 枢纽文件：`io/input_loader.cpp`（ClassReader）、`io/output_writer.cpp`
  （ClassWriter，modified→COMPUTE_FRAMES 否则 COMPUTE_MAXS）、
  `frontend/code_converter.cpp`（指令遍历）、7 个 preprocessor + 3 个
  postprocessor（改写与写回）、`backend/stackc/stackc_registration.cpp`
  （类序列化）。

### 3.2 API 映射表

| libjbytecode（旧） | libjbc（新） | 备注 |
|---|---|---|
| `ClassReader::parse + accept(ClassNode)` | `readClass(ByteView)` → `ClassFile` | 骨架 eager、属性 lazy；错误即值 |
| `tree::ClassNode` 树 | `core::ClassFile` DOM | 成员级增删 / adopt |
| `AbstractInsnNode` 链表遍历 | `ir::CodeDom`（promote 后生成式句柄） | 只读消费批用 |
| 指令流改写 | `ir::CodeEditor` / `ir::MethodEditor` | branch 锚自动改写、eraseBatch |
| `ClassWriter(arena, 0)` | `writeClass(cf)` | 保真写回 |
| `ClassWriter::COMPUTE_MAXS` | `analysis` 帧引擎 maxStack/maxLocals | 显式采用（J7 落地前不自动） |
| `ClassWriter::COMPUTE_FRAMES` | `analysis::smt_build` | 显式 API；Strict 域陈述口径，与 javac 按「公共前缀逐槽一致」对拍 |
| `core::Type`（描述符/签名解析） | `type::descriptor` / `type::signature` | 结构化签名树 |
| `core::Opcodes` | `core::opcode` | |
| `core::Label` | CodeDom 标签锚 | |
| `core::Handle` | CP 建模（bsm 下标锚点校验） | condy = `Kind::Dynamic`（bsm+name+desc）已建模 |
| 注解（AnnotationNode 族） | `Annotation`/`AnnotationDefaultAttr`/`ir::type_annotation_attr` | Runtime(In)Visible 共模型 |
| 抛异常（invalid_argument 等） | `Result` 值语义 | 对齐仓约定 |

### 3.3 批 0 必须钉掉的覆盖面确认清单

- [ ] Runtime(In)VisibleAnnotations / ParameterAnnotations 读写闭环（mj2x 的
  `--native-annotation` 选择路径依赖方法注解读取）。
- [ ] BootstrapMethods 属性 + indy/condy 条目的 intern 与重映射（invokedynamic
  preprocessor 会新增 BSM 条目与常量）。
- [ ] StackMapTable 读回（frontend 是否消费 frames）与生成。
- [ ] MethodParameters / Signature / EnclosingMethod / InnerClasses 等元属性
  保真直通（lambda 合成类落点按 InnerClasses 真实 access 位判定——J-8 修复面）。
- [ ] CP 压缩（`ir::cp_compact`）对 mj2x 产物是否默认开（保真 vs 缩容的取舍，
  默认建议关，行为与旧栈对齐）。

## 四、迁移策略：阶段边界字节往返同步（免适配层）+ 七批推进

**理由**：85 文件一刀切不可回归；写 DOM→tree 或 tree→DOM 的模型适配层约
千行且引入第三份语义。定稿方案：**双表示共存 + 同步只走真实编解码器**
（tree → ClassWriter 字节 → `readClass` → DOM；反向 DOM → `writeClass` 字节
→ ClassReader → tree）。同步点钉在 pipeline 阶段边界，任一时刻每份表示的
新鲜度由「上一阶段谁在改」决定：

- 批 2（同步机制建立）：`CompilerClass` 增 DOM 载体；同步点 = 预处理后
  （tree→DOM，捕获预处理树改动）+ 编译后（tree→DOM，捕获 backend 注入）；
  输出仍走 tree。暂无 DOM 消费方，配 round-trip 校验测试。
- 批 3（读路径切换）：frontend（code_converter）+ backend `compileMethod`
  读路径 → DOM。树改动方（pre/inject/post）不变，既有同步点已覆盖。
- 批 4（预处理/后处理切换）：7 pre + 3 post 的改写与读侧 → DOM
  （CodeEditor/MethodEditor/成员增删 adopt）。同步点变化：预处理后无需同步
  （后续读方全在 DOM），编译后同步方向不变（inject 仍在树上改），后处理后
  增 DOM→tree（output 仍读树）。
- 批 5（backend 注入切换）：四后端 `injectIntoClass`（native 注册注入）→
  DOM；编译后同步点消失。
- 批 6（收口）：output_writer → `writeClass`（未触碰类保真直出）+ 摘 tree
  载体 + 摘 `libjbytecode` 依赖 + 测试件（test_helper/mtest_set/unittest
  夹具）收尾 + 文档收口。**保真收益在批 6 兑现**：未改写类字节原样直出。
  实际推进拆子批 6a/6d/6e/6f/6g（勾账见下）。
- 批 0/1 维持原案：批 0 = 包接线 + 覆盖面盘点（见
  [libjbc_migration_notes.md](libjbc_migration_notes.md)）；批 1 = type/opcode
  工具层（`jbc::type` 门面 + `Opcodes` → `jbc::core::Op` 别名，J-6 根治验证）。

每批：分支提交（单分支 `feat/libjbc-migration` 按批 commit）+ 全量回归门
（unittest + integration + java_corpus 三后端 + LLVM 列）。

### 批次勾账（按批回写；提交号 = 单分支 feat/libjbc-migration 上的批次 commit）

| 批次 | 内容 | 状态 | 提交 |
|---|---|---|---|
| 批 0 | libjbc 0.4.15 包接线 + 覆盖面盘点 | ✅ 完成 | 7ea4c96 |
| 批 1+2 | JvmType 描述符门面/Opcodes 镜像 + tree→DOM 阶段边界同步 | ✅ 完成 | 0923add + adb2b6f |
| 批 3a | MIR 常量载荷切 mj2x::Type 门面 + 同步 plain 化 | ✅ 完成 | 93eef88（修正 6e77aa8） |
| 批 3b | code_converter 读路径切 DOM | ✅ 完成 | 53cadb0 |
| 批 4a | pre/post 改写载体切 DOM（6 pass + 管线双载体拆段） | ✅ 完成 | 8bf3f79 |
| 批 4b | invokedynamic preprocessor 切 DOM（树载体收尾） | ✅ 完成 | aefc14b |
| 批 5 | 四后端 injectIntoClass 切 DOM，编译后同步点消失 | ✅ 完成 | fb80c26 |
| 批 6a | output 切 writeClass 直出 + modified 类帧重算垫片（全量回归 919 可跑 908 过 0 败） | ✅ 完成 | c2c9725 |
| 批 6d | 树载体退役：CompilerMethod 快照化 + 全消费方切 DOM | ✅ 完成 | 1037818 |
| 批 6e | Type/Handle 门面收口，产品面零 libjbytecode | ✅ 完成 | 805c330 |
| 批 6f | 测试件迁移（test_helper/unittest/mtest_set 夹具） | ✅ 完成 | 70d8c7d |
| 批 6g | 文档收口 + 全量回归终验（919 可跑 908 过 0 败，928 总量含 9 disabled） | ✅ 完成 | 本提交 |

注：批 6 子批号按实际推进取用（6b/6c 未产生独立提交），勾账只记实际发生的
批次。T1 小卡与终验收（§五/§六）不在本表范围。

### 风险与回退

- 双表示共存期批批可独立回退（单分支内按批 revert commit）。
- 批 4 是正确性核心批（native 标记、clinit 搬运、indy/ldc/multianewarray
  改写、loader 注入全部换编辑载体），验收 = 对应单测 + java_corpus 全矩阵 +
  interface/native-skip 契约测试。
- SMT/maxs 口径差异预案：libjbc 已按「公共前缀逐槽一致」对拍 javac 产物；
  若语料红，批 6 的 output 侧垫显式采用层（对齐旧 COMPUTE_FRAMES 行为）。
- vendor 纪律：test_helper/mtest_set 上游未迁 jbc 前，仓内副本先行本地迁移
  并在 MIGRATION.md 登记偏差，禁止顺手功能性新增。

## 五、功能完善流（与迁移并行/穿插，取自 dev_plan 既有待办）

### T1 迁移窗口内可穿插的小卡（互不触 frontend/改写面，可并行）

- [ ] dev_plan 账目校准 + BigSwitch / regfile 形态收编 E2E + 语料 obf 档
  `--vmc4-regfile` / `--regc-computed-jumps` 复用验证（J-12/13/15 修后动作）。
- [ ] finalize 错误通道统一（三后端失败行为不一致，G 段遗留）。
- [ ] RegC intrinsic 判定热点：decision 函数级一次算好 + registry 视图统一
  （analyze 走 builtins、emit 走用户 registry 的漂移风险）。
- [ ] 性能遗留三件：SSA 构建 O(n²)（fallthroughBlock 线性扫 /
  findBlockByLabel / reaching_def 无邻接表）、JAR 输出每条目全量拷贝、
  llvmir GlobalStringTable::intern 线性查。
- [ ] EnumSemCheck E2E 门禁力（caller-first 排序变体，J-3）。
- [ ] DEF_HEX 零注册死宏退役（J-7）。
- [ ] rt_protect Linux rwx 误报——**需 Linux 真机**，有环境时优先。

### T2 迁移完成后的大卡（按拍板顺序）

- [ ] H 剩余：indy 原生派发 P5（stackc F1/F2）/ P7（vmc4 编码前改写式）/
  P10（llvm 模块级缓存分派）→ 解锁 H-2 三后端共享层抽取（映射表 →
  RegisterNatives → indy 解析收尾 → 多维数组，四子批各自独立 PR + 全量回归）。
- [ ] K 批次 3（产品化，逐项需拍板）：fat JAR + 多 ABI jar 嵌入 + NativeLoader
  平台挑选（#11 后续切片）；Gradle 插件 + Java SDK SPI（issue #12）；packer
  （代码段流加密 + 两阶段 loader + 免落盘）+ 签名流水线（issue #13）；LTO；
  CFF LLVM 后端 inline-asm 屏障（K1 遗留）。
- [ ] D：配置系统 GUI 工具 P1 实现（vite+vue、schema 驱动表单 + 校验 + 导出；
  设计已完成）。
- [ ] A：VMC4 S2 对抗验证 + 变义强度模型（需 mjdbg 侧联动，开
  `feat/mj2x-vmc4-mutation`）。
- [ ] F：整类下沉单类试点（P3：无反射、无动态类加载、intrinsic 覆盖充分）。

## 六、测试完善流

- [ ] **B：自建测试集步骤 6——mjdbg 对抗闭环**（每用例 expected trace
  surface + weakness report，与 mjdbg 工具端联动；长期项，随各批渐进接入）。
- [ ] 「clang -O2 后 dispatcher 存活」做成自动化 CI 断言（现靠人工跑
  `work/cff_probe/`；配套 LLVM 后端 CFF inline-asm 屏障——K1 遗留一并做）。
- [ ] 产品决策 9：CI runner 安装 llvm/clang——unittest skip 地板 4→2，
  integration LLVM 列 28 例在 CI 转实跑。
- [ ] 迁移自带测试增量（§四 批 2/批 6）：round-trip 字节保真对拍门、
  maxs/SMT 与 javac 对拍门——迁移结束后长期驻留为回归门。
- [ ] 编排层 13：测试分层策略文档重写（计划期表格删表留方法学）。
- [ ] J-6 收官：迁移批 1 后 `IndyBootstrapPlanTest` 守卫摘除 + 用例保留为
  libjbc 边界回归。

## 七、执行纪律（贯穿全程）

- **工作流**：一律分支 + `gh pr create`，开完 PR 停手等用户合并，不自行合并
  （家族约定，mj2x PR #20/#33 同口径）。
- **三后端纪律**：跨 StackC/RegC/VMC4 改动三面都要 emit 形单测 + exec 级 E2E。
- **构建门**：信测试结果前 `rm <exe> && xmake -r <target>` delete-then-rebuild；
  构建与验证不并成一条命令。
- **提交**：约定式前缀 + 模块作用域（`feat(core)` / `fix(cli)` /
  `refactor(io)` / `chore(build)` / `docs`）；不提交测试产物；提交前
  `git status`。
- **多会话并行**：锁协议 v2（mkdir 抢锁 + owner.txt + 验 own 后释放）；
  `xmake build <target>` 一次一个 target。
- **账本**：本文件按批回写状态；dev_plan.md 只留与迁移无关的独立待办。

## 八、排期总览（依赖关系而非日历）

```text
批0 接线盘点 ──> 批1 type层 ──> 批2 io枢纽 ──> 批3 frontend ──> 批4 改写面 ──> 批5 残余+测试件 ──> 批6 摘除
   │              │             │
   │              └─ J-6 根治    └─ 保真门驻留
   └─ T1 小卡可从批0起任意穿插（互不触frontend/改写面）

批6 后 ──> T2 大卡（H 剩余 → H-2 抽取；K 批次3 逐项拍板；D GUI；A S2；F 试点）
测试流（B 对抗闭环 / CFF CI 断言 / CI llvm）全程并行，不占迁移关键路径
```

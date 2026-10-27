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

## 批 3a 实证发现（全量回归 169 败根因）

**现象**：批 3a 提交（93eef88）后全量回归 169 败，全部 E2E 面（StackC/RegC/VMC4/
MtestSet/BackendMatrix）报 `java.lang.VerifyError: Operand stack overflow`；
单测面全绿——单测不跑 JVM 验证，暴露不了。

**根因**：C++ 非限定名查找的静默改绑。批 3a 给 `mir_node.hpp` 加了
`jvm_type.hpp` include，`mj2x::Type` 顺 include 链（code_converter.hpp →
regc_backend.cpp 等）进入大量 TU。这些 TU 里原有的非限定
`Type::getObjectType(...)`（构造树侧 LDC 载荷）从 `jbytecode::core::Type`
（using-directive）**静默改绑**到 `mj2x::Type`——外层命名空间成员名优先于
using-directive 引入名，不构成二义性，编译器不报错。旧栈 writer 的
`visitLdcInsn` 按 Any 载荷类型分派，`mj2x::Type` 不匹配任何分支 →
`constantSymbol = nullptr` → **静默降级发射 ACONST_NULL**（连压栈宽度都不记），
clinit 的 `ldc int; ldc class; invokestatic` 退化成 `ldc int; aconst_null;
invokestatic`，maxStack 少 1 → VerifyError。

**修复**：树载荷构造点全部限定旧栈类型——regc_backend.cpp / vmc4_backend.cpp /
stackc_registration.cpp / ldc_preprocessor.cpp 共 6 处
`jbytecode::core::Type::getObjectType/getType(...)`。

**规则（批 4/5/6 沿用）**：
- 凡把对象**装箱进树**（LdcInsnNode::cst、bsmArgs、FrameNode 值、Any 载荷）的
  构造点，必须显式限定 `jbytecode::core::Type` / `jbytecode::core::Handle`，
  树契约在批 6 摘树前不变。
- 门面 Type 与旧栈 Type 的 Sort 数值逐位一致（VOID=0..OBJECT=10,METHOD=11,
  INTERNAL=12，getSort 双侧掩蔽 INTERNAL→OBJECT），旧对象上的
  `getSort() == Type::METHOD` 类比较侥幸安全，但**不得依赖侥幸**：消费旧对象
  的比较点同样限定 `jbytecode::core::Type::`。
- 旧栈 writer 的 LDC 未知载荷静默降级 ACONST_NULL 是行为面地雷：批 6 切
  writeClass 后此面消失；在那之前新代码给树喂载荷一律走限定名 + 单测兜底。
- 提交纪律：调试态（BISECT-A 注释、临时注释掉的调用）随 93eef88 误提交过，
  commit 前 `git diff --cached` 必查。

## 批 3b 实证发现

**1. Label 身份断言适配（DOM 合成标签）**。DOM 读路径的跳转目标是
LabelPool 按 pc 合成的 `Label`（`labelAt` 同 pc 同指针），与树侧
`LabelNode::label` 不是同一对象——垫片经字节回环后树标签指针必然丢失。
CodeConverterTest 的分支/switch 断言全部改为「目标锚定」：分支的
`targetLabel` 必须等价于某目标块内 MIRLabel 持有的同一指针
（`blockHasLabel` / `targetAnchored` 两个 lambda），指针等值断言不再
跨树/DOM 两侧成立。这同时把断言语义从"指针巧合"收紧为"图结构锚定"。

**2. `std::string` bsm arg 写出废索引 0**。旧栈 writer 的 bootstrap arg
分派（symbol_table.cpp `addBootstrapMethod`）只认 int/i64/float/double/
Utf8StringRef/Type/Handle/ConstantDynamic；`std::string` 落 else 分支
`argIndices.push_back(0)`，字节里写出 CP 索引 0。树路径不序列化所以
从不暴露；垫片字节回环后 readClass 忠实读回 0 → 消费端拿到废索引。
修法在测试件：bsm arg 一律给 writer 可序列化的载荷（`Utf8StringRef`
→ CONSTANT_String，恰是真实 javac 类的形态）。

**3. CP 占位槽/续槽的 tag 哨兵是 Utf8**。libjbc 常量池 slot 0 占位与
Long/Double 续槽的 tag 都记 `CpTag::Utf8` 但 `valid()==false`
（constant_pool.cpp `tagOf` 对无效索引同样返回 Utf8）。这会把"索引 0
的废 arg"伪装成"合法 Utf8 条目"——批 3b 首个症状
（`unsupported CP tag 1 in bootstrap arg`）就是这个哨兵骗出来的，
真因是发现 2。消费 CP 索引前先 `valid()`，不要只看 tag。

**4. resolveConstPayload 补 Utf8 分支**。防御性收下 CONSTANT_Utf8 形态
的 bsm arg（返回 Utf8StringRef 载荷，`classifyIndyBsmArg` 的 String
分支同收 std::string/Utf8StringRef 双型）。JVM 规范 bsm arg 应为可加载
常量，javac 不会写 Utf8，但外部类不必守。

**5. 陈旧 exe 教训再确认**。SwitchInstructions 一度"探针证明机制正确但
断言仍败"，实为中间构建的陈旧 exe 伪影——delete-then-rebuild 后
同构建内探针与断言一致。多轮探针调试时每轮结果必须与当轮 exe 的
构建时间戳对账。

**6. DOM CP 裸视图悬垂（本批最深的坑）**。resolveHandle/resolveConstPayload/LDC
Class/MethodType 载荷最初直接持 `cf->cp.utf8Ref(...)` 裸视图——DOM
ClassFile 是 convert 内的局部 unique_ptr，**convert 返回即析构**，MIR
里的 Handle/Type/字符串视图全部悬垂。症状漂移：INDY bsm desc 长度字段
被复用内存改写（115→107）抛 "truncated descriptor"、内容被发射期 IR
文本覆盖抛 "Invalid method descriptor: %v3 = call ..."。旧树路径无此
问题（树节点自持字符串）。修法：**凡从 DOM CP 解出的字符串载荷，一律
先 `arena->intern(...)` 再装箱**（Handle 三件、String/Class/MethodType/
Utf8 载荷、LDC Class/MethodType）。这条是批 4/5/6 的硬约束。

**7. 块 id 禁用指针截断**。最初 DOM 目标块 id = `labelKey(Label*)`
（指针截断 int）。Windows x64 堆地址超 4GB 时截断为负，translate 里
`labelKeyHere >= 0` 卫语句把负键误判为"非目标"→ 目标块拿合成 id、
MIRLabel 不发射、异常表回填失败——**随 ASLR 50/50 翻车**
（同一二进制两次运行结果不同）。修法：标签身份改顺序 id（labelAt 建
档时 `nextLabelId++`），块 id/异常表键/labelToInsnIdx 全走顺序 id；
指针身份只留在 MIR Label* 消费面（stackc labelToIndex、ssa 节点扫描，
双侧同源恒成立）。

**8. LLVM 异常边拷贝 dest 查表落空 → 跳过而非回退**。handler 块参数若
在块内未读即被重定义，RegIR DCE 后不进寄存器类型表，槽 alloca 也不存在；
异常边拷贝发射时 `registerTypes_.find(dest)` 落空。树路径因夹具双标签
怪癖恰好带出多余块参数掩盖了此面。曾试"回退用块参数自带 SSAType"——
产出 store 到未声明槽的非法 IR（clang 拒收 `use of undefined value`）。
定案修法：查表落空直接 `continue` 跳过该拷贝（拷贝本身无人读，跳过
无损），类型不受支持才 fail-closed。

**9. stackc 深度分析要求全块可达 + DOM 标签锚定**。computeBlockDepths
对 entryDepths 恒 -1 的块（不可达）直接 fail-closed（空文本 NONE）。
夹具 `[GOTO tail, RETURN, tail:]` 的 RETURN 块不可达即翻车。且 DOM 语义
下「空目标块」不存在：标签只在锚定真实指令时物化为目标块的 MIRLabel
首节点（RawInsn 序列无 Label 伪指令），MIRLabel 发射自带 `label:` +
空语句；树时代 `[RETURN, tailLabel]` 的尾部游离标签形状退役，等价钉义
收窄为 `[GOTO tail, tail:, RETURN]`（目标块以标签发射开场）。

## 批 4 实证（pre/post 改写载体切 DOM）

**10. 属性名槽是 CONSTANT_Utf8，不是 CONSTANT_String**。Code/LNT 等
AttrSlot.nameCp 必须指向 Utf8 条目；`ClassSession::internString` 是
ldc 语义（Utf8+String 两跳，返回 String 条目索引）——用它填属性名 =
writeClass Unencodable（CP 索引失效）。属性名走
`cp.findUtf8(name)` 命中既有池不膨胀、miss `cp.addUtf8(name)`（库的
intern 纪律配对 API）。指令操作数语义（LDC String/Class、invoke ref）
才用 session.internString/internClass/internMethodRef 家族。

**11. syncDomToTree：DOM 权威→树投影的状态迁移**。批 4 双载体期
pipeline 拆段：树载体 pass（indy，批 4b 收尾）先跑 → syncTreeToDom
（顺带为 lambda 合成类补 DOM）→ DOM 载体 pass → syncDomToTree →
compile（读 DOM）→ syncTreeToDom（inject 树改动落 DOM）→ post（DOM）→
syncDomToTree（output 读树）。syncDomToTree 重建 classNode 时
CompilerMethod 管线状态按 name+desc 锚定迁移；DOM 侧新增方法（__regc_*
搬运产物）以树空壳 MethodNode 影子进 cls.methods（methodNode 是
CompilerMethod 身份源，markAll 等 DOM 读侧 pass 依赖它锚定），空壳只活
到 syncDomToTree 重建为止。

**12. init_clinit_mover 的 DOM 形态走字节域转移**。CodeEditor 无同会话
方法体搬移（adoptMethod 是跨会话语义）；构造器切点的尾段控制流封闭
（守卫：骨架段无控制流 + 尾段跳转目标不出段 + 异常表空 + 无 LVT/LVTT），
段内跳转相对偏移平移不变——尾段字节原样转移 + 嵌套 LNT 条目按 startPc
归属拆分（尾段条目 startPc -= cutEnd）+ SMT 丢弃（output COMPUTE 重算）。
clinit 整体搬移 = CodeAttr 槽所有权转移（Decoded 槽写回 copy 源 span，
字节保真）。骨架 CodeAttr 手工编码（INVOKESTATIC u2 + RETURN）。

**13. 测试 TU 的 Opcodes 歧义**。批 4 后 dom_edit 传递引入 mj2x::Opcodes，
`using namespace mj2x + jbytecode::core` 双开的测试 TU 里裸 `Opcodes::`
变歧义——统一改 `jbytecode::core::Opcodes::` 限定（树语境测试的旧栈
口径）。另有 libca process/subprocess.hpp `#undef stdout/stderr` 的坑：
需要 stdout/stderr 的头不得排在它后面。

**14. AttrSlot 改 body 必须经 mutate() 置脏**。状态机：Untouched/Decoded
写回都 copy 源 span，只有 Modified/New 重编码（`needsReencode()`）。
`decodeAttr` 只置 Decoded——解码后裸改 body（如 LNT rows 拆分）而不过
`slot.mutate()`，改动被写回静默丢弃：e2e 实证 = mover 拆了 LNT 尾段
条目，writeClass 仍按源 span 写回原始三行，骨架 code 缩短后行 startPc
越界 → JVM ClassFormatError。纪律：**只读 peek（slot.body.get()），
要写必经 mutate()**；findLnt 类辅助函数返回 AttrSlot* 而非 body 指针，
把置脏责任显式化。

**15. CodeEditor.insertAtStart 连续前缀块要逆序插入**。insertAtStart
恒插位首、后插者在前——按最终序正序逐条插，产出恰好颠倒（e2e 实证 =
NativeLoader clinit 注入 emitLoadCallDom 把 invokestatic 插到 @0，操作
数栈下溢 VerifyError）。按最终程序序的逆序插入（call→suffix→path）。

**16. mover 的 SMT（Frame）守卫口径**。树版 hasForbiddenPrefix 的 FRAME
分支在字节域落为：骨架段 [0..cutEnd) 内锚定的帧 → 拒绝搬运；尾段锚定
的帧 → 随体丢弃（output COMPUTE 重算）。帧绝对 pc 链式口径同 mjbase
frame_rewire：首帧 offset 即绝对 pc，其后 offset_delta 经 smtAbsPc 链算。
实证：javac 对无分支方法**完全不发 SMT**（分支目标才需要帧）——真类
构造器经此守卫照常搬运；夹具手构 FrameNode 经树→DOM 往返后帧锚点回树
时 reader 会先落一个 LabelNode（帧引用 label），同 pc 多 label 合并单
锚点，裸 label（无 LNT 锚定/无跳转引用）在字节域无形态。

**17. JVMS reference_kind 1..9 是 DOM 世界的硬边界**。树时代夹具可以
手构 tag=999 的 Handle 走全管线（旧栈不校验）；DOM 同步的 readClass 按
JVMS 4.4.8 拒绝越界 reference_kind——「未知 tag 缺省分支」用例在 DOM
世界不可构建（indyHandleFindFor 的 default 分支对合法类文件不可达），
find4 钉表退役该用例、保留合法 9 tag 全表。

**18. CodeDom 插入路径强制 `srcPc = kNoPc`，元数据锚点要显式回填**。
appendSlot/insertAt 对传入 Insn 一律 `in.srcPc = kNoPc`（「合成指令没有源
位置，原指令 srcPc 只来自 importFrom」），先改 seq 再插入的写法是纯死写。
lower 的属性重定位 pcmap 以**存活指令的 srcPc** 为键（relocateLnt/Lvt/Smt
全部 fail-closed，miss 即 RelocationBadTarget）——erase 掉站点指令后，锚在
该 pc 上的 LNT/LVT/SMT 行全部脱靶：e2e 实证 = `IntUnaryOperator op =
InvokeDynamicLambda::base;` 的 LNT 行 line5:0 锚在 indy(pc 0)，展开后 lower
直接拒编译。正门修法：insertBefore 后经 **CodeDom::get(InsnId)**（公有可变
访问，指针稳定不因插入悬空）把替换序列**首条**的 srcPc 回填为被删指令的
srcPc——行号语义不变（替换首条即原语句位置），LVT 起点/SMT 锚点同一通道
命中。

**19. 旧栈 writer 对不识别的 bsm 参数 Any 载荷落 CP 0**。
SymbolTable::addBootstrapMethod 的分派链只认 int/i64/float/double/
Utf8StringRef/Type/Handle/ConstantDynamic，**std::string 落 `argIndices
push_back(0)`**。树时代夹具用 std::string 装 recipe 能活（载荷不落盘、
anyAsString 双吃两种表示）；DOM 世界树→字节→readClass 同步后即成坏索引
（`dom_payload: bad CP utf8 index 0`）。测试载荷一律 `arena.intern(...)`
（Utf8StringRef）。

**20.indy 站点替换的 e2e 通道是 mj2x-cli.exe**。e2e 夹具 runCase 经
mj2x-cli 子进程走管线，`xmake build mj2x_unittest` 不重链 cli——改了
mj2x-core 后只跑单测门会漏掉 cli 侧 stale 链接，症状 = 单测绿、e2e 仍报
旧行为。core 改动后 cli/unittest 两个 exe 都要重编再下结论。

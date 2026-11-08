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

**21. backend 注入切 DOM 后树投影由管线统一刷，混合态没有捷径**。
injectIntoClass 的「标 ACC_NATIVE + 清指令」DOM 对应物 = MemberInfo.access
置位 + 摘除 Code 属性槽（JVMS：native 方法无 Code）；clinit 前缀注入 =
promote + insertBefore(first) 同锚保序（insertAtStart 的对偶，无需逆序）。
管线序变为 pre→syncDomToTree→compile(inject 走 DOM)→post→syncDomToTree：
编译后 tree→DOM 同步点消失。**stackc 的 clinit 重写必须同批切 DOM**——
它重写 clinit 体（hidden clinit 代理），若留树上做，post 的 DOM 会话提交
经 syncDomToTree 会把树改动顶掉。hidden clinit 的「改名→编译→改回」借
编译期树投影（inject 只动 DOM，树此刻仍与 DOM 同源），改名不落任何权威。

**22. syncDomToTree 重建树会销毁旧 ClassNode，树方法节点按 tree 所有权
delete**（libjbytecode ClassNode 析构 delete fields/methods 堆对象）——
夹具把 unique_ptr<MethodNode> 挂进 classNode 后调用 refreshTreeFromDom，
旧 ClassNode 析构 delete 节点、夹具 unique_ptr 析构再 delete = 双删 AV
（0xc0000005，表象在测试体尾声）。测试纪律：**挂树即 release 所有权**
（stackc 夹具既有模式），release 后原 unique_ptr 不可再解引用，访问一律
经 cls.methods 现查 methodNode。

## 批 6a 实证（output 切 writeClass 直出 + modified 类帧重算垫片，c2c9725，2026-10-27）

**output 弃旧栈 ClassWriter，writeClass 直出**。writeClassBytes 改为：仅存的
树权威类（后处理器构建的生成类，Loader 等）先 `ensureDomFromTree` 落 DOM
（批 6 过渡面，6d 随同步器退役）；modified 类过帧重算垫片后 `writeClass`
直出；未修改类字节原样保留——批 0 盘点的保真收益（round-trip 恒等）在此
兑现，旧 tree writer 不可复现的 verifier 敏感 SMT/debug 元数据不再失真。

**帧重算垫片对齐旧 COMPUTE_FRAMES 口径**。`recomputeOutputFrames`（6d 提为
共享 frame_recompute）：全方法 `promoteMethod` → `FrameEngine(Strict).compute`
→ `applyComputedMaxes`（maxs 落位）→ `lower` → `buildStackMapTable`（v50+，
`ClassTooOld` 同地板跳过）。公共父类口径 = `PortedCommonSuperResolver`——
旧栈 `DefaultCommonSuperClassProvider` 硬编码父类链逐行移植（Byte..Double
→Number、Exception/Error→Throwable、RuntimeException→Exception、
String/Boolean/Character→Object，表外一律 Object 兜底）；mj2x 从未注入
typeResolver，旧 writer 的公共父类判定就是这张表，移植即同口径。Strict 档
不可算即抛（旧 writer 同口径：不可验证的类写不出来）。SMT 槽换装在 Code
嵌套属性表上：needed=false 不留空表（对齐旧 writer），已有槽删除、缺失槽
`AttrSlot::makeNew` 补挂（mutate 置脏纪律沿用发现 14）。abstract/native
（无 Code 槽）promote 即 Err，静默跳过——6e 在生成类构建器上撞到同一语义
（见 23）。

**pipeline 编译后 DOM→tree 同步点摘除**。output 改读 DOM 后 `syncDomToTree`
的最后一个消费方消失；同步序列化旗标恒 plain（COMPUTE 的帧计算自此是
output 侧语义，同步通道只求如实序列化当前 tree 状态——批 2 决策的终局形态）。

**回归基线**：全量回归 919 可跑项：908 过 / 0 败 / 11 工具链门控跳过 /
9 disabled（套件总量 928；早期勾账误记的 921 为总量笔误）。输出面首次
整体行走 libjbc writer 的全量基线。

## 批 6d 实证（树载体退役：CompilerMethod 快照化 + 全消费方切 DOM，1037818，2026-10-27）

**CompilerClass 快照模型**。`classNode`/`methodNode` 字段与 class_dom_sync
同步器整体摘除；CompilerMethod/Field 改为纯元数据快照（name/desc/access/
maxStack/maxLocals——Code 属性快照供 stackc C 发射与 SSA/llvm 消费）。方法
身份源 = name+desc（类内唯一键，与 DOM MemberInfo 对齐），指令/CP 一律经
`cls.dom` 读。`refreshMethodSnapshots` 在管线边界与 DOM 对齐：name+desc
锚定迁移管线旗标（markedForCompile/compiled/backendResult），DOM 侧新增
方法按默认状态落位、消失条目随快照摘除。input_loader 同步简化：`readClass`
是唯一解析通道，快照随建（树解析退役）。

**管线单段化**。runPreprocessors 的双载体拆段（树载体 pass → syncTreeToDom
→ DOM pass）退役，全部 preprocessor 单段顺序执行；管线收缩为
pre → refreshSnapshots → compile → post。

**MIR 标签 id 化**。MIRLabel/Branch/Switch 持 `int labelId`（CodeConverter
顺序发放，同 pc 恒同 id），`labelKey`/`mirLabelKey` 指针键与 labelPool 合成
Label 全删——批 3b 发现 7（指针截断 id 的 ASLR 翻车）的修复面就此收编为
唯一表示：标签身份全链（MIR 节点、块 id、异常表键、dumpMIR）统一顺序 id，
旧栈 `Label*` 产品面清零。stackc 异常表消费切 `Block::exceptionTable`
（CodeConverter 回填、标签 id 键）。

**backend 全面切快照/DOM**。stackc（hidden <clinit> 幻影方法 + 注册壳类）、
regc（内联缓存/枚举形态验证改 decodeInsns raw 匹配、native 壳类）、
llvmir/vmc4（快照签名）。

**生成类构建器 DOM 化**。cloader/native_loader 生成类从树 ClassNode 构建
改为从零建 ClassFile + ClassSession 构建器：`addMethod(MethodSeed)` →
`promote` → `CodeEditor.append` → `lower` → `writeClass`（与 indy lambda
合成类同一构建形态；方法体 javac 直译，全部直线体无分支，v52 校验无 SMT
依赖）。

**帧重算提为共享 frame_recompute**。6a 的 output 侧垫片提为 core 共享件
（`PortedCommonSuperResolver` + `recomputeSessionFrames`），cloader/
native_loader 生成类的 maxes 计算同入口消费。resolver 是权宜实现（硬编码
父类链 = 旧栈 provider 移植，行为对拍基线）：**mjbase 帧计算就绪后换装
mjbase 包实现，已拍板后置**，不阻塞本迁移收口。

**已知断裂（批 6f 已收口，70d8c7d）**：test_helper 三明治与 unittest 树夹具面
已迁 DOM（classFromTree/treeFromDom 五助手 + 追加式投影 shelf）。

## 批 6e 实证（Type/Handle 门面收口，产品面零 libjbytecode，805c330，2026-10-28）

**mj2x::Handle 门面补位**（jvm_type.hpp）。tag/owner/name/desc/itf 视图语义
+ operator==；视图寿命随 intern 进 Utf8StringArena 的底串（与 mj2x::Type 同
纪律），无默认构造（载荷随构造注入，旧栈契约）。dom_payload 的 Any 载荷
随之换型：Class/MethodType 走 `mj2x::Type`、MethodHandle 走 `mj2x::Handle`
——批 3a 规则「树/Any 载荷一律旧栈类型」随树载体退役翻转，Any 载荷一律
mj2x 自有类型，旧栈载荷类型清零。

**全仓 include/using 清扫 + 死面摘除**。jbytecode include/using 行全删
（diff 触达 41 文件：39 代码文件 + 2 xmake）；死面三处：code_converter 树
shim convert（测试垫片，树退役后测试件改喂字节——6f 面）、native_loader_call
树版 emitLoadCall、indy classifyIndySite 树包装（报告面统一走 payload 版）。
产品面（core/cli/rt）源码零残留，仅存 4 处注释性历史提及（jvm_opcodes/
block/indy_dispatch/reaching_def）。

**xmake 摘依赖**。mj2x-core 摘 libjbytecode 包（add_packages 剩 libca/
libjbc/libjstr/libjzip）；mj2x-cli 链接闭包摘 `libjbytecode_{util,commons,
analysis,tree,core}` 与 libgarena（libjbytecode 传递依赖）。产品面不再触碰
libjbytecode：仓内剩余消费方只有测试件（test_helper/unittest/mtest_set 夹具
+ 根 xmake.lua 的 add_requires，树建模用途长期保留，见批 6f 收口口径）。

**23. cloader 构建器对 native 无体方法 promote 返回 Err**。CLI 冒烟实证：
`registerNativesForClass` 是 native 桥接方法（无 Code 属性），`addMethod`
落位后 `promoteMethod` 对无 Code 方法返回 Err，构建器 throw runtime_error。
修复 = `MethodSeed.withCode == false` 时跳过 promote/append/lower（无体可
发，落位即收）。与 6a 帧垫片的「无 Code 槽跳过」同一语义两面：DOM 世界
无 Code 属性的方法没有可 promote 的体，任何 promote-emit 管线都要先问
withCode。

**验证现状**。CLI 冒烟（SsaSamples）通过（Pipeline done + class/c 产物落
盘）。批 6d/6e 未跑全量回归——unittest 面断裂（test_helper 的 class_dom_sync
引用、树夹具）是已知态，由批 6f 迁移收口后重跑全量作最终门。

## 批 6f 实证（test_helper 树投影器 + unittest 夹具迁 DOM，70d8c7d，2026-10-28）

**三明治 DOM 化**。test_helper_dom_sync 五助手：classFromTree（树 plain 序列化 →
jbc readClass 落 DOM + 快照）、treeFromDom（writeClass → 旧栈 reader 回投影断言树）、
findMethodNode、snapshotMethod、convertMethodNode（shim 类喂字节）；runWithDomSync =
缺 dom 抛错 + pass.process + refreshSnapshots。class_dom_sync_test 随树同步器退役删除。

**验证揪出的产品缺陷（随批修复，9016e99）**。(1) stackc 分支/switch 五发射点直传
MIR labelId，与批 6d 引入的 per-method labelIndex 错位，C 产物 goto 悬垂（MSVC
C2094）：带跳转的 StackC E2E 15 例 + mtest_set 7 例全灭，直落用例全绿恰好划出
病灶边界；修 = 统一 labelIndex.at。(2) native loader 前向分支 append 期目标未锚定，
jbc checkHandles 拒 InvalidHandle（IOTest.NativeLoader 3 例抛错）；修 = 
begin/endDeferredTargets 受控延后绑定态（design.md §34.3 契约路径），退出全量补验。

**夹具纪律两条**。(3) 树→字节序列化只认旧栈常量：门面 Handle/Type 喂树，
MethodHandle/MethodType 常量在 classFromTree 写类步死亡（ldc 12 例 0 hits；classify
的 BootstrapMethods 写坏报 bad CP utf8 0）——树建模常量回旧栈类型，门面只用于管线
读出侧载荷断言。(4) 投影 shelf 追加不销毁：同测试内二次 findMethodIn 会 move 覆盖
shelf 槽，先前方法指针悬垂成空指令列表（indy lambda 2 例假绿）——shelf 改 per-class
vector。(5) indy 站点 insertBefore 静默吞错改显式抛（部分插入即字节码损坏）。

**验证现状**。第 5 轮全量回归（强删重链 + 完整日志落盘 + 真实退出码）：919 可跑
908 过 / 0 败 / 11 工具链门控跳过 / 9 disabled（总量 928），exit 0，对齐批 6a 基线。
排障记录：第 3 轮 tail 管道吞掉 FAILED 块（860+11≠919 账不平暴露），第 4 轮落盘
重跑还原 48 项失败清单——读 gtest 汇总先核对 ran = 过 + 败 + 跳 恒等式再下结论。

## 批 6f 补记：CI 双门与 corpus 回归修复（d7d7f79..31a44b1，2026-10-29）

PR #34 首轮 CI 双挂揭出 unittest 之外的整层验证面：integration 套件（230 项，
含 java_corpus 语料 × 三后端差分对拍 + 真实 JVM 运行）从未本地跑过。修复五簇：

**移植性**。runWithDomSync 的 Pass& 临时绑定 MSVC 放行、GCC 整 TU 编译失败
——签名改转发引用。

**字节保真**。CP 内容是 modified UTF-8：arena.intern 校验对孤代理对返回空视图，
字符串常量整条变空（TextUnicode 首曝）——CP 来源一律 intern_raw，下游三个
to_std_string 消费点改原始字节。

**帧锚定**。DOM converter 丢了树 reader 的帧节点——converter 从 Code 嵌套 SMT
重建 MIRFrame（链式 delta 还原 pc；lower 的 relocation 已把 SMT 重锚到与
srcPc 同域），vmc4 边拷贝对死槽类型冲突合并（javac 槽位复用 + TOP 杀槽）
退回源操作数类型。

**DOM 编辑契约**。indy 站点可为分支目标（erase StillUsed）——replaceAllUses
先重定向；合成局部索引 > 0xFF 须 wide 前缀；replaceMethodBody 全量擦除后孤儿
标签槽失锚须显式销毁、旧 LNT/SMT 嵌套属性须随体丢弃；clinit 注入的硬编码
LDC 在大常量池类超 u8——loadConst 自动升 LDC_W。

**会话接管盲区**。openDomEdit move 语义下 cls.dom 为空——宿主包判定（6e）
与同类的 dom 读取在会话期间恒空：当前类一律经 session->backing() 读
（LambdasCapture：SAM 在本类，嵌套判定静默跳过 → 包私有 SAM 跨包
IllegalAccessError）。同类隐患面：任何「会话期间经 index 读本类 dom」的路径。

**终验（第 6 轮）**：unittest 919 可跑 908 过 / 0 败 exit 0；integration 230 项
225 过 / 5 工具链门控跳过 exit 0。读 gtest 输出的教训补一条：CA_LOGT 的 tag 未
注册时日志静默丢弃——插桩先用已知可打印的 tag 验证通路，再换业务 tag。

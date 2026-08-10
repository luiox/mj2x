# mj2x — dev_plan

> 本文档只记录未完成工作。已支持能力见 [doc_ai/capabilities.md](doc_ai/capabilities.md)，路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`../doc_ai/documentation_governance.md` §7)：任务完成后从本文件删除，并更新 CAPABILITIES。

## 后续待办

- [ ] **R5：mjdbg 闭环验证**：使用 mjdbg/c2j 验证 RegC 保护强度并产出量化结论。当前阻塞：仓库未提供 c2j CLI；本机 mjdbg protected-sample 与 native trace smoke 均在 adopt 后的 `trace start` CLI 请求超时。待 trace CLI 恢复并提供 c2j agent 后复验。
- [ ] **R6：RegC 覆盖率提升到接近 100%（仿 JNIC/native-obfuscator 全转策略）**。当前 `TEST.jar` 业务方法覆盖率从 82% 提升到 ~90%（IO/类加载/内部类黑名单已放开，详见 [regc_native_coverage_analysis.md](doc_ai/regc_native_coverage_analysis.md) §13）。仅剩并发组（有意保留）与 invokedynamic（R6.1）。
  - [x] **R6-A：IO 组放开**（2026-07-25）：移除 `java/io/InputStream`/`ByteArrayInputStream` 黑名单。附带修复 `_mj2x_regc_collect_args` 的数组参数 bug（`[X` 被当元素类型读，导致 `ByteArrayInputStream(byte[])` 构造错乱 crash）。e2e `IOGroupRoundTrip` 通过。详见覆盖率文档 §13。
  - [x] **R6-B：类加载组放开**（2026-07-25）：移除 `java/lang/ClassLoader` 黑名单。`loadClass(String)`/`getResourceAsStream(String)`/`getSystemClassLoader()` 在 native 帧端到端通过，引用管理吸收 Class/InputStream ref 返回。e2e `ClassLoaderGroupRoundTrip` 通过。
  - [x] **R6-C：内部类访问路径验证**（2026-07-25）：内部类 `$` skip 已随 RegisterNatives（PR #661）撤销，本批补端到端验证——非静态内部类 `this$0` 合成字段访问 + 调用外部类实例方法（`Outer.this.method()`）在 native 帧走通。e2e `InnerClassFieldAccessRoundTrip` 通过。附带修复实例方法分派的抽象 owner bug（见 §13）。
  - [x] **R6-D：并发组根因调研 → 决策保留**（2026-07-25）：实测 `Thread.sleep(long)` 在主线程 native 帧调用**不**抛 `IllegalMonitorStateException`（与 dev_plan 旧描述不符），仅是调用结果与 baseline 存在调度抖动差异。原描述的 IMSE 只在线程池工作线程场景成立（`ReentrantLock` monitor 与 native 帧交互）。**决策**：保留并发组黑名单（`Thread`/`java.util.concurrent`）——线程池/锁语义在 native 帧不可靠，放开风险高且收益低（并发 API 本就稀疏）。详情见 §13。
  - **R6.1：invokedynamic 支持**（分阶段，优先 pre-SSA expansion 而非照搬 StackC runtime bridge——RegC 缺 owner-class 使 bridge 成本高，详见 [regc_invokedynamic_strategy.md](doc_ai/regc_invokedynamic_strategy.md)）：
    - [x] **阶段1：string concat 展开**（2026-07-25）：`InvokeDynamicPreprocessor` 把 `StringConcatFactory.makeConcat[WithConstants]` 改写成 `new StringBuilder`/`append` 链/`toString` 字节码（栈参数先 xSTORE 到新局部槽，按 recipe 展开），RegC 已有 StringBuilder intrinsic 故转 native 无需 native 新代码、绕开 owner-class 障碍。e2e `InvokeDynamicStringConcatRoundTrip` + 单元 `InvokeDynamicPreprocessorTest.*` 通过。详见覆盖率文档 §14。
    - [x] **阶段2：无状态 + 捕获 lambda 展开**（2026-07-25）：`InvokeDynamicPreprocessor` 为每个 `LambdaMetafactory.metafactory` 站点合成实现 functional interface 的类（SAM `invokestatic` 转调 impl，impl 形参=captured++sam）。无状态=空 `<init>`；捕获=N 个 private final 字段 + 参数化 `<init>`（站点栈上捕获值先 xSTORE 暂存、再 new/dup/顺序 xLOAD/invokespecial），SAM 先 getfield 取回捕获值。私有目标提权避免 IllegalAccessError；`OutputWriter::isGeneratedRuntimeClass` 认 `mj2x/lambda/*` 前缀。阶段2 MVP 覆盖 impl tag==`H_INVOKESTATIC`、无装箱/桥接。e2e `InvokeDynamicStatelessLambdaRoundTrip`/`InvokeDynamicCapturingLambdaRoundTrip` + 单元 `ExpandsStatelessLambda`/`ExpandsCapturingLambda` 通过。详见覆盖率文档 §15。
    - [x] **阶段2c：实例 + 构造器方法引用**（2026-07-26）：`buildLambdaClassNode` 按 impl 的 method-handle tag 发射 SAM 体——`H_INVOKEVIRTUAL`/`H_INVOKEINTERFACE`（实例引用，receiver=fullArgs 首元：bound 来自捕获字段、unbound 来自 SAM 首参）、`H_INVOKESPECIAL`（私有实例引用，提权后按 INVOKEVIRTUAL 发射）、`H_NEWINVOKESPECIAL`（构造器引用，`NEW`/`DUP`/args/`INVOKESPECIAL <init>`/`ARETURN`）。`expandLambdaSite` 改为 tag 感知校验（实例 receiver-first、返回类别一致性守卫拒装箱/桥接），de-privatize 扩展为「用户类非 public 目标（含私有实例方法、包私有构造器）一律提权 public」。站点改写与 tag 无关，复用阶段2b 捕获逻辑。e2e `InvokeDynamicMethodRefRoundTrip`（String::length/charAt、base::concat、String::new）+ 单元 `ExpandsUnboundInstanceRef`/`ExpandsBoundInstanceRef`/`ExpandsInterfaceMethodRef`/`ExpandsConstructorRef`/`LeavesUnsupportedHandleUnexpanded` 通过。
    - [x] **阶段2d：装箱/桥接适配**（2026-07-26）：泛型 functional interface（`Function`/`Supplier`/`Predicate`/`ToIntFunction`…）类型擦除后 samMethodType 形参/返回为 Object 而 impl 是具体/基本类型，此前整类 fail-closed——这是 javac 产出 lambda 的多数形态。合成 SAM 体静态发射 LMF 运行期适配的可展开子集：引用收窄 `CHECKCAST`、引用→基本类型 `CHECKCAST 包装类`+`xxxValue()` 拆箱、基本类型→Object/包装类 `valueOf` 装箱、SAM void 返回 `POP/POP2` 丢弃 impl 返回值（`canAdaptValue`/`emitAdaptValue`/`canAdaptReturn`/`emitReturnAdapt`）。基本类型互转（i2l 加宽等，需读 instantiatedMethodType）与非 Object/包装类装箱目标仍 fail-closed。栈峰值按适配两侧较大槽宽计，并修复零参 SAM 调 `()J` impl 时 maxStack 低估的边界。e2e `InvokeDynamicErasedGenericRoundTrip`（Math::abs 拆装箱、String::length/isEmpty receiver cast、String::valueOf(J) 拆箱、String::new Supplier、捕获+擦除 lambda）+ 单元 `ExpandsErasedStaticRefWithBoxing`/`ExpandsErasedConsumerDroppingReturn`/`ExpandsErasedInstanceRefReceiverCast`/**负例** `LeavesPrimitiveWideningUnexpanded` 通过。altMetafactory 留后续。
    - [x] **阶段2e：altMetafactory serializable/marker/bridge**（2026-07-26）：`javac` 为 serializable lambda、交集类型 marker 接口或泛型桥接发 `altMetafactory`（非 `metafactory`），bsmArgs 尾部按 `flags`（`FLAG_SERIALIZABLE=1`/`MARKERS=2`/`BRIDGES=4`）跟 markerCount+Class markers、bridgeCount+MethodType bridges。`isLambdaMetafactorySite` 认 altMetafactory；`parseAltMetafactoryTail` 解析为 `AltLambdaInfo{markerIfaces, bridgeDescs}`（未知 flag 位/形态异常 fail-closed）。`buildLambdaClassNode` 追加 marker 接口（含 `java/io/Serializable`，靠默认序列化 round-trip capN 字段，不复刻原生 SerializedLambda）到 interfaces，SAM 体发射抽成 `buildSam` lambda 并为每个 bridge 发 `ACC_BRIDGE|ACC_SYNTHETIC` 同名方法；校验抽成 `canEmitSam`（主 SAM + 各 bridge 逐个校验）。e2e `InvokeDynamicSerializableLambdaRoundTrip`（真实 writeObject→readObject→invoke，无状态 + 捕获两形态）+ 单元 `ExpandsAltMetafactorySerializable`/`ExpandsAltMetafactoryMarkerInterface`/`ExpandsAltMetafactoryWithBridge`/**负例** `LeavesUnknownAltFlagsUnexpanded` 通过。详见覆盖率文档 §18。
    - [x] **阶段2f：基本类型加宽桥接**（2026-07-26）：method reference 实参/返回基本类型与 SAM 相差一个 JLS §5.1.2 加宽（int→long/float/double、long→float/double、float→double）时 LMF 运行期插入 `i2l`/`i2d`/`l2d`/`f2d` 等。新增 `wideningOpcode`，`canAdaptValue` 对基本类型互转仅放行加宽、`emitAdaptValue` 在 primitive→primitive 分支发对应转换；实参段与返回段复用同一适配。收窄、装箱+加宽复合适配（需读 instantiatedMethodType）仍 fail-closed。e2e `InvokeDynamicPrimitiveWideningRoundTrip`（i2l/i2d/l2d/f2d 返回 + i2l 实参）+ 单元 `ExpandsPrimitiveWidening`/**负例** `LeavesPrimitiveNarrowingUnexpanded` 通过。详见覆盖率文档 §19。
    - [x] **阶段3：自定义 bootstrap 运行时 call-site helper**（2026-08-09）：SSA 层对无法静态展开的 invokedynamic（非 LambdaMetafactory/StringConcatFactory）登记站点元数据并降级为 `Intrinsic("indy:<index>")`；RegIR/emitter 透传；finalize 为每站点生成文件级 `_mj2x_regc_indy_bsm_<i>(owner)` bootstrap 函数（MethodHandles.lookup + privateLookupIn 回退、MethodType.fromMethodDescriptorString、bsm 静态参数重建（String/Class/MethodType/Handle find* 构造/装箱原语/varargs Object[]）、CallSite.getTarget 全局引用缓存），方法体收集参数装箱 Object[] 后 invokeWithArguments 调用并拆箱回寄存器。owner-class 经 trampoline 存入 thread_local `_mj2x_regc_owner_class`。验证：ASM 生成 invokedynamic 字节码两用例——`(I)I` 无 bsmArgs 与 `(String,I)String` + Handle bsmArg——转换后 INDY_RESULT=6 / INDY2_RESULT=hi:42 全 PASS；TEST.jar 17 项与 snake 回归无破坏。仍 fail-closed 的次要形态：装箱+加宽复合适配。
  - [x] **R6.2：native 库打包/加密技术 wiki**（草稿，知识储备）：已整理 [native_library_packaging.md](doc_ai/native_library_packaging.md)。具体解密算法待实现时逆向补充。

- [x] **VMC4 Phase 0+1：虚拟机后端 MVP**（2026-08-10）：从零搭建 Themida 风格可变多 VM 后端骨架。设计见 [vmc4_multi_vm_design.md](doc_ai/vmc4_multi_vm_design.md)。
  - **Phase 0（单 VM 管线）**：RegIR → VmEncoder → opcode 字节流 → VmInterpreterEmitter → C 解释器，端到端管线跑通纯算术 + 循环方法。前端链路（L1→SSA→RegIR→DCE→transform）与 RegC 共享，分叉点在 RegIR 之后。产物仍是 ACC_NATIVE + RegisterNatives 绑定，不引入新 class 注入机制。
  - **Phase 1（5 种 dispatcher）**：SwitchTable / FuncPtrTable / ComputedGoto（GCC 扩展）/ IfNest / StateDispatcher 全部真实落地（非占位 TODO），共享 handler 逻辑片段、外层调度结构各异。
  - **MVP 支持范围**：Copy/Unary/Binary/Compare/Convert + Jump/Branch/Return；I32/I64/F32/F64；JVM 整数环绕/除零/INT_MIN/-1/UShr 语义精确复刻。LoadField/Call/Allocate/Intrinsic 等对象/数组/调用 family 整体 fail-closed。
  - **接入**：CLI `--backend vmc4` + `--vmc4-dispatcher <kind>` + `--vmc4-seed <hex>`；新建 `mj2x-core/src/mj2x/backend/vmc4/`（6 文件），`xmake.lua` glob 自动纳入无需改构建脚本。
  - **验证**：`vmc4_backend_test.cpp` 10/10 通过（编码成功/fail-closed/解释器源码结构/5 种 dispatcher 特征）；RegC/StackC 单测无回归。

- [x] **VMC4 E2E 闭环 + 正确性修复**（2026-08-10）：首个 e2e 测试（`vmc4_backend_e2e_test.cpp`，3 用例 × 4 MSVC 可编译 dispatcher = 12 项，ComputedGoto 需 GCC 排除）把生成的 C **真编译成 DLL 并 JNI 调用**，基线/保护双跑对比。立即暴露并修复一批 Phase 0+1 单测从未验证的真实缺陷：
  - `encodeCopy` 读 `ins.value` 而非 `ins.copy.source`（Copy 指令源丢失 → 含常量/分支的方法全 fail-closed）
  - 边拷贝（SSA 块参数 RegCopy）被编码器静默丢弃 → 循环/汇合块读未初始化寄存器；新增合成块机制（每条带拷贝的边生成 [Copy...]+Jump 块，branch T/F 边分别解析）
  - FuncPtrTable 的 dispatch body 是函数内嵌函数（C 语法非法，MSVC 编译失败）→ ctx struct/typedef/body 移到文件级
  - FuncPtrTable 循环退出判断读 operand 末字节（RETURN 后永不退出/越界）→ 记录 opcode 位置判断
  - StateDispatcher 残留 `code[pc-1]==0x22` 死代码（operand 恰为 0x22 时提前退出）→ 删除
  - VMC4 路径误跑 `applyRegTransformPasses`（RegC 发射期 MBA/CFF 混淆；CFF 注入 switch dispatcher VMC4 无法编码）→ 移除，仅保留共享前端的 DCE
  - 空常量池 `= {}` MSVC 不可编译 → `{0}`；异常区域无编码 → encode 入口 fail-closed
  - 验证：`xmake run mj2x_unittest --gtest_filter="Vmc4*"` 26/26 通过（含 12 项 e2e：纯计算+控制流/浮点+饱和转换/fail-closed 混合共存），RegC/StackC 无回归。
  - **后续待办**：Phase 2（opcode 映射随机化 + seed 驱动 + per-class 粒度）、Phase 3（ALT 别名/super-op/bogus handler/per-method 独立源码）、Phase 4（mjdbg 红队闭环）、e2e 扩展（ComputedGoto 需 GCC 工具链；除零语义后续 Phase 补 ThrowNew）。

- [x] **VMC4 Phase 2：opcode 映射随机化 + 实例粒度**（2026-08-10）：「多 VM 实例」核心价值首次落地。
  - `VmInstance` 支持集（Halt+8 计算/控制流 opcode）经 splitmix64 Fisher-Yates 洗牌做双射映射（`opcodeTable`）；8 个 handler 独立洗牌（`handlerOrder`），决定 switch case / 函数指针表项 / if 链排列。
  - 实例 seed = `vmc4DeriveInstanceSeed(mainSeed, fnv1a64(identity))`：per-class 按 className、per-method 按 className.method+desc 缓存/派生，确定性可复现（同 seed+同输入 → 同产物）。
  - 编码器/发射器两侧消费同一映射：编码写实例字节，解释器 case 常量/Halt 哨兵/FuncPtrTable 的 RETURN 退出判断用实例字节。
  - CLI：`--vmc4-granularity <class|method>`；`--vmc4-seed` 起真正驱动映射。
  - 验证：`Vmc4*` 32/32 通过（新增 P2-1..6：可复现性/双射/编码字节/解释器 case/seed 派生/跨类实例独立）；12 项 e2e 在随机化映射下全部通过；RegC/StackC/Cli 106/106 无回归。
  - **后续待办**：Phase 3（ALT 别名/super-op/bogus handler/handler 形态变形/per-method 独立解释器源码）、Phase 4（mjdbg 红队闭环）、e2e 扩展（ComputedGoto、除零 ThrowNew）。

- [x] **VMC4 Phase 3：完整可变性维度**（2026-08-10）：设计 §4.5 可变性矩阵剩余主项全部落地。
  - **ALT 别名**：每语义 canonical + aliasCount（0..3）个别名字节，全局双射、集合互斥；编码器按指令 nonce 确定性选字节（`pickOpcodeByte`），解释器为同语义全部字节发射共享 handler（switch case / 函数指针表项 / goto 表槽 / if 链 OR 条件）。
  - **super-operator**：`Binary(x,1,Add/Sub)` 折叠为 SuperInc/SuperDec（无 rhs operand）；新增 opcode 参与映射/别名/handlerOrder。
  - **bogus handler**：每方法注入 0..4 个文件级假 handler（seed 确定性假体），dispatch 结构不引用——只污染静态分析、运行时永不命中。
  - **handler 形态**：Unary 位取反与 Compare Eq/Ne 双形态（`~x` vs `x^全1`、`a==b` vs `!(a!=b)`），`usesAltHandlerForm` 按 seed 确定性选择。
  - CLI：`--vmc4-alias <0..3>`、`--vmc4-bogus <0..4>`。
  - 验证：`Vmc4*` 50/50 通过（新增 P3-1..8）；12 项 e2e 在别名+bogus+super-op+morphing 全开下全部通过（可变性叠加不破坏语义）；RegC/StackC/Cli 124/124 无回归。
  - **后续待办**：Phase 4（mjdbg 红队闭环 + expected trace surface + 强度量化）、e2e 扩展（ComputedGoto 需 GCC 工具链；除零 ThrowNew）。

- [x] **VMC4 Phase 4：红队闭环样本侧契约**（2026-08-10）：mjdbg 自动还原工具属 mjdbg 产品线，mj2x 侧闭环前置全部交付。
  - `VmInstance::describe()` 确定性生成 expected trace surface（dispatcher/instanceSeed/aliasCount/bogusHandlers/每语义字节集合/handlerOrder/alt-form 标记）；CLI `--vmc4-trace-surface <file>` 写交付样本随附文件。
  - 契约文档 `doc_ai/vmc4_red_team_contract.md`：观测面表、强度归零基准（ISA 全恢复/跨实例可复用/无 seed 敏感性）、跨实例分析复用性测试方法、opcode 编码熵、trace-surface 一致性核对。
  - 验证：`Vmc4*` 51/51（新增 P4-1 trace surface 确定性/内容/跨实例差异）；RegC/StackC/Cli 无回归。
  - **后续待办**：mjdbg VM 分析 agent（按契约落地）、e2e 扩展（ComputedGoto 需 GCC 工具链；除零 ThrowNew）、对象 family 支持（新阶段，需 VM 对象模型/引用管理/异常语义）。

- [x] **VMC4 Phase 5：执行期 dispatch drift 低风险档**（2026-08-10，issue #762）：竞品共识方向的落地。
  - 语义标签轮转：字节集合固定、集合→语义标签按 `(pc / driftPeriod) % D` 周期轮换；编码器与解释器共用同一偏移公式天然同步（跳转不影响）；解释器 D 个 unmap 表（逆排列）解回 canonical 后分发，case 精简为每语义一个，语义绝对不变。
  - CLI `--vmc4-drift <0..4>` / `--vmc4-drift-period <bytes>`（默认 16）；trace surface 新增 drift 字段。
  - 验证：单测 7 项（round-trip/周期公式/变体差异/emit 结构/流解码）+ 12 项 e2e 在 drift 全开下真编译通过；`Vmc4*` 58/58，RegC/StackC 无回归。
  - **后续待办（#762 升级档）**：period 语义变义（对齐 jvmtp dispatch-mutation / JShield Tesseract），红队契约升级「变义强度模型」待 mjdbg 就绪；嵌套 VM context（#759 Abyss 方向）为后期阶段。

## 下一步验证命令

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter=CliTest.*
xmake run mj2x_unittest --gtest_filter=RegCCompareIntrinsicTest.*
xmake run mj2x_integration_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

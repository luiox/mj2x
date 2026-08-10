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
  - **后续待办**：Phase 2（opcode 映射随机化 + seed 驱动 + per-class 粒度）、Phase 3（ALT 别名/super-op/bogus handler/per-method 独立源码）、e2e 测试（`vmc4_backend_e2e_test.cpp`，需 JDK+MSVC 工具链）、Phase 4（mjdbg 红队闭环）。

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

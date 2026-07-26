# RegC Invokedynamic Strategy

## Purpose

`invokedynamic` is intentionally not expanded in this RegC/R3 runtime ABI
slice.  The current safe behavior is fail-closed: the preprocessor detects indy
sites, `CodeConverter` preserves a `MIRInvokeDynamic` marker, and SSA rejects it
with a clear reason instead of silently emitting incorrect native code.

This document records the intended expansion order so the next implementation
does not blur three different cases: Java string concatenation, Java lambdas,
and obfuscator-specific bootstrap methods.

## Current Boundary

Current behavior:

- `InvokeDynamicPreprocessor` **expands** StringConcatFactory sites into
  StringBuilder bytecode (阶段1) and LambdaMetafactory sites into a synthesized
  functional-interface class — stateless + capturing lambdas via `H_INVOKESTATIC`
  impl (阶段2/2b), instance (`H_INVOKEVIRTUAL`/`H_INVOKEINTERFACE`/
  `H_INVOKESPECIAL`) and constructor (`H_NEWINVOKESPECIAL`) method references
  (阶段2c), and erased-generic boxing/bridge shapes where samMethodType≠implDesc
  (阶段2d：CHECKCAST / box / unbox / drop-return adaptation)；其余 indy 仅诊断、
  不改写。
- SSA rejects still-unexpanded indy (primitive-widening bridge, altMetafactory,
  or custom bootstrap) with `invokedynamic not yet expanded`.
- StackC has a separate runtime bridge that invokes the bootstrap method,
  reads `CallSite.getTarget()`, and executes `MethodHandle.invokeWithArguments`.
- RegC should not mark a method native when indy lowering fails.

This is the correct RegC boundary until each shape has deterministic tests.
StackC's runtime bridge is a correctness path, not a RegC no-stack lowering —
RegC prefers pre-SSA expansion (阶段1 string concat 已落地此路线).

## Expansion Order

1. String concat first. **[DONE — R6.1 阶段1，2026-07-25]**

   Support `java/lang/invoke/StringConcatFactory.makeConcatWithConstants` (and
   `makeConcat`) for javac-generated concatenation.  Expand it before SSA into
   ordinary `StringBuilder`-style bytecode.  This is the lowest-risk useful shape
   because the recipe and constants are visible at compile time.

   **落地**：`InvokeDynamicPreprocessor`（`preprocessor/invokedynamic_preprocessor.cpp`）
   不再只诊断，而是把 concat 站点改写成 `new StringBuilder / append 链 / toString`
   字节码。栈上已求值的 N 个动态实参先 `xSTORE` 到新分配的局部槽（bump `maxLocals`），
   再按 recipe（``=动态参数、``=常量、其余=字面量）从左到右 `append`。
   单块直线代码、不引入分支/label，StackMapTable 帧不受影响。RegC 已有 StringBuilder
   intrinsic，故转 native 无需任何 native 新代码，**完全绕开 RegC 缺 owner-class 的障碍**
   （不走 StackC 的 runtime bridge）。测试：e2e `RegCBackendE2E.InvokeDynamicStringConcatRoundTrip`
   + 单元 `InvokeDynamicPreprocessorTest.*`（展开断言 + lambda 负例仍 fail-closed）。

2. Stateless + capturing lambda factories. **[DONE — R6.1 阶段2/2b，2026-07-25]**

   Support `java/lang/invoke/LambdaMetafactory.metafactory` when the target method
   handle and functional interface signature resolve locally. Both stateless
   (method reference, no capture) and capturing lambdas are handled.

   **落地**：`InvokeDynamicPreprocessor` 为每个 lambda 站点**合成一个实现 functional
   interface 的类**（照 `CLoaderPostprocessor` 套路）。SAM 方法（名=indy.name、
   desc=samMethodType）以 `invokestatic` 转调 implMethodHandle 目标；impl 形参布局
   （javac）= capturedTypes ++ samArgs。
   - **无状态**（indy desc 无参）：空 `<init>()V`，站点改写为 `new / dup / invokespecial <init>()V`。
   - **捕获**（indy desc 带 N 个捕获参数）：合成类加 N 个 `private final` 字段 capI +
     参数化 `<init>(capturedDescs)V`（super 后逐个 `aload_0 / xload / putfield`）；SAM 方法先
     `aload_0 / getfield` 取回捕获字段、再接 SAM 参数一起 invokestatic。站点：栈上 N 个捕获值
     先逆序 `xSTORE` 到新局部槽（bump `maxLocals`），再 `new / dup / 顺序 xLOAD / invokespecial
     <init>(capturedDescs)V`。
   合成类单类输出走 `addClass`(sidecar)、JAR 走 `upsertJarResource`，`isLibrary=true` 使 RegC
   跳过编译。**关键正确性**：(1) 目标私有方法提权（去 `ACC_PRIVATE` 置 public），否则跨类
   `invokestatic` 抛 `IllegalAccessError`；(2) `OutputWriter::isGeneratedRuntimeClass` 须认
   `mj2x/lambda/*` 前缀，否则合成类会被误当 primary（且 sidecar 路径须相对 classpath 根）。
   展开后走 RegC 通用 new/invoke 路径转 native，**绕开 owner-class 障碍**（不走 StackC bridge）。
   阶段2/2b MVP 约束：`metafactory`（非 altMetafactory）+ impl tag==`H_INVOKESTATIC` +
   impl 形参==capturedTypes++samArgs（无装箱/桥接）。测试：e2e
   `InvokeDynamicStatelessLambdaRoundTrip` / `InvokeDynamicCapturingLambdaRoundTrip`（单/多/
   混合类型捕获）+ 单元 `ExpandsStatelessLambda` / `ExpandsCapturingLambda`（含提权、捕获字段断言）。

2c. Instance + constructor method references. **[DONE — R6.1 阶段2c，2026-07-26]**

   把阶段2 的 `H_INVOKESTATIC` 限制推广到全部方法引用形态。关键洞察：**站点改写与 impl tag
   无关**（捕获的是 indy 描述符参数，与调用形态正交），只有合成类 SAM 体的发射指令和校验分支
   随 tag 变化。`buildLambdaClassNode` 按 `impl.getTag()` 发射：
   - `H_INVOKEVIRTUAL` / `H_INVOKEINTERFACE`（实例引用）：receiver = fullArgs（=capturedTypes
     ++ samArgs）首元——**bound** 时来自捕获字段（GETFIELD cap0）、**unbound** 时来自 SAM 首参
     （xLOAD 1）。因 receiver 天然是首个压栈项，压栈序与静态调用完全一致，只是把 `INVOKESTATIC`
     换成 `INVOKEVIRTUAL`/`INVOKEINTERFACE`。
   - `H_INVOKESPECIAL`（私有实例方法引用）：目标提权 public 后按 `INVOKEVIRTUAL` 发射（跨类
     `INVOKESPECIAL` 非 `<init>`/super 非法）。super 方法引用（库超类目标、无法提权）仍 fail-closed。
   - `H_NEWINVOKESPECIAL`（构造器引用）：`NEW owner` / `DUP` / fullArgs / `INVOKESPECIAL owner.<init>` /
     `ARETURN`（返回新建对象）。
   校验（`expandLambdaSite`）改为 tag 感知：静态/构造器要求 fullArgs 逐一匹配 implArgs；实例要求
   fullArgs[0] 为对象/数组 receiver、其余匹配 implArgs；静态/实例额外加**返回类别一致性守卫**
   （`returnOpcode(samRet)!=returnOpcode(implRet)` 即拒，挡住装箱/加宽的桥接形态）。de-privatize
   推广为「用户类非 public 目标一律提权 public」（合成类在 `mj2x/lambda/` 包，跨包访问 private/
   protected/包私有目标——含私有实例方法、包私有构造器——都会 `IllegalAccessError`）。测试：e2e
   `InvokeDynamicMethodRefRoundTrip`（`String::length`/`String::charAt` unbound、`base::concat`
   bound、`String::new` 构造器；functional interface 须 public 否则合成类跨包实现抛
   `IllegalAccessError`）+ 单元 `ExpandsUnboundInstanceRef`/`ExpandsBoundInstanceRef`/
   `ExpandsInterfaceMethodRef`/`ExpandsConstructorRef`/`LeavesUnsupportedHandleUnexpanded`
   （`H_GETFIELD` 字段句柄仍 fail-closed）。未覆盖：altMetafactory（serializable/marker
   interface）；装箱/桥接见阶段2d。

2d. Erased-generic boxing/bridge adaptation. **[DONE — R6.1 阶段2d，2026-07-26]**

   泛型 functional interface（`Function`/`Supplier`/`Consumer`/`Predicate`/`ToIntFunction`…）
   类型擦除后 samMethodType 形参/返回是 `Object`，与 impl 的具体引用/基本类型不同；
   LambdaMetafactory 在运行期做的适配是 javac 产出 lambda 的**多数形态**，此前整类 fail-closed。

   **落地**：合成 SAM 体在每个值压栈后静态发射适配字节码（`canAdaptValue`/`emitAdaptValue`），
   返回经 `canAdaptReturn`/`emitReturnAdapt`：
   - 引用收窄：`CHECKCAST`（目标 Object 免 cast）——覆盖 unbound receiver（`Object→String`）、
     擦除参数、擦除返回。
   - 引用→基本类型：`CHECKCAST 包装类`（已是包装类免）+ `xxxValue()` 拆箱。
   - 基本类型→引用：`包装类.valueOf` 装箱；目标限 `Object`/对应包装类（javac 擦除只产出
     这两种），其余 fail-closed。
   - SAM 返回 void：`POP`/`POP2` 丢弃 impl 返回值（Consumer 引用有返回值方法的形态）。
   - **不支持**：基本类型互转（`i2l` 加宽等，需读 instantiatedMethodType 才能定语义）、
     impl 返回 void 而 SAM 要值——均保持 fail-closed。
   校验从逐位描述符相等改为逐位 `canAdaptValue`（同型仍是快速路径），捕获段与 SAM 参数段
   统一走适配。栈峰值按适配两侧较大槽宽累计（`Object→long` 会把 1 槽变 2 槽），并修复
   零参 SAM 调 `()J` impl 时 maxStack 低估为 1 的边界。测试：e2e
   `InvokeDynamicErasedGenericRoundTrip`（`Math::abs` 拆装箱、`String::length`/`String::isEmpty`
   receiver cast、`String::valueOf(long)` 拆箱、`String::new` Supplier、捕获+擦除 lambda，
   protected native 与 Java baseline 逐行一致）+ 单元 `ExpandsErasedStaticRefWithBoxing`/
   `ExpandsErasedConsumerDroppingReturn`/`ExpandsErasedInstanceRefReceiverCast`/**负例**
   `LeavesPrimitiveWideningUnexpanded`。未覆盖：altMetafactory（serializable/marker interface）。

3. Obfuscator bootstrap methods last.

   For custom bootstrap methods, do not try to execute arbitrary bootstrap code
   during mj2x compilation.  Prefer either a documented fail-closed diagnostic
   or a later protected runtime call-site helper designed with mjdbg feedback.

## Tests Required Before Enabling

Each supported indy shape needs:

- a committed Java source sample under `mj2x/test_resource/`
- an SSA/RegIR test showing the indy marker is gone before lowering
- a RegC native E2E comparison against the original Java behavior
- a negative test proving unsupported bootstrap shapes still fail closed

Until those gates exist, `invokedynamic` remains outside the RegC accepted
surface.

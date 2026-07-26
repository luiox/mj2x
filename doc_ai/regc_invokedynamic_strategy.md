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
  StringBuilder bytecode (阶段1) and LambdaMetafactory sites (stateless +
  capturing) into a synthesized functional-interface class (阶段2/2b)；其余 indy
  仅诊断、不改写。
- SSA rejects still-unexpanded indy (non-INVOKESTATIC-impl lambda / custom
  bootstrap) with `invokedynamic not yet expanded`.
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
   MVP 约束：`metafactory`（非 altMetafactory）+ impl tag==`H_INVOKESTATIC` +
   impl 形参==capturedTypes++samArgs（无装箱/桥接）。测试：e2e
   `InvokeDynamicStatelessLambdaRoundTrip` / `InvokeDynamicCapturingLambdaRoundTrip`（单/多/
   混合类型捕获）+ 单元 `ExpandsStatelessLambda` / `ExpandsCapturingLambda`（含提权、捕获字段
   断言）/ `LeavesNonStaticImplUnexpanded`（非 INVOKESTATIC impl 仍 fail-closed）。

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

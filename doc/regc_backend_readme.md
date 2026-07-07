# RegC Backend README

## 定位

RegC 是 mj2x 当前的保护导向 native 后端。它不直接输出 JVM operand stack 模板，而是走：

```text
L1 stack MIR -> SSA -> RegIR -> RegC -> JNI wrapper / R3 runtime helpers
```

目标是让生成 C 呈现 virtual-register、block label、edge copy、helper/runtime call 的结构，
逐步减少 c2j 对 `cstack[]`、`sp`、全局 method/class table 等固定形状的依赖。

当前 RegC 已达到内部 MVP：可通过 CLI 生成 RegC C 源，能对一组 javac 常见形状做 native
DLL E2E 行为对比，并已把 integer div/rem/barrier 迁到 R3 runtime ABI。

## CLI 用法

```powershell
xmake build mj2x-cli
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c --backend regc
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c --backend regc
```

RegC 后端是 fail-closed：某个方法无法完成 SSA/RegIR/RegC lowering 时，不会把该方法标记为
native。JAR 输入仍沿用 mj2x pipeline 的整包读写能力；signed-JAR metadata、空 JAR、
无 root class JAR 会在输入阶段 fail-closed。

## 支持矩阵

| 能力 | 状态 | 验证入口 |
|------|------|----------|
| 纯计算、虚拟寄存器输出、无 stack anchor | 支持 | `RegCTest.EmitsPureArithmeticWithoutStackMachineAnchors` |
| 分支、循环、edge copy、switch | 支持 | `RegCTest.EmitsLoopControlFlowWithEdgeCopies`、`EmitsSwitchControlFlow` |
| 字段、对象、调用、reference check、monitor helper | 支持 | `RegCTest.EmitsObjectFieldAndCallHelpers`、`EmitsReferenceCheckAndMonitorHelpers` |
| 基本数组、多维数组、窄类型字段/数组 | 支持 | `RegCTest.EmitsArrayHelpersWithoutStackMachineAnchors`、`EmitsMultiArrayHelperWithoutStackMachineAnchors`、`EmitsNarrowPrimitiveFieldAndArrayHelpers` |
| throw、try/catch exception dispatch | 支持 | `RegCTest.EmitsThrowTerminatorWithImmediateReturn`、`EmitsTryCatchExceptionDispatch` |
| integer div/rem runtime-backed trap | 支持 | `RegCTest.EmitsIntegerDivRemHelpers`、`RegCBackendE2E.IntegerDivisionTrapRoundTrip` |
| JNI wrapper 与 native method injection | 支持 | `RegCBackendTest.EmitsJniWrapperAndMarksStaticMethodNative`、`EmitsInstanceWrapperWithReceiverRegister` |
| native DLL 行为 E2E | MVP 支持 | `RegCBackendE2E.*` |
| `.jar` 输入/输出 | MVP 支持 | `CliTest.RegCJarInputEmitsJarAndRegCSource`；核心 JAR I/O 由 `IOTest` / `PipelineTest` 覆盖 |
| JAR 负向边界 | fail-closed | `CliTest.BadJarInputsFailClosed`、`IOTest.JarInputLoaderRejectsSignedJarMetadata`、`IOTest.JarInputLoaderRejectsJarWithoutRootClasses` |
| mjdbg register/runtime samples | 支持生成 | `RegCTraceSamples.GenerateMjdbgFixtures` |

## RegC E2E 覆盖

`RegCBackendE2E` 会在本机 JDK/MSVC 可用时，把 RegC 生成 C 编译为 DLL，再由 Java 加载运行并和
原 Java 结果对比。当前覆盖：

- `PureArithmeticLoopAndArrayRoundTrip`
- `FieldAndReferenceHelpersRoundTrip`
- `MultiArrayHelpersRoundTrip`
- `CallHelpersRoundTrip`
- `NarrowPrimitiveRoundTrip`
- `StackJoinTernaryRoundTrip`
- `NullTrapHandlerRoundTrip`
- `ThrowHelperRoundTrip`
- `TryCatchDispatchRoundTrip`
- `IntegerDivisionTrapRoundTrip`

## 已知限制

- RegC 是保护路线 MVP，不是完整 javac/bytecode 全覆盖后端；遇到 unsupported shape 必须 fail closed。
- 对象、字段、数组、调用等多数 Java 语义仍通过生成 C 内的 helper 层承载；R3 runtime ABI 目前先接入
  div/rem/barrier 等核心 helper。
- `invokedynamic` 暂不直译，策略见 `regc_invokedynamic_strategy.md`。
- JAR 输出沿用 pipeline MVP 限制：资源保留、class 替换、stored ZIP 输出；
  signed-JAR metadata fail-closed，multi-release class 作为资源 passthrough。
- 保护强度不能由实现自证，需要 mjdbg/c2j trace 反馈闭环后再升级结论。

## 相关设计文档

- `ssa_algorithm.md`、`register_ir.md`：SSA 和 RegIR 设计。
- `regc_mvp.md`、`regc_native_backend.md`：RegC lowering 和 backend facade。
- `regc_runtime_helpers.md`、`regc_r3_runtime_abi.md`：helper/runtime ABI。
- `regc_trace_samples.md`：mjdbg RegC 样本。
- `mvp_backend_verification.md`：MVP 能力到测试/样本的验证索引。

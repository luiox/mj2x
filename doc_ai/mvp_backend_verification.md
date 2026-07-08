# MVP Backend Verification

## 目的

本文记录 mj2x 内部 MVP 可发版前的后端验证索引。判断标准不是“代码看起来支持”，而是每项
能力至少能追到一个自动化测试或 committed Java sample。

## 输入/输出能力

| 能力 | 状态 | 自动化验证 |
|------|------|------------|
| 单 `.class` 读取与写回 | 已覆盖 | `IOTest.InputLoaderRoundtrip`、`PipelineTest.FullPipelineRoundtrip` |
| `.jar` 读取、class 解析、资源 passthrough | 已覆盖 | `IOTest.JarInputLoaderAndOutputWriterPreserveClassesAndResources` |
| JAR manifest、多 class、资源与 multi-release class passthrough | 已覆盖 | `IOTest.JarMvpFixturePreservesManifestResourcesAndMultiReleaseClasses`；fixture 在 `test_resource/mvp_jar/` |
| signed JAR metadata fail-closed | 已覆盖 | `IOTest.JarInputLoaderRejectsSignedJarMetadata`、`CliTest.BadJarInputsFailClosed` |
| 空 JAR / 无 root class JAR fail-closed | 已覆盖 | `IOTest.JarInputLoaderRejectsJarWithoutRootClasses`、`CliTest.BadJarInputsFailClosed` |
| `.jar` pipeline native injection | 已覆盖 | `PipelineTest.JarPipelineMarksClassesNativeAndPreservesResources` |
| CLI StackC `in.jar -> out.jar + out.c` | 已覆盖 | `CliTest.JarInputEmitsJarAndCSource` |
| CLI RegC `in.jar -> out.jar + out.c` | 已覆盖 | `CliTest.RegCJarInputEmitsJarAndRegCSource` |
| CLI RegC backend flag | 已覆盖 | `CliTest.RegCBackendFlagEmitsRegCSource` |
| CLI 负向参数 | 已覆盖 | `CliTest.NegativeArgumentsFail` |

## StackC 验证索引

| 能力组 | 验证 |
|--------|------|
| i32/long/float/double/转换 | `StackCBackendE2E.IntAdd`、`IntMulSubNeg`、`IntIinc`、`IntBitwiseShift`、`LongArith`、`FloatDouble`、`Conversions` |
| int/long div/rem trap | `StackCBackendE2E.IntegerDivisionTrap` |
| 控制流 | `StackCBackendE2E.IntControlFlow`、`DenseSwitch`、`SparseSwitch`、`NestedLoopsAndContinue`、`ReferenceBranches` |
| 调用与字符串 | `StackCBackendE2E.MethodCalls`、`RecursionCalls`、`StringLiterals`、`InterfaceCalls` |
| StackC invokedynamic | `StackCBackendTest.InvokeDynamicEmitsMethodHandleBridge`、`StackCBackendE2E.InvokeDynamicStringConcat`、`InvokeDynamicLambda` |
| 对象/字段/类型检查 | `StackCBackendE2E.ObjectFieldsAndTypeChecks` |
| 数组 | `StackCBackendE2E.PrimitiveArrays`、`ObjectArrays`、`MultiDimArrays` |
| 异常与 monitor | `StackCBackendE2E.ThrowPropagation`、`TryCatchExceptions`、`MultiCatchAndFinally`、`ExceptionFromNativeCallAndMonitor` |
| 综合样本 | `StackCBackendE2E.IntegratedSample` |
| 结构生成与 injection | `StackCBackendTest.*` |
| mjdbg 样本 | `StackCBackendProtectedSamples.GenerateMjdbgFixtures`；Java sources 在 `test_resource/protected_samples/stackc/` |

## RegC 验证索引

| 能力组 | 验证 |
|--------|------|
| stackless 结构断言 | `RegCTest.*` 中的 no stack anchor assertion |
| SSA/RegIR 到 RegC 输出 | `RegCTest.EmitsPureArithmeticWithoutStackMachineAnchors`、`EmitsLoopControlFlowWithEdgeCopies`、`EmitsSwitchControlFlow` |
| helper surface | `RegCTest.EmitsArrayHelpersWithoutStackMachineAnchors`、`EmitsObjectFieldAndCallHelpers`、`EmitsReferenceCheckAndMonitorHelpers` |
| 窄类型和多维数组 | `RegCTest.EmitsNarrowPrimitiveFieldAndArrayHelpers`、`EmitsMultiArrayHelperWithoutStackMachineAnchors` |
| throw/try-catch | `RegCTest.EmitsThrowTerminatorWithImmediateReturn`、`EmitsTryCatchExceptionDispatch` |
| R3 div/rem trap | `RegCTest.EmitsIntegerDivRemHelpers`、`RegCBackendE2E.IntegerDivisionTrapRoundTrip` |
| JDK intrinsic(Math / parseInt / wrappers / Random) | `RegCIntrinsicRegistryTest.*`、`RegCIntrinsicTest.*`、`RegCBackendE2E.IntrinsicMathAndParseIntRoundTrip` |
| JNI wrapper/injection | `RegCBackendTest.*` |
| native DLL 行为对比 | `RegCBackendE2E.*` |
| mjdbg 样本 | `RegCTraceSamples.GenerateMjdbgFixtures`；Java sources 在 `test_resource/protected_samples/regc/` |
| unsupported lowering fail-closed | `RegCBackendTest.FailedLoweringDoesNotMarkMethodNative` |

## Unsupported Boundary 验证索引

| 边界 | 策略 | 验证 |
|------|------|------|
| RegC `invokedynamic` | 保持 fail-closed，不进入 SSA/RegIR/RegC lowering | `RegCBackendTest.FailedLoweringDoesNotMarkMethodNative` |
| JSR/RET | 转为显式 unsupported MIR 节点，让后端 fail closed | `CodeConverterTest.JsrAndRetBecomeUnsupportedBoundaryNodes` |
| RegC lowering 失败 | 不标记 `native`，不清空原 bytecode | `RegCBackendTest.FailedLoweringDoesNotMarkMethodNative` |

## 样本清单

StackC committed Java samples：

- `StackCTinySample.java`
- `StackCObjectArraySample.java`
- `StackCExceptionMonitorSample.java`

RegC committed Java samples：

- `RegCTinySample.java`
- `RegCHelperSurfaceSample.java`
- `RegCExceptionSurfaceSample.java`
- `RegCRuntimeSurfaceSample.java`
- `RegCIntrinsicSurfaceSample.java`

JAR MVP committed fixtures：

- `mvp_jar/MvpJarMain.java`
- `mvp_jar/MvpJarHelper.java`
- `mvp_jar/META-INF/MANIFEST.MF`
- `mvp_jar/assets/config.properties`

生成物不提交，统一写入 `test/mj2x/protected_samples/...`。这符合
`doc_ai/project-test-resource-spec.md`：`test_resource/` 存稳定输入，测试输出进入 `test/`。

## MVP 剩余风险

- signed JAR metadata 当前已 fail-closed；签名剥离/重签策略仍留到后续产品化阶段。
- StackC 是广覆盖正确性后端，不是防 c2j 主力。
- RegC 的保护强度需要 mjdbg/c2j 反馈闭环确认；当前只能声明结构上已脱离 stack-style anchor。
- JAR 级产品体验仍是 MVP writer，不做签名重建和压缩策略保真；核心读写、manifest/resource
  passthrough、multi-release passthrough 和负向边界已有自动化覆盖。

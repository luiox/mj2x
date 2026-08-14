# RegC Backend README

## 定位

RegC 是 mj2x 当前的保护导向 native 后端。它不直接输出 JVM operand stack 模板，而是走：

```text
L1 stack MIR -> SSA -> RegIR -> RegC -> JNI wrapper / R3 runtime helpers
```

目标是让生成 C 呈现 virtual-register、block label、edge copy、helper/runtime call 的结构，
逐步减少 c2j 对 `cstack[]`、`sp`、全局 method/class table 等固定形状的依赖。

当前 RegC 已过 MVP：真实 jar（JavaObfuscatorTest）业务方法 100% 转 native（零 skip），
JDK17/21 双矩阵性能追平并反超 JNIC 竞品 os30（见 benchmark/results）；v2.2 持续
迭代（safe-env、反侦测、模板化发射器、栈帧语义修复）。

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
| receiver 精确类型静态绑定（分配点传播） | 支持 | `RegCTest.EmitsStaticBoundCallForSingleAllocationReceiver`、`KeepsResolvedDispatchForUnknownReceiverType` |
| 字符串常量按内容缓存（免每次 NewStringUTF） | 支持 | RegC 单测 98/98 回归（intrinsic string literal 缓存） |
| native 互调默认走 JNI（JVM 栈帧语义） | 支持 | `RegCTest.EmitsJniCallForSelfStaticByDefault`；`--regc-direct-calls` 显式开启直调（#814/#816） |
| 性能：13-probe 双 JDK 对比 os30(JNIC) | 反超 | `mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1`（JDK21 0.88x） |

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
- `invokedynamic` 暂不直译，展开策略见 `../doc_ai/regc_codegen.md` §转换边界。
- JAR 输出沿用 pipeline MVP 限制：资源保留、class 替换、stored ZIP 输出；
  signed-JAR metadata fail-closed，multi-release class 作为资源 passthrough。
- 保护强度不能由实现自证，需要 mjdbg/c2j trace 反馈闭环后再升级结论。

## 相关设计文档

- `ir_design.md`、`ssa_algorithm.md`：L2 SSA IR 结构与构造算法（ir_design §13 含 L3 RegIR lowering）。
- `regc_mvp.md`、`regc_native_backend.md`：RegC lowering 和 backend facade。
- `regc_codegen.md`：代码生成规则、转换边界与翻译决策。
- `doc_ai/samples/regc_trace_samples.md`：mjdbg RegC 样本。
- `capabilities.md`：能力到测试/样本的验证索引。

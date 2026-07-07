# CBackend README

## 定位

CBackend 是 mj2x 当前覆盖面最宽的通用 native 后端。它把 L1 栈式 MIR 直接生成
JNI C 源码，并把被保护的 Java 方法改写为 `native` 方法。它的主要价值是：

- 提供 `.class` / `.jar` 到 C/JNI 的可执行 MVP 路径。
- 覆盖 javac 常见字节码形状，作为 RegC 之前的正确性基线。
- 为 mjdbg 提供 stack-style CBackend trace surface，方便和 RegC register-style
  产物对比。

CBackend 不承担主要抗 c2j 责任。生成 C 中保留 JVM operand stack 形状，是为了广覆盖和
可调试性；保护导向路线见 `regc_backend_readme.md`。

## CLI 用法

```powershell
xmake build mj2x-cli
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c
```

默认 `--emit-c` 后端就是 CBackend，也可以显式传入：

```powershell
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c --backend cbackend
```

JAR MVP 会读取每个可处理的 class entry，保留原 entry 顺序和非 class 资源，并在输出 JAR
中替换被改写的 class bytes。`META-INF/versions/**.class` 在 MVP 中作为资源
passthrough，不参与 root class 解析或 native injection。

## 支持矩阵

| 能力 | 状态 | 验证入口 |
|------|------|----------|
| i32 算术、位运算、`iinc`、分支和循环 | 支持 | `CBackendE2E.IntAdd`、`IntMulSubNeg`、`IntIinc`、`IntBitwiseShift`、`IntControlFlow` |
| long/float/double 与基础转换 | 支持 | `CBackendE2E.LongArith`、`FloatDouble`、`Conversions` |
| `tableswitch` / `lookupswitch` | 支持 | `CBackendE2E.DenseSwitch`、`SparseSwitch` |
| 引用分支、字符串常量、方法调用、递归、接口调用 | 支持 | `CBackendE2E.ReferenceBranches`、`StringLiterals`、`MethodCalls`、`RecursionCalls`、`InterfaceCalls` |
| 对象、字段、`instanceof`、`checkcast` | 支持 | `CBackendE2E.ObjectFieldsAndTypeChecks` |
| 基本类型数组、对象数组、多维数组 | 支持 | `CBackendE2E.PrimitiveArrays`、`ObjectArrays`、`MultiDimArrays` |
| `athrow`、try/catch、多 catch、finally、monitor | 支持 | `CBackendE2E.ThrowPropagation`、`TryCatchExceptions`、`MultiCatchAndFinally`、`ExceptionFromNativeCallAndMonitor` |
| 综合样本 | 支持 | `CBackendE2E.IntegratedSample` |
| `.class` 输入/输出 | 支持 | `IOTest.InputLoaderRoundtrip`、`PipelineTest.FullPipelineRoundtrip` |
| `.jar` 输入/输出 | MVP 支持 | `IOTest.JarInputLoaderAndOutputWriterPreserveClassesAndResources`、`PipelineTest.JarPipelineMarksClassesNativeAndPreservesResources`、`CliTest.JarInputEmitsJarAndCSource` |
| JAR manifest/resource/multi-release passthrough | MVP 支持 | `IOTest.JarMvpFixturePreservesManifestResourcesAndMultiReleaseClasses` |
| signed/empty/no-class JAR 边界 | fail-closed | `IOTest.JarInputLoaderRejectsSignedJarMetadata`、`IOTest.JarInputLoaderRejectsJarWithoutRootClasses`、`CliTest.BadJarInputsFailClosed` |
| mjdbg protected samples | 支持生成 | `CBackendProtectedSamples.GenerateMjdbgFixtures` |

## 已知限制

- 生成 C 是 `union jvalue stack[]` / `sp` 风格，便于正确性覆盖，但容易被 c2j 类工具识别。
- JNI class/method/field ID 目前按需查找，未做缓存优化。
- `<init>` / `<clinit>` 仍按 MarkAll 边界保守处理，不作为默认 native 化目标。
- `invokedynamic` 采用 fail-closed 策略，不直译 lambda、string concat 或 method handle。
- JAR 输出是 MVP：entry 顺序和非 class payload 保留，输出 entry 使用 stored ZIP 形态；
  signed-JAR metadata 触发 fail-closed，签名剥离/重签策略留到产品化阶段。
- 没有 root class entry 的 JAR 会 fail-closed；multi-release `META-INF/versions/**.class`
  作为资源 passthrough，不在 MVP 中重写。
- CBackend 的整数除零 trap 不等同完整 JVM trap 模型；RegC/R3 已先覆盖 div/rem runtime-backed
  trap 路径。

## 相关设计文档

- `cbackend_revival_and_waves.md`：Wave 0-7 覆盖推进记录。
- `c_backend_protected_samples.md`：mjdbg CBackend 样本契约。
- `mvp_backend_verification.md`：MVP 能力到测试/样本的验证索引。

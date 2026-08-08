# StackC Backend README

## 定位

StackCBackend 是 mj2x 当前覆盖面最宽的通用 native 后端。它把 L1 栈式 MIR 直接生成
JNI C 源码，并把被保护的 Java 方法改写为 `native` 方法。它的主要价值是：

- 提供 `.class` / `.jar` 到 C/JNI 的可执行 MVP 路径。
- 覆盖 javac 常见字节码形状，作为 RegC 之前的正确性基线。
- 为 mjdbg 提供 stack-style StackCBackend trace surface，方便和 RegC register-style
  产物对比。

StackCBackend 不承担主要抗 c2j 责任。生成 C 中保留 JVM operand stack 形状，是为了广覆盖和
可调试性；保护导向路线见 `regc_backend_readme.md`。

Native 绑定使用 `static` C 函数加 `JNI_OnLoad` / `RegisterNatives`。生成 DLL/SO 不再为每个
Java 方法导出 `Java_*` 符号；Java 方法名和描述符保留在注册表中供 JVM 绑定。方法体内
应用类解析会使用当前 owner class 的 `ClassLoader`，避免自定义 class loader 场景下
直接 `FindClass` 找不到业务类。生成方法体会跟踪临时 JNI local refs，并在正常返回或
异常提前返回前批量释放；对象返回值会保留下来作为返回引用。字段和方法 ID 在各生成
调用点按需懒初始化到 `static jfieldID` / `static jmethodID`，避免每次执行都重新解析。
字符串 LDC 常量会去重到 TU 级 `_mj2x_string_pool[]`，方法体只保留 pool offset。
StackCBackend 对 `invokedynamic` 采用运行时桥:在 native 侧调用 bootstrap，取
`CallSite.getTarget()` 后通过 `MethodHandle.invokeWithArguments` 执行，覆盖 javac
生成的 string concat、方法引用和捕获 lambda。

## CLI 用法

```powershell
xmake build mj2x-cli
xmake run mj2x-cli -i input.class -o output.class --emit-c output.c
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c
```

默认 `--emit-c` 后端就是 StackCBackend，也可以显式传入：

```powershell
xmake run mj2x-cli -i input.jar -o output.jar --emit-c output.c --backend stackc
```

也可以生成独立 CMake 工程，工程内包含 `native.c`、`CMakeLists.txt`、`.gitignore` 和
构建说明：

```powershell
xmake run mj2x-cli -i input.class -o output.class --generate-cmake build/native_project
cmake -S build/native_project -B build/native_project/build
cmake --build build/native_project/build --config Release
```

JAR MVP 会读取每个可处理的 class entry，保留原 entry 顺序和非 class 资源，并在输出 JAR
中替换被改写的 class bytes。`META-INF/versions/**.class` 在 MVP 中作为资源
passthrough，不参与 root class 解析或 native injection。

## 支持矩阵

| 能力 | 状态 | 验证入口 |
|------|------|----------|
| i32 算术、位运算、`iinc`、分支和循环 | 支持 | `StackCBackendE2E.IntAdd`、`IntMulSubNeg`、`IntIinc`、`IntBitwiseShift`、`IntControlFlow` |
| int/long 除零 trap 与 `MIN_VALUE / -1` 边界 | 支持 | `StackCBackendE2E.IntegerDivisionTrap` |
| long/float/double 与基础转换 | 支持 | `StackCBackendE2E.LongArith`、`FloatDouble`、`Conversions` |
| `tableswitch` / `lookupswitch` | 支持 | `StackCBackendE2E.DenseSwitch`、`SparseSwitch` |
| 引用分支、字符串常量、方法调用、递归、接口调用 | 支持；字符串常量走 TU 级 pool，静态/特殊/接口调用的 owner 解析使用当前 owner `ClassLoader` | `StackCBackendE2E.ReferenceBranches`、`StringLiterals`、`MethodCalls`、`RecursionCalls`、`InterfaceCalls` |
| `invokedynamic` string concat / lambda | 支持；native 侧 bootstrap + MethodHandle 桥执行 | `StackCBackendE2E.InvokeDynamicStringConcat`、`InvokeDynamicLambda` |
| 对象、字段、`instanceof`、`checkcast` | 支持；业务类解析使用当前 owner `ClassLoader` | `StackCBackendE2E.ObjectFieldsAndTypeChecks` |
| 基本类型数组、对象数组、多维数组 | 支持；对象/多维数组类解析使用当前 owner `ClassLoader`，`BALOAD`/`BASTORE` 对 byte[]/boolean[] 做运行时分流 | `StackCBackendE2E.PrimitiveArrays`、`ObjectArrays`、`MultiDimArrays` |
| `athrow`、try/catch、多 catch、finally、monitor | 支持 | `StackCBackendE2E.ThrowPropagation`、`TryCatchExceptions`、`MultiCatchAndFinally`、`ExceptionFromNativeCallAndMonitor` |
| JNI local refs | 支持方法级追踪与返回前批量释放 | `StackCBackendTest.*`、对象/数组/异常 StackCBackend E2E |
| JNI method/field ID | 支持调用点级懒缓存 | `StackCBackendTest.FieldAccess`、`InvokeMethod`、方法/字段 StackCBackend E2E |
| 综合样本 | 支持 | `StackCBackendE2E.IntegratedSample` |
| `.class` 输入/输出 | 支持 | `IOTest.InputLoaderRoundtrip`、`PipelineTest.FullPipelineRoundtrip` |
| `.jar` 输入/输出 | MVP 支持 | `IOTest.JarInputLoaderAndOutputWriterPreserveClassesAndResources`、`PipelineTest.JarPipelineMarksClassesNativeAndPreservesResources`、`CliTest.JarInputEmitsJarAndCSource` |
| CMake 独立工程输出 | 支持 `--generate-cmake <dir>` | `CliTest.GenerateCMakeProjectEmitsStandaloneFiles` |
| JAR manifest/resource/multi-release passthrough | MVP 支持 | `IOTest.JarMvpFixturePreservesManifestResourcesAndMultiReleaseClasses` |
| signed/empty/no-class JAR 边界 | fail-closed | `IOTest.JarInputLoaderRejectsSignedJarMetadata`、`IOTest.JarInputLoaderRejectsJarWithoutRootClasses`、`CliTest.BadJarInputsFailClosed` |
| mjdbg protected samples | 支持生成 | `StackCBackendProtectedSamples.GenerateMjdbgFixtures` |

## 已知限制

- 生成 C 是 `union jvalue stack[]` / `sp` 风格，便于正确性覆盖，但容易被 c2j 类工具识别。
- JNI local refs 按方法级固定容量追踪并在返回前释放；极端长循环中持续创建新局部引用时，后续仍可补周期性清理。
- JNI method/field ID 懒缓存未加锁；多线程首次同时命中同一调用点时可能重复解析一次，但最终缓存值等价。
- `<init>` / `<clinit>` 仍按 MarkAll 边界保守处理，不作为默认 native 化目标。
- `invokedynamic` 通过 MethodHandle runtime bridge 执行，正确性优先；不是隐藏 bootstrap
  或 lambda 语义的保护型实现。RegC 对未展开 indy 仍 fail-closed。
- JAR 输出是 MVP：entry 顺序和非 class payload 保留，输出 entry 使用 stored ZIP 形态；
  signed-JAR metadata 触发 fail-closed，签名剥离/重签策略留到产品化阶段。
- 没有 root class entry 的 JAR 会 fail-closed；multi-release `META-INF/versions/**.class`
  作为资源 passthrough，不在 MVP 中重写。

## 相关设计文档

- `stackc_revival_and_waves.md`：Wave 0-7 覆盖推进记录。
- `doc_ai/samples/stackc_protected_samples.md`：mjdbg StackCBackend 样本契约。
- `capabilities.md`：能力到测试/样本的验证索引。

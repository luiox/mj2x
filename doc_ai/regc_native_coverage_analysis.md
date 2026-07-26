# RegC Native 覆盖率分析

> 实测文档:基于 `JavaObfuscatorTest/test.jar`(37 class,Main-Class `pack.Main`)的 RegC 全量 native 转换实测,逐方法定位"未转 native"的根因,并给出可修复性判断。本文档是性能评估规范(见 `performance_evaluation_spec.md`)的前置依据——覆盖率决定了性能数据的真实性。

## 1. 背景

RegC 后端宣称"全量 native",但实测 `test.jar` 只有约 52% 业务方法真正转为 native。本文档回答:

1. 哪些方法没转?
2. 每个未转方法的根因是什么(代码级)?
3. 是设计如此还是 bug?
4. 对性能分析有何影响?

**结论速览**:`test.jar` 的 46 个业务方法中,25 个成功 native、20 个显式 skip、1 个静默 fail-closed(浮点比较 intrinsic 未实现,属 bug)。显式 skip 是 RegC 的保守兼容策略,静默 fail-closed 是代码遗漏。**注:浮点比较 bug 与静默 fail-closed 可观测性问题均已修复(见 §5、§6);修复后该 jar 的 46 方法全部成功 native 或显式 skip,0 静默 fail-closed。**

## 2. 两层"不转"机制

RegC 的"不转"分两层,最终都汇到 `injectIntoClass`(`regc_backend.cpp:295`):

```cpp
// Failed lowering leaves the Java bytecode intact.
if (result.kind != BackendResult::Kind::C_SOURCE) return;
```

`BackendResult::Kind` 只有三个值(`backend_result.hpp`):`NONE`(默认/失败)、`C_SOURCE`(成功)、`BINARY`(未用)。**只要 `compileMethod` 没走到设置 `kind = C_SOURCE` 这行,方法就保留 Java 字节码。**

### 2.1 第一层:显式 skip

SSA 构造**之前**直接返回,同时打 `[RegCBackend] Skipping <method>: <reason>` 日志。判定入口 `regc_backend.cpp` 的 `compileMethod`,先查内部类,再调 `shouldKeepBytecodeForCompatibility`。

**特征**:有日志,可观测。

### 2.2 第二层:静默 fail-closed

SSA builder / RegLowering / Emitter 任一返回失败,`compileMethod` 仍会产出一段 JNI wrapper,但 `kind` 保持 `NONE`,`injectIntoClass` 静默退出。

**特征**:无任何日志输出,生成 C 里也无失败注释,**完全不可观测**。这是诊断盲区(见第 6 节可观测性问题)。

## 3. 显式 skip 触发点(5 类)

判定逻辑在 `regc_backend.cpp`,函数 `shouldKeepBytecodeForCompatibility`(`:109-137`)和 `isCompatibilityRiskCall`(`:97-107`)。

| # | skip 原因(日志文案) | 触发条件 | 代码位置 | 性质 |
|---|---|---|---|---|
| ~~S1~~ | ~~`inner class method remains on bytecode path for compatibility`~~ | ~~类名含 `$`~~ | ~~`regc_backend.cpp`~~ | **已撤销**（R6-B RegisterNatives 绑定，PR #661） |
| S2 | `lambda body remains on bytecode path for invokedynamic compatibility` | 方法名以 `lambda$` 开头 | `shouldKeepBytecodeForCompatibility` | 设计如此 |
| ~~S3~~ | ~~`large exception-table method remains on bytecode path`~~ | ~~`tryCatchBlocks.size() > 4`~~ | — | **已撤销**（异常表阈值放宽，PR #657） |
| S4 | `invokedynamic remains on bytecode path for compatibility` | 块内有 `MIRKind::INVOKE_DYNAMIC` | `shouldKeepBytecodeForCompatibility` | 设计如此(待展开) |
| S5 | `high-risk runtime call remains on bytecode path: <owner>.<name>` | INVOKE 命中黑名单 | `shouldKeepBytecodeForCompatibility` | 设计如此 |

### 3.1 high-risk API 黑名单(完整列表)

`isCompatibilityRiskCall`(`:97-107`)按 owner 维度判定(粗粒度,除 System/Object):

| owner 条件 | 覆盖范围 |
|---|---|
| `java/lang/Class` | Class 全部方法(forName/newInstance/getMethod/getDeclaredField...) |
| `java/lang/ClassLoader` | ClassLoader 全部方法 |
| `java/lang/Thread` | Thread 全部方法 |
| `java/io/InputStream` | InputStream 全部方法 |
| `java/io/ByteArrayInputStream` | ByteArrayInputStream 全部方法 |
| `java/lang/reflect/*`(前缀) | 反射包全部(Method/Field/Constructor/Array/Proxy...) |
| `java/util/concurrent/*`(前缀) | 并发包全部(Executor/Future/atomic/locks...) |
| `java/lang/Object` + name=`getClass` | 仅 getClass |
| `java/lang/System` + name∈{`setSecurityManager`,`getSecurityManager`,`exit`} | 安全/退出 |

注意:`System.currentTimeMillis`、`System.getProperty` 等**不在**黑名单,走正常 INVOKE 路径。黑名单是兼容性保守策略(避免 native 化后语义偏差),非实现限制。

## 4. 静默 fail-closed 路径(4 大类)

`compileMethod` 的 4 个失败点,全部走 `emitJniWrapper(... ok=false ...)` 产出废弃 wrapper:

| # | 失败路径 | 触发条件 | 代码位置 |
|---|---|---|---|
| F1 | SSA 构造失败 | `ssaBuilder.build` 返回 nullptr | `regc_backend.cpp:254-259` |
| F2 | RegLowering 失败 | `lowering.lower` 返回 nullptr | `:262-266` |
| F3 | Emitter 失败 | `emitter.emitFunction` 返回 ok=false | `:270-275` |
| F4 | 其他逻辑路径默认 ok=false | — | — |

F1 子原因:`ssa_builder.cpp` 的栈下溢、DUP/SWAP category 校验、invokedynamic 未展开等数十处。F2 子原因:`reg_lowering.cpp` 的 unknown kind、stack-slot 引用(`validateRegIRNoStackSlots`)等。F3 子原因:`regc_emitter.cpp` 的 native 对象逃逸、unsupported intrinsic、stack anchor 检测等。

**实测 `test.jar` 仅触发 F3 的一个子原因**(见第 5 节),其余 F1/F2 在该 jar 未出现。

## 5. Calc.runAdd 的根因(实测确认的 bug,已修复)

`Calc.runAdd` 是纯 double 循环(`dconst_0/dload/dcmpg/ifge/dadd/dstore/goto`),不在任何显式 skip 列表,曾保留完整 Java 字节码——是静默 fail-closed。**已于本批修复**:转换后 javap 确认 `runAdd` 为 `native`,运行 `Calc: <N>ms` 且 `runAll()` 校验 count==30000 通过。

**根因链(代码行已验证)**:

1. `dcmpg` 在 SSA 构造时由 `lowerCmp`(`ssa_builder.cpp:824-842`)翻译为 intrinsic,name = `cmp.nan_greater`(dcmpg)或 `cmp.nan_less`(dcmpl):
   ```cpp
   instr->name = cmp->nanGreater ? "cmp.nan_greater" : "cmp.nan_less";  // ssa_emitter.cpp:833
   ```
2. 该 intrinsic 一路通过 RegLowering(只 mapValue 不过滤 name),`args[0].type` 保留 F32/F64/I64 类型信息。
3. 到 Emitter 的 `RegInstructionKind::Intrinsic` 分支(`regc_emitter.cpp:1965-2009`),原只识别 4 个 name:`array.length`、`instanceof:*`、`monitor.enter`、`monitor.exit`。`cmp.nan_*` 不在其中,落入:
   ```cpp
   reason = "unsupported RegC intrinsic: " + ins.intrinsicName;  // regc_emitter.cpp:2008
   return false;
   ```
4. `emitFunction` 返回 ok=false,走 F3 路径,`kind` 保持 NONE,静默保留字节码。

**影响范围**:任何用 `dcmpg/dcmpl/fcmpg/fcmpl` 的方法都会命中——即几乎所有带浮点比较/循环条件的浮点方法。double 算术本身已支持(`binaryResultType` 产出 F64、`binaryExpr` 支持 ADD/MUL/SUB/REM),只差浮点 NaN-aware 比较。注意 `lcmp`(long 比较)也共用同一 intrinsic(`nanGreater=false` → `nan_less`),修复一并覆盖。

**修复实现**(已完成):在 `regc_emitter.cpp` 的 intrinsic 分支补 `cmp.nan_greater`/`cmp.nan_less` 两个 case,按 `ins.args[0].type` 分派:
- 浮点(F32/F64):NaN-aware 三值,`(isnan(l)||isnan(r)) ? <±1> : ((l>r)-(l<r))`,`nan_greater` 取 1(dcmpg/fcmpg),`nan_less` 取 -1(dcmpl/fcmpl)。
- 整型(I64/LCMP):无 NaN 概念,直接 `(l>r)-(l<r)`。
- 覆盖测试:`RegCCompareIntrinsicTest.*`(5 例,含 F64/F32/I64 发射 + 全链路 opcode)。语义蓝本参考 StackC `stackc_backend.cpp:2301 emitCmp`。

## 6. 可观测性问题(静默 fail-closed 的诊断盲区,已修复)

实测发现:静默 fail-closed 曾**完全不可观测**——不打 stderr 日志,生成 C 里也无失败注释(原本设计的 `/* RegC lowering failed: ... */` 注释实际未输出)。这导致:

- 用户无法知道哪些方法没转
- 性能分析时若不 javap 逐方法检查,会误以为全转了(本文档的起因)

**修复实现**(已完成):`compileMethod` 的 3 个 fail-closed 点(SSA 构建/RegIR lowering/emitter 发射失败,`regc_backend.cpp`)各在 `emitJniWrapper` 调用前 `printf` 一行,格式统一为 `[RegCBackend] Skipping <class>.<method><desc>: <stage> failed: <reason>`,`stage` 为 `SSA build` / `lowering` / `emit`。与显式 skip 日志 grep 一致,阶段前缀可区分来源。`RegCBackendTest.FailedLoweringDoesNotMarkMethodNative` 实测输出 `[RegCBackend] Skipping Bad.bad()I: SSA build failed: stack underflow at RETURN_VALUE`。


## 7. StackC vs RegC 覆盖策略差异

| 维度 | StackC | RegC |
|---|---|---|
| invokedynamic/lambda | 完整支持(MethodHandle 桥,`stackc_backend.cpp:2729`) | fail-closed |
| high-risk call | **无黑名单**,全走 JNI | 显式 skip |
| 内部类/大异常表 | 无限制 | 显式 skip |
| dcmpg/dcmpl/fcmpg/fcmpl/lcmp 比较 | 支持(原生 C 比较) | 支持(emitter intrinsic NaN-aware 三值) |
| IR 模型 | MIR→C,忠实栈机翻译(`jvalue stack[]`) | MIR→SSA→RegIR→C,要求无栈语义 |

**本质差异**:StackC 把 JVM 操作数栈原样搬到 C 的 `jvalue stack[]`,所以 invokedynamic(用 MethodHandle)、浮点比较(用原生 C)、反射调用(用 JNI)都能直接表达。RegC 追求"无栈纯寄存器代码",必须消除所有栈语义。浮点/long 三值比较已通过 `cmp.nan_*` intrinsic 实现(见 §5);invokedynamic 展开未做完,用 fail-closed 兜底(且有可观测日志,见 §6)。

## 8. `test.jar` 实测覆盖统计

> 以下为修复前快照。修复 cmp.nan_* 后,Calc.runAdd 转为成功 native,静默 fail-closed 降为 0。

46 个业务方法(不含 init/clinit):

| 类别 | 数量 | 说明 |
|---|---|---|
| 成功 native | 25 | 生成 `Java_*` JNI export |
| 显式 skip(high-risk call) | 13 | 反射/安全/IO/Thread API |
| 显式 skip(invokedynamic) | 1 | Task.lambda$run$0 |
| 显式 skip(large exception-table) | 1 | Main.main(tryCatchBlocks>4) |
| 显式 skip(inner class) | 1 | Exec$Inner.doAdd |
| ~~静默 fail-closed~~ | ~~1~~ | ~~Calc.runAdd(cmp.nan_greater 未实现,bug)~~ **已修复,现转 native** |

显式 skip 高频命中的黑名单 API:`java/lang/Class.*`(getDeclaredMethods/getFields/...)、`Object.getClass`、`System.exit/setSecurityManager`、`InputStream.read`、`Thread.sleep`。

## 9. 可修复性判断(优先级)

| 类别 | 可修复性 | 优先级 |
|---|---|---|
| ~~`cmp.nan_*` intrinsic(浮点/long 比较)~~ | ~~可修复,bug~~ | ~~高~~ **已修复** |
| 大异常表阈值 > 4 | 阈值可调/可移除 | 中(异常表 lowering 已实现,阈值偏保守) |
| 内部类 `$` 硬跳 | 可讨论放宽 | 中 |
| ~~静默 fail-closed 可观测性~~ | ~~加 printf 即可~~ | ~~中~~ **已修复** |
| invokedynamic/lambda | 工作量大,设计如此 | 低(需 SSA 层展开 bootstrap) |
| high-risk 黑名单·反射组 | ~~不可直接删~~ **已放开**（引用管理前置完成） | ~~中~~ **已完成** |
| high-risk 黑名单·类加载/IO/并发 | 分项放开（各有独立前置） | 中/低（见 §12） |
| init/clinit | JVM 限制,设计如此 | 不修 |

## 10. 关联

- 性能评估规范:`performance_evaluation_spec.md`
- invokedynamic 策略:`regc_invokedynamic_strategy.md`
- intrinsic 体系:`regc_intrinsic.md`(本文档的 `cmp.nan_*` 是该体系的遗漏项)
- RegC 能力声明:`capabilities.md`

## 11. 覆盖率提升实验记录（2026-07-24，对标 JNIC/native-obfuscator 全转策略）

**目标**:把 RegC 业务方法覆盖率从 75% 提升到接近 100%,对齐 JNIC（实测 pack.* 100% 转 native）。

**安全放开的（已合入）**:
- 异常表阈值 `tryCatchBlocks > 4`（`regc_backend.cpp` `shouldKeepBytecodeForCompatibility`）:lowering 三层（SSA `ssa_builder.cpp:1402`、RegIR `reg_lowering.cpp:360`、emitter `regc_emitter.cpp:1631`）对 catch 数量/类型/嵌套无上限。实测 `pack.Main`（14 个 catch(Throwable)）转 native 后全测试通过。

**实测发现不可直接放开的（已回退,需前置工作）**:
- **high-risk 黑名单**（`isCompatibilityRiskCall`）:取消后反射测试（Test #2 Counter/Resource/Field/Loader/ReTrace/Annotation）全失败。根因:RegC `_mj2x_regc_call_ref`（`regc_runtime_template.cpp:113`）返回的 JNI local ref 不做 `NewLocalRef`/`DeleteLocalRef` 管理,反射返回的 Class/Method/Field 对象生命周期与 Java 不一致。`Object.getClass`（javac 空检查惯用法）、`Class.*`、`reflect.*` 全受影响。
- **Thread.sleep**:在 native 帧（线程池工作线程）调用会破坏 JDK 并发库（`ThreadPoolExecutor`/`LinkedBlockingQueue` 的 `ReentrantLock`）monitor 状态,抛 `IllegalMonitorStateException`。
- **内部类 `$` skip**:取消后 `Exec$Inner.doAdd` 转 native 语义出错（InnerClass 测试失败）,`this$0` 合成字段访问/外部类方法调用路径不完善。

**前置条件（R6）**:先完善 RegC call 路径的引用管理（local ref 追踪/释放,参考 StackC `trackLocalRef` `stackc_backend.cpp:2749`）+ 字段访问路径,再分项放开黑名单与内部类 skip。invokedynamic 是独立大工程（R6.1,搬 StackC 477 行实现）。native 库打包/加密技术整理成 wiki（R6.2）。

**参考实现**:JNIC（AOT 真机器码,`JNICLoader` 解密流,dump 工具 `JNIC-Virtualization`）、native-obfuscator（`LoaderUnpack`,无 high-risk 黑名单全转,indy 走字节码预处理不编译 native）、morpher-legacy `CompressDataTool`（带去重排序压缩）。

## 12. R6 引用管理落地 + 反射组放开（2026-07-24，已完成）

**引用管理机制（JNI PushLocalFrame/PopLocalFrame）**:
RegC 每个 native 方法在单一 JNI 帧内运行，产 object 的 helper（`_mj2x_regc_call_ref` / `class_literal` / 非缓存 `find_class` / ref-field/array load）返回的 local ref 全程累积不释放，反射密集方法撑爆 JNI 默认保证的 16 槽 local ref 表。落地方案（`regc_emitter.cpp` `emitFunction`）：入口 `PushLocalFrame(env,16)`（JVM 按需扩容），单出口 `_mj2x_regc_cleanup:` 处 `PopLocalFrame`——REF 返回值经 promote 到父帧，非 REF/VOID 传 NULL。语义等价 StackC 的 `trackLocalRef`+`clear`，但零 C 数组、无 c2j stack anchor（不触发 `validateRegCNoStackAnchors`），更契合 RegC 保护目标。纯标量方法（`bodyProducesLocalRefs` 判空）不发射帧，保留「零 JNI」形态。缓存 class/method/field 走 `NewGlobalRef`，不受帧影响。

**附带修复的 REF 比较 bug**:
`if_acmpeq/ne` 及与 null 比较原来发射裸 `void* == void*`。JNI local ref 是句柄，同一对象的两个不同 ref 句柄指针不相等，导致 `a.getClass() == b.getClass()` 误判。改为 `IsSameObject`（对 NULL 也正确）。这是引用管理暴露出的独立正确性 bug。

**反射组黑名单放开**（`isCompatibilityRiskCall` 重构为分组）:
移除 `java/lang/Class`、`java/lang/reflect/*`、`Object.getClass`；保留类加载（`ClassLoader`）、并发（`Thread`/`java.util.concurrent`）、IO（`InputStream`/`ByteArrayInputStream`）、System（`exit`/`SecurityManager`）。

**实测验证（JavaObfuscatorTest `TEST.jar`）**:
- 覆盖率 46/56 方法转 native（82%）；剩余 10 个 skip 全部是有意保留黑名单（IO×4 / ClassLoader×1 / Thread×1 / System×2 / indy×2），无任何反射方法被 skip。
- javap 确认反射类方法转 native：`Countee.{mpuli,mprot,mpackp,mpriv}`、`FTest.run`、`Tracer.run`、`annoe.{dox,dov}` 均为 `native`；`Loader.findClass`（ClassLoader）正确保留字节码。
- 生成 C++ 编译为 202KB DLL（cl /O2），全 jar 运行反射测试 2.1 Counter / 2.3 Resource / 2.4 Field / 2.5 Loader / 2.6 ReTrace / 2.7 Annotation **全部 PASS**，与 baseline 逐行一致（1.6 Pool 反而 FAIL→PASS，2.8 Sec ERROR 与 baseline 相同——`setSecurityManager` 保留字节码）。
- E2E 回归：`RegCBackendE2E.ReflectionRoundTrip` 新增通过，mj2x_unittest 全量 216 passed。

**剩余待办（各有独立前置，不属纯引用管理）**:
- ~~**类加载**（`ClassLoader`）~~:~~验证自定义 loader 下 `defineClass`/`loadClass`/`getResourceAsStream` 路径。~~ **已放开（R6-B，见 §13）**。
- ~~**IO**（`InputStream.read` 等）~~:~~引用管理稳定后放开。~~ **已放开（R6-A，见 §13）**。
- **并发**（`Thread.sleep`）:**独立根因**——native 帧调用破坏线程池 `ReentrantLock` monitor 状态抛 `IllegalMonitorStateException`，非引用管理可解，需单独设计。**调研结论（R6-D，见 §13）：保留**。
- ~~**内部类 `$`**~~:~~`this$0` 合成字段访问/外部类方法路径。~~ **已验证（R6-C，见 §13）**。

## 13. R6-A/B/C/D 黑名单分项放开 + 并发组调研（2026-07-25，已完成）

本批逐项放开 IO/类加载黑名单、验证内部类访问路径、调研并发组根因并决策保留。覆盖率从 §12 的 46/56（82%）提升到约 50/56（~90%，IO×4 + ClassLoader×1 转为 native；剩 Thread×1 / System×2 / indy×2 共 5 个有意 skip，加 init/clinit JVM 限制）。

### 13.1 R6-A IO 组放开 + collect_args 数组参数 bug 修复（关键）

放开 `java/io/InputStream`/`java/io/ByteArrayInputStream` 黑名单。新增 e2e `IOGroupRoundTrip`：`read()`/`available()`/`read(byte[])` 在 native 帧调用，覆盖单字节 int 返回、byte[] 读写。

**踩出的关键 bug**：放开后 `ByteArrayInputStream(byte[])` 构造直接 crash JVM（HotSpot 17 `EXCEPTION_ACCESS_VIOLATION`，pc=jvm.dll+0xbfb0，reading 0x88）。根因在 `_mj2x_regc_collect_args`（`regc_runtime_template.cpp`）：参数描述符 `[B` 的 `[` 被 `while (tag == '[') tag = *++p;` 消耗后 `tag` 变成元素类型 `B`，落入 `case 'B': values[i].b = (jbyte)va_arg(ap, int);`——**把数组指针当 jbyte 读**（截断 64 位指针低 8 位），构造器收到垃圾 buf 指针。

修复：跟踪 array-ness，数组参数（任意 `[X`）与 `L` 类型一样走 `values[i].l = va_arg(ap, mj2x_ref)`：
```c
int32_t is_array = 0;
while (tag == '[') { is_array = 1; tag = *++p; }
if (is_array || tag == 'L') {
    values[i].l = (jobject)va_arg(ap, mj2x_ref);
} else { switch (tag) { /* 原生类型 cases */ } }
```
**影响面**：所有带数组参数的 JNI 方法调用此前都被破坏（只是 RegC e2e 此前没有数组参数用例命中），非 IO 特有。

### 13.2 R6-B 类加载组放开

放开 `java/lang/ClassLoader` 黑名单。新增 e2e `ClassLoaderGroupRoundTrip`：`ClassLoader.getSystemClassLoader()`（static）/`loadClass(String)`（ref 参数 + Class ref 返回）/`getResourceAsStream(String)`（InputStream ref 返回）。引用管理吸收 Class/InputStream ref 返回值；`loadClass` 触发的递归类初始化与 RegisterNatives 惰性绑定无冲突。实测 `loadClassName("java.lang.String")`、`resourceAvailable("RegCLoader.class")=1`、`systemLoaderName()` 全部 baseline 一致通过。

### 13.3 R6-C 内部类访问路径验证 + 实例方法抽象 owner bug 修复

内部类 `$` 整体 skip 已随 RegisterNatives（PR #661）撤销。本批补端到端验证：新增 e2e `InnerClassFieldAccessRoundTrip`——非静态内部类 `Inner` 访问外部类 `this$0.base` 合成字段 + 调用 `Outer.this.outerMethod()`。实测 `viaInner(10,5)`、`viaInner(100,1)` baseline 一致通过。

**踩出的关键 bug**：调试期间发现实例方法分派对**抽象声明 owner** 不安全。`_mj2x_regc_cached_instance_method` 原按声明 owner（`cache->owner`）`GetMethodID`——当 owner 是抽象类/接口（如 `java/io/InputStream.read`）时，得到的是抽象方法条目，`CallIntMethodA` 在 JIT 内联路径直接 crash。修复：方法 ID 改为按 `GetObjectClass(receiver)` 实际类解析，仅当 receiver 运行时类 == 声明 owner 时复用/写回缓存（覆盖最常见的同类型多次调用）；子类 receiver 每次重解析（开销可忽略），避免缓存某子类 ID 误导另一子类。

### 13.4 R6-D 并发组根因调研 → 决策保留

**实测纠正 dev_plan 旧描述**：`Thread.sleep(long)` 在**主线程** native 帧调用**不**抛 `IllegalMonitorStateException**——去掉 Thread 黑名单后 native `Thread.sleep(1)` 正常返回，仅结果存在调度抖动（baseline 2.0ms vs protected 2.5ms，因 JNI 调用开销 + 调度器差异）。旧描述的 IMSE 只在**线程池工作线程**场景成立（`ThreadPoolExecutor`/`LinkedBlockingQueue` 的 `ReentrantLock` monitor 与 native 帧交互）。

**决策**：保留并发组黑名单（`java/lang/Thread` + `java/util/concurrent/*`）。理由：
1. 线程池/锁语义在 native 帧不可靠（IMSE 风险），且难在编译期静态判定调用点是否在线程池工作线程上下文。
2. 并发 API 在受保护业务方法中本就稀疏，放开覆盖率收益低。
3. 即便主线程 sleep 能跑通，结果受调度抖动影响，对 round-trip 测试不友好。

R6-D e2e 用例（`nativeSleep` 比对耗时）因 sleep 耗时非确定性被废弃——并发组的放/留不靠 e2e 判定，靠上述根因分析。

### 13.5 黑名单现状（R6 后）

`isCompatibilityRiskCall` 现仅保留两组：
- **并发**（保留）：`java/lang/Thread`、`java/util/concurrent/*`。
- **System**（保留）：`System.exit`/`setSecurityManager`/`getSecurityManager`。

其余（反射 / IO / 类加载）均已放开。invokedynamic 走分阶段展开（见 §14）。

## 14. R6.1 阶段1：StringConcatFactory invokedynamic 展开（2026-07-25，已完成）

**背景与路线选择**：dev_plan 原方案是照搬 StackC 的 477 行 native runtime bridge
（`stackc_backend.cpp:2732`，运行时真调用 bootstrap → CallSite → invokeWithArguments）。
调研发现 RegC 后端**没有 `_mj2x_owner_class` 概念**（native wrapper 有 `clazz`/`thisObject`
但未传入内层 helper），而 StackC bridge 的 privateLookupIn/findSpecial/classloader 可见性
强依赖它——照搬需先给 RegC 引入 owner-class 机制，成本高。改采策略文档
（[regc_invokedynamic_strategy.md](regc_invokedynamic_strategy.md)）的 **pre-SSA expansion**
路线，分阶段展开，阶段1 先做占 javac 生成 indy 绝大多数的 string concat。

**机制**：`InvokeDynamicPreprocessor`（`preprocessor/invokedynamic_preprocessor.cpp`）从纯诊断
改为改写字节码。命中 `java/lang/invoke/StringConcatFactory` 的 `makeConcatWithConstants`
（有 recipe 常量）/ `makeConcat`（无 recipe，全动态参数顺序拼接）时：
1. indy 执行前 N 个动态实参已在操作数栈（栈底=第1参）。逆序 `xSTORE` 到从 `maxLocals` 起
   新分配的局部槽（long/double 占 2 槽），清空栈上参数。
2. `new StringBuilder` / `dup` / `<init>()V`。
3. 按 recipe 从左到右：字面量文本→`append(String)`；`` 动态占位→`xLOAD` 对应槽 +
   `append(<按参数类型选重载>)`（boolean/char 专用重载，byte/short/int→`append(I)`，
   long/float/double 各有重载，String→`append(String)`，其余对象→`append(Object)`）；
   `` 常量占位→`LDC` bsmArg 常量 + `append(String)`。
4. `toString()`，栈顶恢复为单个 String（与 indy 返回一致）。

**关键正确性点**：展开是**单块直线代码**，不引入分支/label，故 StackMapTable 帧
（只在块边界）不受影响，仅 `maxLocals` 增大。RegC 已有完整 StringBuilder intrinsic
（`regc_emitter.cpp:344+`），展开后的字节码直接转 native，**无需任何 native 新代码，
也完全绕开 owner-class 障碍**。

**测试**：
- e2e `RegCBackendE2E.InvokeDynamicStringConcatRoundTrip`：`"a="+int+",b="+long+",s="+String`、
  null 拼接、相邻动态参数、boolean/char/double 混合，protected native 与 Java baseline 逐行一致。
- 单元 `InvokeDynamicPreprocessorTest.*`：makeConcatWithConstants / makeConcat 展开断言
  （indy marker 消失、StringBuilder 构造/append/toString 计数、maxLocals 增大）+ lambda 相关
  见 §15。

**剩余**：阶段2 已完成（见 §15）、阶段3（自定义 bootstrap / 评估 runtime bridge），见 dev_plan R6.1。

## 15. R6.1 阶段2：无状态 + 捕获 lambda 展开（2026-07-25，已完成）

**路线选择**：调研确认 lambda 的 StackC runtime bridge 同样强依赖 `_mj2x_owner_class`
（lambda$/目标方法多为 private，metafactory 生成的实现类需 caller 的 `privateLookupIn`
才能访问）——RegC 无此机制。故继续走 pre-SSA expansion，采**合成实现类**路线（等价
Java 8 之前 javac 对 lambda 的降级形态）。

**lambda indy 结构**（javac 17）：`bsm = LambdaMetafactory.metafactory`；`bsmArgs[0]` =
samMethodType(Type)、`bsmArgs[1]` = implMethodHandle(Handle)、`bsmArgs[2]` =
instantiatedMethodType(Type)。**indy desc 的参数个数 = 捕获变量个数**（无状态=0 参），
返回类型 = 目标 functional interface。无状态方法引用（`X::base`）的 impl 直接指向用户
方法，javac 不生成 lambda$ 合成方法。

**机制**（`InvokeDynamicPreprocessor::expandLambdaSite`，无状态+捕获统一）：
1. 校验：`bsmArgs[1]` 是 tag==`H_INVOKESTATIC` 的 Handle；impl 形参 == capturedTypes ++
   samArgs（capturedTypes = indy desc 参数；无装箱/桥接的直通形态）。
2. 合成类 `mj2x/lambda/Lambda$<N>`（`buildLambdaClassNode`，照 `cloader_postprocessor.cpp`
   套路）：`implements` 目标接口。
   - 无状态（0 捕获）：空 `<init>()V`，SAM 直接加载参数 `invokestatic` 转调 impl。
   - 捕获（N 捕获）：N 个 `private final` 字段 capI + 参数化 `<init>(capturedDescs)V`
     （super 后逐个 `aload_0 / xload / putfield`）；SAM 先 `aload_0 / getfield` 取回各捕获
     字段、再接 SAM 参数一起 `invokestatic`，按返回类型 `xRETURN`。
3. 站点改写：
   - 无状态：`new / dup / invokespecial <init>()V`。
   - 捕获：栈上 N 个捕获值先逆序 `xSTORE` 到新局部槽（bump `maxLocals`），再
     `new / dup / 顺序 xLOAD / invokespecial <init>(capturedDescs)V`。
4. **提权**：impl 目标若在本 index 非 library 类且 private，去 `ACC_PRIVATE` 置 public
   （否则合成类跨类 `invokestatic` 抛 `IllegalAccessError`）。
5. 合成类登记：遍历 `getClasses()` 结束后统一 `addClass`（单类 sidecar）/`upsertJarResource`
   （JAR），`isLibrary=true` 使 RegC 跳过编译、纯字节码写出。

**正确性要点**：改写后原方法不再含 INVOKE_DYNAMIC，自动脱离 `shouldKeepBytecodeForCompatibility`
的 fail-closed；合成类经 `FindClass`（同 classpath sidecar / jar entry）正常加载，不需
owner-class。展开后的 `new/dup/invokespecial` + 调用点 `invokeinterface applyAsInt` 走
RegC 已验证的通用 lowering。**踩出的 OutputWriter bug**：`isGeneratedRuntimeClass` 原只认
`mj2x/cloader/Loader`，合成的 `mj2x/lambda/*` 会被误当 primary 输入类（getClasses 是无序
map，多个合成类时随机把某个 Lambda$N 当 primary 写成 `_protected.class`，真类与 Loader 变
错位 sidecar → `NoClassDefFoundError: mj2x/cloader/Loader`）。修复：`isGeneratedRuntimeClass`
增认 `mj2x/lambda/` 前缀（sidecar 路径亦须相对 classpath 根，与 Loader 一致）。此 bug 对
无状态也潜在，只是之前 map 顺序碰巧没触发。

**测试**：
- e2e `InvokeDynamicStatelessLambdaRoundTrip`：`X::base` 方法引用单次/嵌套调用。
- e2e `InvokeDynamicCapturingLambdaRoundTrip`：`v->v+delta`（单捕获）、`v->a*v+b`（多捕获）、
  `v->(int)(scale*v)+bias`（long+int 混合类型捕获）、`(u,v)->u+v+c`（双 SAM 参数+捕获），
  protected native 与 Java baseline 逐行一致。
- 单元 `ExpandsStatelessLambda` / `ExpandsCapturingLambda`（indy 消失、NEW 出现、合成类入
  index、捕获字段 cap0 断言、私有目标提权断言）。

**剩余待办**：samMethodType≠implDesc 时的 bridge/装箱、altMetafactory（serializable/marker
interface）。实例/构造器方法引用已在 §16（阶段2c）落地。见 dev_plan R6.1。

## 16. R6.1 阶段2c：实例 + 构造器方法引用展开（2026-07-26，已完成）

把 §15 的 `H_INVOKESTATIC` 限制推广到全部方法引用 handle 形态。**关键洞察**：站点改写（把
indy 栈上捕获值搬进合成类构造器）**与 impl tag 无关**——捕获的是 indy 描述符参数，与 impl 的
调用形态正交。只有两处随 tag 变化：合成类 SAM 体的发射指令、`expandLambdaSite` 的校验分支。

**`buildLambdaClassNode` 按 `impl.getTag()` 发射 SAM**（fullArgs = capturedTypes ++ samArgs）：
- `H_INVOKEVIRTUAL` / `H_INVOKEINTERFACE`（实例引用）：receiver = fullArgs 首元。**bound**
  （`expr::m`）时 receiver 来自捕获字段（`GETFIELD cap0`）、**unbound**（`Type::m`）时来自 SAM
  首参（`xLOAD 1`）。receiver 天然首个压栈，故压栈序与静态调用一致，仅把 `INVOKESTATIC` 换成
  `INVOKEVIRTUAL`/`INVOKEINTERFACE`。
- `H_INVOKESPECIAL`（私有实例方法引用）：目标提权 public 后按 `INVOKEVIRTUAL` 发射（跨类
  `INVOKESPECIAL` 仅限 `<init>`/super，对无关类的普通方法非法）。super 方法引用（库超类目标、
  无法提权）仍 fail-closed。
- `H_NEWINVOKESPECIAL`（构造器引用 `Type::new`）：`NEW owner` / `DUP` / fullArgs /
  `INVOKESPECIAL owner.<init>` / `ARETURN`（返回新建对象；SAM 返回类型 = 该对象）。

**`expandLambdaSite` 校验改 tag 感知**：
- 静态 / 构造器：fullArgs 与 implArgs 逐一匹配（构造器额外要求 impl 名为 `<init>`、SAM 返回对象）。
- 实例：fullArgs[0] 须对象/数组 receiver，其余匹配 implArgs。
- 静态 / 实例额外加**返回类别一致性守卫**：`returnOpcode(samRet) != returnOpcode(implRet)` 即拒，
  挡住 samMethodType≠implDesc 的装箱/加宽桥接形态（避免发射会验证失败的字节码）。
- de-privatize 推广为「用户类**非 public** 目标（private/protected/包私有——含私有实例方法、包私有
  构造器）一律提权 public」。合成类在 `mj2x/lambda/` 包，跨包访问非 public 目标会 `IllegalAccessError`。

**关键正确性**：e2e 中 functional interface 必须 `public`——合成类在 `mj2x.lambda` 包，跨包实现
包私有接口运行期抛 `IllegalAccessError`（JVM 仅放行 public 跨包访问；这与 §15 私有 impl 提权同源）。

**测试**：
- e2e `InvokeDynamicMethodRefRoundTrip`：`String::length`/`String::charAt`（unbound 实例引用）、
  `base::concat`（bound 实例引用，receiver 捕获入字段）、`String::new`（构造器引用，`new char[]`
  参数）——impl 目标均为 JDK 方法（合成 SAM 纯字节码转调 JDK，不要求 impl 本身可 native 化），
  protected native 与 Java baseline 逐行一致。
- 单元 `ExpandsUnboundInstanceRef`（SAM 体 `INVOKEVIRTUAL`、无捕获字段）/ `ExpandsBoundInstanceRef`
  （cap0 字段 + `GETFIELD` + `INVOKEVIRTUAL`）/ `ExpandsInterfaceMethodRef`（SAM 体 `INVOKEINTERFACE`）/
  `ExpandsConstructorRef`（`NEW`/`INVOKESPECIAL <init>`、包私有构造器提权断言）+ **负例**
  `LeavesUnsupportedHandleUnexpanded`（`H_GETFIELD` 字段句柄仍 fail-closed、无合成类）。

**剩余待办**：samMethodType≠implDesc 的 bridge/装箱、altMetafactory、自定义 bootstrap（阶段3）。

## 17. R6.1 阶段2d：擦除泛型装箱/桥接适配（2026-07-26，已完成）

泛型 functional interface（`Function`/`Supplier`/`Consumer`/`Predicate`/`ToIntFunction`…）
经类型擦除后 samMethodType 形参/返回是 `Object`，与 impl 的具体引用/基本类型不同——
LambdaMetafactory 在运行期做的这类适配是 **javac 产出 lambda 的多数形态**，§15/§16 的
逐位描述符相等校验把它们整类 fail-closed。本阶段把适配静态展开进合成 SAM 体。

**适配子集**（`canAdaptValue`/`emitAdaptValue`，参数与返回统一走它）：
- 引用收窄：`CHECKCAST`（目标 `Object` 免 cast）。覆盖 unbound receiver（`Object→String`）、
  擦除参数、擦除返回（返回向发 `CHECKCAST samRet` 保证验证器通过）。
- 引用→基本类型：`CHECKCAST 包装类`（源已是包装类免）+ `xxxValue()` 拆箱。
- 基本类型→引用：`包装类.valueOf` 装箱；目标限 `Object`/对应包装类（javac 擦除只产出
  这两种），其余 fail-closed。
- SAM 返回 void：`POP`/`POP2` 丢弃 impl 返回值（`canAdaptReturn`/`emitReturnAdapt`）。
- **不支持**（仍 fail-closed）：基本类型互转（`i2l` 加宽等，须读 instantiatedMethodType
  定语义，MVP 不做）、impl 返回 void 而 SAM 要值。

**校验与栈账**：`expandLambdaSite` 逐位校验从描述符相等改为 `canAdaptValue`（同型是快速
路径，行为与 §16 完全一致）；捕获段与 SAM 参数段统一适配（捕获引用加宽 `String→Object`
顺带覆盖）。SAM 栈峰值按适配两侧较大槽宽累计（`Object→long` 拆箱把 1 槽变 2 槽），并修复
零参 SAM 调 `()J` impl 时 maxStack 被低估为 1 的既有边界（返回槽宽单独计入下界）。

**测试**：
- e2e `InvokeDynamicErasedGenericRoundTrip`：`Math::abs`（Function<Integer,Integer>，
  CHECKCAST+intValue 拆箱、valueOf 装箱返回）、`String::length`/`String::isEmpty`
  （ToIntFunction/Predicate，receiver cast）、`String::valueOf(long)`（Long 拆箱）、
  `String::new`（Supplier 构造器引用+擦除返回）、`x -> x + base`（捕获+擦除参数），
  6 站点全部展开（Lambda$0..$5），protected native 与 Java baseline 逐行一致。
- 单元 `ExpandsErasedStaticRefWithBoxing`（CHECKCAST/intValue/valueOf 断言）/
  `ExpandsErasedConsumerDroppingReturn`（POP+RETURN）/`ExpandsErasedInstanceRefReceiverCast`
  （receiver CHECKCAST）+ **负例** `LeavesPrimitiveWideningUnexpanded`（`(I)V` SAM 对
  `(J)V` impl 保持 fail-closed、无合成类）。

**剩余待办**：altMetafactory（serializable/marker interface）、自定义 bootstrap（阶段3）。

## 18. R6.1 阶段2e：altMetafactory serializable/marker/bridge（2026-07-26，已完成）

`javac` 在 lambda 需要 `Serializable`、额外 marker 接口（交集类型 `(Runnable & Serializable)`）
或桥接方法（泛型特化）时，改发 `LambdaMetafactory.altMetafactory`（而非 `metafactory`）。§15–§17
只认 `metafactory`，这些站点整类残留 → SSA fail-closed。本阶段补齐 alt 形态。

**bsmArgs 布局**：前 3 项与 metafactory 同（`[0]samMethodType [1]implHandle [2]instantiatedMethodType`），
随后 `[3]flags:int`，按 flag 位跟 `markerCount:int + Class markers…`、`bridgeCount:int + MethodType bridges…`。
`FLAG_SERIALIZABLE=1` 不带额外参。所有附加参经 `class_reader` 以 typed `Any` 呈现（int / Type(OBJECT
=CONSTANT_Class) / Type(METHOD=CONSTANT_MethodType)），无需改 libjbytecode。

**落地**：
- `isLambdaMetafactorySite` 认 `altMetafactory`；`parseAltMetafactoryTail` 解析尾部为
  `AltLambdaInfo{markerIfaces, bridgeDescs}`。出现未知 flag 位、缺计数、类型不符一律 false → fail-closed。
- `buildLambdaClassNode` 追加 marker 接口（含 `java/io/Serializable`）到 `cn->interfaces`——marker 无方法，
  SAM 体不变，只满足 `instanceof`/序列化标记语义。SAM 体发射抽成 `buildSam(name,desc,extraAccess)`，
  主 SAM 外为每个 bridge 描述符发一个 `ACC_BRIDGE|ACC_SYNTHETIC` 同名方法（复用同一 impl 适配路径，
  去重且跳过与主 SAM 重复的签名）。
- 校验抽成 `canEmitSam`（主 SAM 与各 bridge 逐个校验，任一不可适配整站点 fail-closed）。
- **序列化**：合成类实现 `Serializable` 后靠**默认序列化**其 `capN` 字段即可同进程 round-trip，
  不复刻 JVM 原生 `SerializedLambda` 格式（对混淆输出无意义，且跨 JVM 不可依赖）。

**测试**：
- e2e `InvokeDynamicSerializableLambdaRoundTrip`：`IntOp extends Serializable`，无状态方法引用
  `RegCSerLambda::base` + 捕获 lambda `v -> v + k`，各在同进程内 `writeObject`→`readObject`→invoke，
  protected 合成类与 JVM 原生 lambda 逐行一致。javac 实测发 4 站点（2 创建 + 2 在 `$deserializeLambda$`），
  全部展开。
- 单元 `ExpandsAltMetafactorySerializable`（追加 `java/io/Serializable`）/`ExpandsAltMetafactoryMarkerInterface`
  （追加 `java/lang/Cloneable`）/`ExpandsAltMetafactoryWithBridge`（主 SAM + 1 个 `ACC_BRIDGE` 同名方法）/
  **负例** `LeavesUnknownAltFlagsUnexpanded`（未知 flag 位 8 → fail-closed、无合成类）。

## 19. R6.1 阶段2f：基本类型加宽桥接（2026-07-26，已完成）

method reference 的实参/返回基本类型与 SAM 相差一个 JLS §5.1.2 加宽（`int→long/float/double`、
`long→float/double`、`float→double`）时，LambdaMetafactory 运行期插入 `i2l`/`i2f`/`i2d`/`l2f`/`l2d`/`f2d`。
§17 的负例 `LeavesPrimitiveWideningUnexpanded` 即此形态整类 fail-closed。本阶段静态展开。

**落地**：新增 `wideningOpcode(from,to)`（受支持加宽返回对应转换 opcode，收窄/boolean/同型返回 -1）。
`canAdaptValue` 对基本类型互转仅放行加宽（`wideningOpcode!=-1`）；`emitAdaptValue` 在 primitive→primitive
分支发对应转换指令。实参段（samArg→implArg）与返回段（implRet→samRet）复用同一适配，栈峰值按两侧较大槽宽
累计（`int→long` 1→2 已被 §17 的 `std::max(slotSize…)` 覆盖）。**装箱+加宽复合适配**（擦除引用先拆到
instantiated 包装类再加宽到 impl 基本类型，需读 `instantiatedMethodType`）与**收窄**仍 fail-closed。

**测试**：
- e2e `InvokeDynamicPrimitiveWideningRoundTrip`：`retI2L`/`retI2D`/`retL2D`/`retF2D`（返回加宽）+
  `argI2L`（首参加宽），自定义非泛型 functional interface 规避与 §17 装箱路径叠加，5 站点全部展开、
  protected 与 baseline 逐行一致。
- 单元 `ExpandsPrimitiveWidening`（SAM 体含 `I2L` + `INVOKESTATIC`）/**负例**
  `LeavesPrimitiveNarrowingUnexpanded`（`(J)V` SAM 对 `(I)V` impl 收窄 → fail-closed、无合成类）。

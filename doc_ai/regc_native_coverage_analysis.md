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
| S1 | `inner class method remains on bytecode path for compatibility` | 类名含 `$` | `regc_backend.cpp:233-241` | 设计如此 |
| S2 | `lambda body remains on bytecode path for invokedynamic compatibility` | 方法名以 `lambda$` 开头 | `:112-115` | 设计如此 |
| S3 | `large exception-table method remains on bytecode path` | `tryCatchBlocks.size() > 4`(**阈值 4**) | `:116-119` | 设计如此(阈值可调) |
| S4 | `invokedynamic remains on bytecode path for compatibility` | 块内有 `MIRKind::INVOKE_DYNAMIC` | `:120-125` | 设计如此(待展开) |
| S5 | `high-risk runtime call remains on bytecode path: <owner>.<name>` | INVOKE 命中黑名单 | `:126-133` | 设计如此 |

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
| high-risk call 黑名单 | **不可直接删**（见 §11，需先完善 call 引用管理） | 中（前置：call 路径引用管理） |
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

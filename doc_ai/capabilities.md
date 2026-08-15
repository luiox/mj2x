# mj2x — 已实现功能清单 (CAPABILITIES)

> 本文档列举 mj2x 当前**已实现且可用**的功能。
> 用法见 `doc/user_guide.md`;路线图见 `doc/roadmap.md`;待办见 `dev_plan.md`。
>
> 能力边界对应 `dev.3` 的实现(2026-07-06 冻结),权威范围见 `doc/mvp_release_scope.md`。

---

## 1. 输入 / 输出

| 能力 | 状态 | 验证 |
|---|---|---|
| 单 `.class` 输入 → 改写后 `.class` + emitted C | ✅ | StackC / RegC E2E |
| `.jar` 输入 → 改写后 `.jar` + emitted C | ✅ | `mj2x_unittest` JAR pipeline 测试 |
| RegC `.class` / `.jar` 输入 → emitted C++17 | ✅ | `CliTest.RealZigCompilesRegcCppClassAndRunsJvm` + CLI negative tests |
| LLVM IR：静态纯计算 / 对象 / 数组 / 调用 / 异常 / monitor 全 JNI 语义 → textual `.ll` + native library | ✅ v2.2 转正默认开启 | 构建开关 `with_mj2x_llvm`（默认 on）；显式 `=n` 时 CLI 不暴露 `--backend llvm`/`--emit-llvm`/`--llvm-tool`（fail-closed）；后端单测 `LlvmIrBackendTest.*` + `CliTest.Llvm*`（pipe/CMake/xmake 工程 + 综合 E2E：arraycopy/异常/monitor/ldc，clang 编译 → JVM 运行） |
| StackC 单 `.class` 输出附带 `mj2x-cloader` sidecar class | ✅ | StackCBackend E2E |
| StackC `.jar` 输出注入 `mj2x-cloader` dispatch loader class | ✅ | JAR pipeline / CLI 测试 |
| JAR 输出保留 entry 顺序和非 class 资源 payload | ✅ | JAR fixture 测试 |
| class entry 在 native method 注入后重新写回 | ✅ | JAR pipeline 测试 |
| `--native-annotation Lpkg/Anno;` 只翻译带目标方法注解的方法 | ✅ | CLI 注解筛选测试 |
| 提供 `mj2x.Native` Java 注解源码包 | ✅ | javac + CLI 注解筛选测试 |
| `--compile` 将 `--emit-c` 结果编译为同目录 native library | ✅ | CLI 编译错误/可用工具链 smoke 测试 |
| `--emit-cpp <file>` 输出 RegC C++17 JNI dialect，并通过 C ABI 链接 `mj2x-crt` | ✅ | Zig JVM E2E、MSVC direct compile、xmake/CMake project compile |
| Native source artifact contract（C99 / C++17 / LLVM IR language、content、建议文件名） | ✅ | StackC/RegC/LLVM artifact metadata 单测 + native E2E |
| `--compile-mode pipe` 直接编译 backend 内存源码；GCC/Clang/Zig 走 stdin，MSVC 使用自动清理的 temporary-file fallback | ✅ | `CliTest.PipeCompile*`、`CliTest.FakeZigUsesCcForFileAndPipeModes`、Windows `CliTest.RealZigCompilesClassAndJarAndRunsJvm` |
| `--generate-cmake <dir>` 输出独立 CMake native 工程 | ✅ | `CliTest.GenerateCMakeProjectEmitsStandaloneFiles` |
| `--generate-xmake <dir>` 输出独立 xmake native 工程；与 `--compile` 同用时直接 configure/build 并复制产物；`--c-compiler xmake` 使用自动清理的临时工程 | ✅ | `CliTest.GenerateXmake*`、`CliTest.XmakeCompiler*` + golden |
| xmake 临时工程清理失败恢复 | ✅ | build 失败时只读 `native.c` 自动恢复属性并清理，无 native library、源码或 JAR 内嵌资源残留 |
| StackC/RegC class 和 jar 重复生成稳定性 | ✅ | `CliTest.ClassInputRepeatableForStackCAndRegC`、`CliTest.JarInputRepeatableForStackCAndRegC`、`tools/product_sample_smoke.ps1 -Mode fast` |

---

## 2. StackC(广覆盖正确性后端)

定位:正确性基线后端,覆盖广,输出仍是 stack-style(`union jvalue stack[]` + `sp`)。
Native 绑定使用 `static` C 函数 + `JNI_OnLoad` 预注册 `mj2x-cloader`
dispatch bridge + `RegisterNatives`,业务方法和注册桥均不产生 per-class `Java_*`
导出符号。单 `.class` 输出在目标目录旁写出 `mj2x/cloader/Loader.class`,JAR 输出注入
同名 loader class。生成方法体内的业务类解析使用当前 owner class 的 `ClassLoader`，覆盖
自定义 class loader 下 `new` / `anewarray` / `multianewarray` / 静态成员 / type check
等路径，bootstrap 异常类和注册入口仍使用 JVM `FindClass`。生成方法体跟踪临时 JNI
local refs，并在正常返回或异常提前返回前批量释放；对象返回值作为 keep ref 保留。
字段和方法 ID 在各生成调用点用 `static jfieldID` / `static jmethodID` 懒缓存，读写用
**无锁 CAS + 快路径**（`_mj2x_cache_load` 无锁读 + `_mj2x_cache_cas` 发布，CAS 输家复用
赢家结果；MSVC 走 `InterlockedCompareExchangePointer`，GCC/Clang/Zig 走 `__atomic`，
平台宏定义在 runtime prelude），不再持有 owner class monitor（等价 JNIC 3.7.0 的无锁
缓存结构）。异常检查 / NPE trap / 慢路径全部带 `_MJ2X_UNLIKELY` 分支预测提示
（MSVC 下退化为普通 if，GCC/Clang/Zig 下为 `__builtin_expect`）。hidden proxy class
cache 使用 `jweak` 弱全局引用,调用前通过 `NewLocalRef` 提升。字符串 LDC 常量去重到
TU 级 `_mj2x_string_pool[]`（**UTF-16 code units，`uint16_t` 数组**，方法体按
`NewString(env, (const jchar*)(pool + offset), len)` 物化，防 strings 扫描明文命中；
等价 JNIC 3.5.1/3.7.0 stringObf=false 形态），方法体只引用 pool offset。

**已覆盖的 javac 输出形状**:

- 算术:整数 / 长整型 / 浮点运算
- 类型转换:`i2b` / `i2s` / `i2c` / `i2l` 等窄化与拓宽
- 控制流:`if*` / `goto` / switch (`tableswitch` / `lookupswitch`)
- 字段读写:`getfield` / `putfield` / `getstatic` / `putstatic`
- 对象:`new` / `checkcast` / `instanceof`
- 方法调用:`invokevirtual` / `invokespecial` / `invokestatic` / `invokeinterface`
- interface default 方法:原 default 方法保留 Java stub,调用 `DefineClass` 注入的 hidden
  proxy class;native 只注册 proxy 方法,不直接把 interface default 标成 `ACC_NATIVE`
- 数组:`newarray` / `anewarray` / `multianewarray` / `arraylength` / `*aload` / `*astore`;
  `BALOAD`/`BASTORE` 对 `byte[]`/`boolean[]` 做运行时类型分流
- 栈形状操作:`POP` / `POP2` / `DUP` / `DUP_X1` / `DUP_X2` / `DUP2` /
  `DUP2_X1` / `DUP2_X2` / `SWAP`;StackC 通过转换期类型快照区分 category-2
  与 category-1 pair
- 异常:`athrow` / try-catch / finally / monitor (`monitorenter` / `monitorexit`)
- 显式 NPE trap:field / array / instance invoke 在 JNI 调用前主动抛
  `NullPointerException`,并进入当前 catch 分派或按返回类型传播
- int/long DIV/REM 除零:`ArithmeticException` trap,含 catch 分派与未捕获传播
- owner ClassLoader-aware 类解析:应用类查找走 `_mj2x_find_class(env, _mj2x_owner_class, ...)`
- JNI local-ref 追踪:临时 class/string/object/array refs 登记到方法级列表，返回前释放
- JNI method/field ID 懒缓存:调用点首次解析,owner class monitor 保护缓存读写,后续复用缓存 ID
- hidden proxy class cache:使用 WeakGlobalRef,不因 native cache 强持有阻止类卸载
- String pool:重复 LDC 字符串只落一份 `_mj2x_string_pool[]` 数据，方法体按 offset 物化 jstring
- StackC `invokedynamic`:native 侧调用 bootstrap,经 `CallSite.getTarget()` +
  `MethodHandle.invokeWithArguments` 执行;已覆盖 javac string concat、方法引用和捕获 lambda

**验证**:28 个 StackCBackend 真 E2E(Java → mj2x → C → DLL → Java 行为对比)和 StackCBackend
发射单测。

**已知限制**:

| 限制 | 说明 |
|---|---|
| 栈全用 `union jvalue` 数组 | 性能低于寄存器方案,后续可优化 |
| RegC `INVOKEDYNAMIC` 分阶段 | string concat（阶段1，StringBuilder 字节码）+ 无状态/捕获 lambda（阶段2）+ 实例/构造器方法引用（阶段2c）+ 擦除泛型装箱/桥接（阶段2d，CHECKCAST/拆装箱/POP 适配）+ altMetafactory serializable/marker/bridge（阶段2e）+ 基本类型加宽桥接（阶段2f，i2l/i2d/l2d/f2d）已 pre-SSA 展开转 native;自定义 bootstrap（阶段3，任意 bootstrap 方法）由 SSA 登记站点 + finalize 生成运行时 call-site helper（MethodHandles.lookup/privateLookupIn + MethodType + bsmArgs 重建 + CallSite.getTarget 全局缓存 + invokeWithArguments），2026-08-09 已实现;基本类型收窄/装箱+加宽复合仍 fail-closed |
| JSR / RET 不纳入 | 废弃字节码,由 preprocessor / 输入规范处理 |
| `<init>` / `<clinit>` 搬运法 | 空构造器骨架化（super + 调 `__regc_init_<hash>`），有逻辑构造器保留原字节码（注入方法会被反射方法数断言计数，业界同此策略），`<clinit>` 搬运到 `__regc_clinit_<hash>`；详见 `regc_codegen.md` §9 |

---

## 3. RegC(保护导向无栈后端)

定位:保护导向后端,抗 c2j 分析。流水线:L1 stack MIR → SSA → RegIR → RegC。

**结构断言(输出保证)**:RegC 输出不出现 `cstack` / `stack[]` / `union jvalue` / `sp--` 这类 stack-machine anchor。

**JNI local ref 引用管理**:每个 native 方法用 JNI `PushLocalFrame`(入口) / `PopLocalFrame`(单出口 `_mj2x_regc_cleanup`，REF 返回值 promote 到父帧、非 REF 传 NULL)批量释放方法内产生的 local ref，避免反射等密集调用撑爆 JNI 16 槽 local ref 表。零 C 数组、不引入 stack anchor；纯标量方法不发射帧。缓存 class/method/field 走 `NewGlobalRef` 不受帧影响。REF 相等比较(`if_acmpeq/ne`、与 null 比较)使用 `IsSameObject` 而非裸 `void*` 指针比较。

**invokedynamic 分阶段展开(R6.1)**:`InvokeDynamicPreprocessor` 在 SSA 前把可静态解析的 indy 改写成 RegC 已能 lower 的普通字节码；阶段3（2026-08-09）起自定义 bootstrap 不再整方法保留字节码，由 SSA 登记站点 + finalize 生成运行时 call-site helper（见 `regc_invokedynamic_strategy.md`）。已展开：string concat（StringBuilder 链）、无状态/捕获 lambda、实例/构造器/接口方法引用、擦除泛型装箱/桥接、altMetafactory（serializable/marker/bridge）、基本类型加宽；自定义 bootstrap 运行时解析已实现。仍 fail-closed：基本类型收窄、装箱+加宽复合。展开形态与边界见 `regc_codegen.md` §转换边界。

**覆盖率限制(重要)**:RegC 对部分方法 fail-closed 保留 Java 字节码(显式 skip + 可观测 fail-closed),实测复杂 jar(JavaObfuscatorTest `TEST.jar`)业务方法（非构造器）已 **100% 转 native（零 skip）**。未转的合法情形：有逻辑构造器（局部变量表非空——注入方法会被反射方法数断言计数，业界同此策略）、含 try-catch/局部变量注解/跨段 label 引用的构造器、抽象方法、mj2x runtime 类自身。历史 fail-closed 分组（反射组、IO 组、类加载组、并发组 `Thread`/`java.util.concurrent/*`、System 组 `exit`/`SecurityManager`、invokedynamic 自定义 bootstrap）均已放开（引用管理 + thread-local env/ctx + 阶段3 实现）。**内部类 `$`** 注册已随 RegisterNatives 撤销,`this$0` 合成字段访问与外部类方法调用路径端到端通过。浮点/long 三值比较(`cmp.nan_*` intrinsic)已实现。大异常表(多 catch)已支持(lowering 三层无上限)。fail-closed 点通过 `[RegCBackend] Skipping <method>: <stage> failed: <reason>` 日志暴露,不再静默。**性能评估必须先做覆盖率审计**,见 [performance_evaluation_spec.md](performance_evaluation_spec.md) 与 [regc_native_coverage_analysis.md](regc_native_coverage_analysis.md)。

- **RegIR dead-code elimination**:在 RegIR→RegC 之间迭代删除未被终结器、CFG edge copy 或异常边消费的纯计算链；可能抛异常、内存访问、调用与 barrier 一律保留。`RegDceTest.*` 覆盖死链删除和 edge copy 活跃值保留，RegC native E2E 覆盖 JVM 语义。
- **RegIR protection transforms**:RegC 在 DCE 后应用 i32/i64 加法 MBA 恒等式、分支 opaque predicate 与纯控制流 CFF。CFF 通过状态寄存器与 dispatcher switch 路由边，同时保留 edge copy；包含异常区域、内存访问、调用、barrier、throw 或 div/rem 的函数保持原 CFG。`RegTransformPassTest.*` 覆盖变换结构与保守跳过，`RegCBackendE2E.PureArithmeticLoopAndArrayRoundTrip` 覆盖 JVM 循环/数组语义。
- **safe-env（JNI 函数表保护，#748）**：`--regc-safe-env` 开启后，JNI_OnLoad 首行捕获并做归属校验（表项必须位于 jvm.dll/libjvm.so 代码段，豁免 HotSpot `UseFastJNIAccessors` 的 8 个字段 getter JIT stub），每个 protected native 入口首行内容校验 + 原子恢复被 hook 的函数表。防 `SetJNIFunctionTable` 整表替换、原地改写（指针不变）与单 entry 改写；启动期 agent 污染快照时默认 fail closed（JNI_OnLoad 返回 JNI_ERR），`--regc-safe-env-debug` 改为告警观察。inline detour 与 JVMTI 事件层在射程外（见 `doc_ai/regc_safe_env.md`）。验证：CRT 单测 `test_safe_env` + `RegCSafeEnvE2E.*` 三个真 E2E（运行时原地改写恢复/对照组、启动期污染硬失败、debug 模式继续），E2E 全程走 `ca::process::Command` 无临时 bat。

**已覆盖的 E2E 形状**:

- 纯计算(算术、转换)
- 分支 / 循环
- switch
- 字段读写
- 对象创建与操作
- 方法调用
- 数组操作
- 窄类型(`i2b` / `i2s` / `i2c`)
- throw
- try / catch
- null trap
- integer div / rem trap(走 R3 runtime ABI)
- generated C TU 级 static/instance method、static/instance field、class cache CSE:跨方法复用相同 JNI ID/class cache declaration;String intrinsic class cache、固定异常类 class cache、`new`/reference-array allocation 的编译期 descriptor class cache 也统一入池。动态 descriptor 与 runtime `class_name` 不进入全局 cache，避免破坏 class-loader identity 和类卸载语义；决策理由见 `regc_codegen.md` §6
- **JDK intrinsic(Math / Integer/Long 位操作 / wrapper parse/toString/compare/value-unbox / Random next* / String UTF-16 hot paths / mutable String CRT 链)**:RegC 在 Call 分支识别 JDK 方法签名,匹配调用点不发 `_mj2x_regc_call_*` 通用 JNI helper,直接调用 CRT ABI 或 TU-local cache helper

**JDK intrinsic 覆盖(初版,匹配调用点无通用 JNI helper 派发)**:

- `java.lang.Math`:abs/min/max(int/long/float/double)、sin/cos/tan/sqrt/pow、floor/ceil/round、random
- `java.lang.Integer/Long` 位操作:rotateLeft/rotateRight、reverse/reverseBytes、bitCount、numberOfLeadingZeros/numberOfTrailingZeros、highestOneBit/lowestOneBit、signum——纯 C 计算(`mj2x_intrinsic_{int,long}_*`),语义与 OpenJDK 一致(distance 按 32/64 取模、无符号旋转/反转、符号位仅扩展在返回值),CRT 实现与 JDK 交叉验证(`mj2x-crt/test/mj2x_regc_intrinsic_test.c`);`RuntimeFunction` emitKind 纯计算 intrinsic 发射为纯 C 表达式并省略 ExceptionCheck(异常检查稀疏化,见 `regc_codegen.md` §4)
- `java.lang.Integer.parseInt(String[,radix])`、`Long.parseLong(String[,radix])`、`Short.parseShort(String[,radix])`、`Byte.parseByte(String[,radix])`:native digit parser 解析并检查目标类型范围,NumberFormatException 经 context 回调路由
- `java.lang.Boolean.parseBoolean(String)`:按 UTF-16 ASCII 大小写不敏感匹配 `true`;`Float.parseFloat(String)` / `Double.parseDouble(String)` 通过专用 TU 级缓存调用 JDK parser,保留 NaN、Infinity、hex float 与异常语义
- `java.lang.Integer/Long/Short/Byte/Float/Double/Boolean/Character.toString`、`java.lang.Integer/Long/Short/Byte/Float/Double/Boolean/Character.compare`:无状态 compare 与基础整型/布尔/字符 `toString` 走 `mj2x-crt` C ABI;`Float/Double.toString` 复用缓存的 JDK `String.valueOf(F/D)` 保持浮点文本格式,`Float/Double.compare` 显式处理 NaN 与 signed zero 语义
- `java.lang.String.valueOf(int|long|boolean|char|float|double|Object)`:RegC local helper 覆盖常见值转字符串;`float|double` 和 `Object` 通过缓存的 JDK methodID 保持浮点格式和虚分派语义
- String LDC:经 `_mj2x_intrinsic_string_literal` 物化为 jstring,供 intrinsic 路径消费
- `java.lang.String`:equals/length/charAt/hashCode/isEmpty/startsWith(String)/endsWith(String)/contains(CharSequence),以及 `concat`、`substring`、`compareTo`、`indexOf(int|String[,fromIndex])`、`replace(char,char)`、`trim`、`split(String[,limit])`。`split` 仅对非空且无正则元字符的 delimiter 走 native UTF-16 fast path,完整正则、空模式与转义模式回退 JDK;`contains` 对非 String `CharSequence` 同样回退缓存的 Java methodID
- `java.lang.StringBuilder`:`new`/`<init>()`/`<init>(String|CharSequence|capacity)` +
  `append(String|Object|CharSequence[range]|int|long|boolean|char|char[][range]|float|double)` + `appendCodePoint`、`length/capacity/ensureCapacity/trimToSize/setLength/charAt/setCharAt/toString`,由 CRT opaque UTF-16 state 承载并在统一 cleanup 销毁。
  返回、未知调用、字段/数组存储和 CFG alias 会按需 materialize 为真实 Java 对象;逃逸后仍继续修改的链从创建点整体回退 Java,避免 native/Java 状态分叉
- `java.lang.StringBuffer`:复用 StringBuilder 的 opaque UTF-16 storage 与相同 range/capacity 常用 API,但通过独立 `mj2x_string_buffer_*` C ABI 暴露同步语义。Windows 使用可重入 `CRITICAL_SECTION`,POSIX 使用 recursive pthread mutex;materialize 生成真实 Java `StringBuffer`
- 包装类 value/unbox:`Integer/Long/Short/Byte/Float/Double/Boolean/Character.valueOf`
  后接可证明安全的对应 `xxxValue` 时使用 native shadow;返回、字段/数组存储或未知调用等逃逸链从 `valueOf` 起回退真实 Java wrapper
- `java.util.Random` 非逃逸 new/init/next*:`mj2x_random` shadow state + runtime LCG,
  nextInt/nextLong/nextBoolean/nextFloat/nextDouble 不发 JNI call

**intrinsic 限制(当前)**:Random shadow state 仍只支持非逃逸路径。StringBuilder/StringBuffer 仅覆盖 registry 中的常用构造、append 与 range/capacity API;未登记方法正常走 Java fallback。`String.split` 的 native fast path 不实现正则,所有正则语义由 JDK 保持。

**RegC 性能优化(2026-08,issue #778/#780)与对比基线**:

- **异常检查稀疏化(#778 P2)**:纯计算 intrinsic(`RuntimeFunction` emitKind:位操作/Math 等)发射为纯 C 表达式、不经过 JNI、不可能抛异常,异常分发前复判省略 ExceptionCheck;Arithmetic 热循环探针 66x→2x
- **resolved native dispatch 双槽缓存(#780)**:站点级缓存按 receiver 精确类持有 `{method, fn, implClass, receiverClass}` global-ref 组;refresh 只写槽 1、槽 0 保留上次命中,A/B 交替 receiver 时两槽各自命中避免每轮 refresh(每轮 5-6 个 JNI),命中槽 1 时交换让热 receiver 留在槽 0
- **JDK 17/21 矩阵 vs JNIC 竞品(os30)**:13-probe jvm-obf-tester 总耗时 JDK17 2.25x→1.16x(122ms vs 105ms,Control flow 0.87x 已反超),JDK21 1.56x(157.7ms vs 101.3ms);报告见 `benchmark/results/2026-08-13-jvm-obf-tester-os30.md`(含 javap 覆盖率审计:RegC native 74.7% vs os30 61.6%)

**CLI**:

- `--backend regc`,支持 `.class` 与 `.jar` 输入输出路径。
- `--backend llvm --emit-llvm <file.ll>` 输出 textual LLVM IR（非 LLVM SDK 依赖，`clang -shared` 或 `llc -> clang` 生成 native 库）。支持：
  - 标量运算与类型：`int`/`long`/`float`/`double`/`void` 全运算（移位、整数除/取余——除零抛 `ArithmeticException`、`INT_MIN/-1` 语义正确）、`boolean`/`byte`/`short`/`char` 窄类型（i8/i16 边界 sext/zext、返回值截断）、比较（REF 用 `IsSameObject`）、浮点→整数（`llvm.fptosi.sat`）、常量折叠；
  - 对象模型（全走 JNI，零 JVM 内部依赖）：对象分配 `AllocObject`、`<init>` 经 `CallNonvirtualXxxMethodA`、实例/静态字段 Get/SetXxxField、`checkcast`/`instanceof`（`IsInstanceOf`）、`ldc String`（`NewStringUTF`）/`ldc Class`（`FindClass`）；
  - 数组：分配 NewXxxArray/NewObjectArray/多维递归、length、读写 Get/SetXxxArrayRegion 与 Get/SetObjectArrayElement、`boolean[]` 经 `[Z` 判定走 jboolean 0/1 归一化、`System.arraycopy` 经 JNI 保异常语义；
  - 调用：编译索引内已转换 invokestatic 目标 direct call（0 JNI）；实例/外部/未转换目标经 `CallXxxMethodA`/`CallNonvirtualXxxMethodA`/`CallStaticXxxMethodA`（jvalue 数组，null receiver 抛 NPE）；实例方法 this 为 SSA 参数 0，wrapper 透传 jobject this；
  - 异常：`athrow`（JNI `Throw`，null → NPE）、try/catch/finally 异常表分发（`ExceptionCheck` → `ExceptionOccurred`+`ExceptionClear` → 按表序 `IsInstanceOf` 匹配 → handler；catch-all 重抛语义）；
  - monitor：`monitorenter/monitorexit` → `MonitorEnter/Exit`（null → NPE）；
  - fail-closed：未初始化局部（undef Opaque）、未知 intrinsic、`String` 之外的 Handle 常量等保持字节码，调用方对不可转换目标走 JNI fallback（不再整链拒绝）。
  - `--llvm-tool clang|llc` 选择直接 clang 编译或 `llc -> clang` 链接；`--compile-mode pipe` 经 clang stdin 编译；`--generate-cmake` / `--generate-xmake` 生成含 `native.ll` 的工程。**v2.2 转正**：构建开关 `with_mj2x_llvm` 默认 on（v2.1 曾按发布范围关闭）；显式 `=n` 时 CLI 不暴露相关选项（fail-closed）。
- `--native-annotation <desc>` 启用方法级筛选,descriptor 使用 JVM 内部注解格式(如 `Lmj2x/Native;`)。未指定时仍使用 `MarkAllMethodsPreprocessor` 默认标记所有可翻译方法。
- `mj2x/mj2x-annotations/src/main/java/mj2x/Native.java` 提供用户可直接引入的 `@mj2x.Native` 注解源码。
- `--compile` 在 `--emit-c`、`--emit-cpp` 或 `--emit-llvm` 之后自动调用 native 编译器生成 `.dll` / `.so` / `.dylib`,输出到 `--output` 同目录;C++17 分别使用 `cl`、`g++`、`clang++` 或 `zig c++`，并将 RegC CRT 保持为 C ABI。LLVM IR 由 clang 或 `llc -> clang` 编译，不使用 `--c-compiler`。
- `--regc-trace-calls`、`--regc-safe-env`、`--regc-safe-env-debug` 均要求 `--backend regc`；safe-env 默认关闭，开启才产生运行时开销
- **RegC 混淆变换默认全部关闭**（基础三项 MBA/opaque/CFF 默认关；RegC 定位高性能 native 生成器，默认产物语义直译零混淆）。显式开启：
  - `--regc-obfuscate`：开启基础三项（MBA + opaque 谓词 + CFF 平坦化）。
  - **#829 增强（Cloakwork 借鉴，默认关闭，确定性、同 seed 同产物）**，各自联动开启对应基础项：
    - `--regc-cff-dead <n>`：CFF 恒假死块注入密度（每 N 个 dispatcher 跳转插 1 死块；隐含开启 CFF）。死块条件 `(state*state+1)==0`（模 2³² 无解恒假、依赖运行时 state 不可被编译器证明），内容夹 `mj2x_regc_barrier`（副作用语义）→ MSVC/GCC O2 下不可 DCE（已验证 DLL 存活）。
    - `--regc-cff-state-encrypt`：派生随机 state 码（splitmix64(seed,index) 替代顺序 1,2,3）+ dispatcher 入口状态异或（case 常量 ≠ 写入值）；隐含开启 CFF。
    - `--regc-mba-deep`：Add 展开为 De Morgan/进位恒等链（per-site 按 seed 选变体），链中夹 barrier 防折叠；隐含开启 MBA。
    - `--regc-opaque-zero`：opaque 恒真谓词变体（demorgan `~(~x)==x` / shift-pair / or-dec，零 UB 位运算）；隐含开启 opaque。
    - `--regc-transform-seed <n>`：变换种子（state 码/变体选择），默认 0。
- **native-to-native 调用默认走 JNI（#814/#816）**：已转换目标间的 direct C 调用不产生 JVM 栈帧，`Throwable.getStackTrace` / `StackWalker` / 反射的栈观察语义会失真（regc 2.6 ReTrace 实证）。默认 `CodeOptions.directCalls=false`，所有调用经 JNI `Call*MethodA` 保帧；`--regc-direct-calls` 显式开启直调（性能优先且确认无栈观察的产物）。实测 obf.jar 总耗时影响约 1%
- `--compile --compile-mode pipe` 不要求 `--emit-c`。GCC/Clang/Zig (`zig cc`) 严格走 stdin；MSVC 因 `cl`/`c1` 不接受 stdin 或命名管道源路径，使用正常返回路径必清理的临时 `.c` fallback，不承诺严格 no-write。
- 编译阶段从 `JAVA_HOME` 推导 JNI include 路径;Windows/MSVC 环境未初始化时会尝试调用 Visual Studio 2022 `vcvarsall.bat`;`JAVA_HOME` 缺失、JNI include 缺失、`vcvarsall.bat` 缺失或编译器退出失败时返回非零并打印明确错误。
- `--generate-cmake <dir>` / `--generate-xmake <dir>` 保留 `native.c` 或 `native.cpp`；RegC 项目同时复制 `mj2x-crt`，CMake 通过 `find_package(JNI REQUIRED)` 发现 JNI headers。LLVM IR 同样支持两条工程生成路径（生成含 `native.ll` 的 CMake/xmake 工程，clang 编译为 native 库）。

---

## 4. VMC4(VM 虚拟化后端)

定位:VM 虚拟化后端(保护导向,性能非关键)。流水线:L1 stack MIR → SSA → RegIR →
VMC4 VM 编码。RegIR 编码为 per-method VM 字节流(编译期 opcode 随机化,seed 驱动、
per-class/method 粒度、ALT 别名、super-op、bogus handler),解释器 4 种 dispatcher
形态(switch_table / func_ptr / if_nest / state),执行期 dispatch drift
(`--vmc4-drift <0..4>` + `--vmc4-drift-period`,语义标签按字节偏移周期轮转,
语义绝对安全——unmap 是纯函数,canonical handler 不变;issue #762 低风险档,PR #766)。

**调用/字段/数组/分配**:per-callsite 独立 JNI helper(懒缓存 class/method/field ID +
按 argKinds marshalling + Call[Static]<Type>MethodA),经 `mj2x_vm_call_table_<tag>[]`
函数指针表分派;invokespecial → CallNonvirtual,invokevirtual/interface → JVM 虚分派;
字段 accessor 按声明类型精确选择;null receiver/数组显式 NPE;越界交 JNI 抛 AIOOBE;
MULTIANEWARRAY 共享递归 helper 按 JVM eager 语义逐层分配;String LDC 物化为常量槽
(wrapper NewStringUTF 预加载)。

**异常语义**:编码期把异常区域静态展开为 VmExceptionRegionEntry(handler/caughtType/
exceptionReg + 边拷贝),dispatcher 循环底部 ExceptionCheck → ExceptionOccurred →
按块线性扫描活跃条目 → IsInstanceOf 匹配 → 边拷贝 + 异常写 exceptionReg;未匹配
重新 Throw 挂起 + status=1 传播。除零 ArithmeticException、athrow(null → NPE)。

**intrinsic 子集**:array.length / cmp.nan_* / instanceof:<desc> / monitor.enter|exit
按名称泛化分派(per-intrinsic helper);Math/wrapper 走普通 Call(JNI);与 RegC 双后端
对拍(`MathIntrinsicDualBackendParity`)。

**float 精度(JVM 语义)**:FADD/FSUB/FMUL/FDIV/FREM 与 SuperInc/SuperDec 的 float
分支按 float 精度运算并正确舍入回 float 存槽(slot 是 double——不做 float 舍入会
引入 double 中间精度,比较/再次运算差 1 ULP;issue #814/#816 修复)。

**转化率(v2.2 可用版)**:真实 jar(jvm-obf-tester)业务方法 100% 转 VM;
VMC4 单测 45 + e2e 56 全绿(4 dispatcher × drift 全开,`-Xverify:all` round-trip);
转化率报告基建 `test/mj2x/vmc4_conversion_report.md`。

**CLI**:`--backend vmc4` + `--vmc4-dispatcher` / `--vmc4-seed` / `--vmc4-granularity` /
`--vmc4-alias` / `--vmc4-bogus` / `--vmc4-drift` / `--vmc4-drift-period` /
`--vmc4-trace-surface`;`--runtime-protection` 检查点注入通用(RegC/VMC4)。

**已知限制**:MULTIANEWARRAY 初始 fail-closed 已放开;仍 fail-closed:反射、动态类加载、
invokedynamic 自定义 bootstrap 等与 RegC 相同的边界。性能非目标。

---

## 5. R3 Runtime 基础

| 模块 | 状态 |
|---|---|
| runtime core(crypto / kernel / stringpool / antidebug / antijvmti) | ✅ 单测模块 |
| `mj2x-crt` 可分发 RegC ABI | ✅ C99/JNI source pack + `mj2x_crt_test`;包含 Math、Random 和非逃逸 primitive wrapper intrinsic；无 `libca` 分发依赖 |
| RegC / R3 ABI 冻结 | ✅ 当前覆盖 div / rem / barrier、compare、基础 `toString`、UTF-16 String/split 与 opaque StringBuilder/StringBuffer helper |
| runtime-backed div / rem trap | ✅ RegC native E2E 覆盖 |
| **JDK intrinsic runtime**(intrinsic/wrapper、intrinsic/math、intrinsic/random) | ✅ 单测模块,RegC native E2E 覆盖 |
| Math intrinsic(abs/min/max/sin/cos/tan/sqrt/pow/floor/ceil/round/random) | ✅ runtime + emitter 识别 |
| wrapper parse / toString / compare intrinsic | ✅ Integer/Long/Short/Byte radix parser、Boolean parser、Float/Double JDK parser cache + E2E;无状态 compare/基础 `toString` 由 CRT 提供 |
| 包装类 native struct(Integer/Long/Short/Byte/Float/Double/Boolean/Character)valueOf/xxxValue | ✅ 可证明安全的 value/unbox 使用 native shadow;逃逸链回退真实 Java wrapper |
| Random Java 兼容 LCG(nextInt/nextLong/nextDouble/nextBoolean/nextInt(bound)) | ✅ runtime + RegC 非逃逸 new/init/next*;逃逸 fail-closed |
| String UTF-16 intrinsic | ✅ read hot paths + concat/substring/compareTo/indexOf overloads/replace/trim/split;split 字面量 fast path 与 JDK regex fallback 均有 native E2E |
| StringBuilder/StringBuffer opaque CRT state + materialize | ✅ range/capacity 常用 API、escape-aware materialize/fallback、StringBuffer 可重入平台锁与并发 CRT 测试 + native E2E |
| antidebug live probe | ✅ Windows IsDebuggerPresent/PEB/NtGlobalFlag/NtQuery debug port/object/flags/硬件断点 + mockable 软件断点扫描；**Linux**（v2.2）：TracerPid（/proc/self/status 文本谓词）、PTRACE_TRACEME 自检（seccomp 限制不误判）、maps 可写可执行段、单调时钟时间差（弱信号不进聚合） |
| antijvmti live probe | ✅ Windows Toolhelp/Linux proc maps 模块枚举 + JavaVM/JVMTI capability query + agent/JDWP/dump 信号聚合；**参数自动收集**（v2.2）：Windows GetCommandLineW 引号感知切分 / Linux /proc/self/cmdline |
| runtime detection policy | ✅ 聚合 debug/agent/probe-failure 报告并显式返回 fail-closed 判定,不在探针内主动退出 |
| **enforcement 反制响应层**（v2.2） | ✅ LogOnly/Delay/FakeData/Terminate 可配置 action + 多阶段检查点（step 确定性下限 + xorshift 随机提前命中 + force 语义）；Terminate 返回 outcome 由集成层执行 |
| **rt_protect 生成代码检查点**（v2.2） | ✅ mj2x-crt C99 自包含检查点（随 resources.manifest 分发）；RegC/VMC4 方法入口注入（CLI `--runtime-protection none\|log\|delay\|fake\|terminate` + step/delay，默认不注入零产物变化）；真实 MSVC 编译链验证 |

---

## 6. 项目结构 + codegen 工具层

**模块化目录**(对齐 `libs/libjbytecode/<module>/` 子库模式):

```
mj2x/
├── mj2x-core/src/mj2x/      核心库(include 根 = src,内层 mj2x/ 保留 → 98 处 include 零改动)
├── mj2x-cli/src/             CLI 入口
├── mj2x-crt/src/             可分发 C99/JNI RegC ABI source pack
├── mj2x-runtime/src/         in-tree native runtime(crypto/kernel/intrinsic/...)
├── unittest/src/             单测
```

| 能力 | 状态 |
|---|---|
| codegen 工具层(`mj2x-core/src/mj2x/codegen/`) | ✅ CSourceStream(缩进管理 + `appendLineStart` 链式)、IncludeSet(include 去重)、cStringLiteral/mangle 收口 |
| StackC 拆到 `backend/stackc/` 文件夹 | ✅ stackc_backend + stackc_runtime_prelude 分离 |
| 重复 helper 收口(cStringLiteral/mangle 跨 StackC/RegC) | ✅ 单一来源在 codegen |
| 日志门面（libca log + SimpleLogBackend） | ✅ 全部模块日志走 `CA_LOGT_*`（stderr 彩色、级别/模块/位置）；CLI `--quiet`/`--verbose` 控制级别；`mj2x::initLogging` 供库使用者 |

**codegen 工具层设计**:替代裸 `std::ostringstream << "    ..."` 字符串拼接。`CSourceStream`
管缩进、`IncludeSet` 管 include 去重排序、`c_helpers.hpp` 是无状态 helper 的单一来源。
两个 C 发射后端(StackC/RegC)共用。**StackC 发射器已全量迁移到 CSourceStream**：
`stackc_backend.cpp` 单体(3300+ 行)按领域拆分为 `stackc_mir_arith.cpp` / `stackc_mir_objects.cpp`
/ `stackc_mir_invoke.cpp` / `stackc_exception.cpp` / `stackc_registration.cpp` /
`stackc_snippets.cpp` + 内部共享头 `stackc_backend_internal.hpp`（辅助结构 + inline helper
+ `Impl` 完整定义），行级文本一律走 `emitLine`/`appendLineStart`，不出现手写空格拼接。
`fnv1a64` / `hex64` / `nativeFunctionName` / `registerFunctionName` 收口到
`c_helpers.hpp`（RegC 同源共用），浮点字面量走 `formatFloatLiteral`（`%.*g`，与
`setprecision` 默认格式同规则，不依赖 ostringstream/iomanip）。

## 7. 能力索引

| 能力域 | 当前权威来源 |
|---|---|
| 字节码转 native | `mj2x/mj2x-core/src/mj2x/backend/{stackc,regc}/` |
| 可分发 RegC 运行时 ABI | `mj2x/mj2x-crt/src/regc/` + `mj2x/mj2x-crt/resources.manifest` |
| native 运行时 | `mj2x/mj2x-runtime/src/` |
| RegC JDK intrinsic | `mj2x/mj2x-core/src/mj2x/backend/regc/` + `mj2x/mj2x-runtime/src/intrinsic/` + `mj2x/mj2x-crt/src/regc/` |
| 方法选择与 CLI native 编译 | `mj2x/mj2x-cli/` |
| 用户注解包 | `mj2x/mj2x-annotations/` |
| 配置系统（参数注册表） | `mj2x/mj2x-cli/src/param_registry.{hpp,cpp}` + `mj2x/doc/config_file.md` |
| protected samples | `mj2x/test_resource/protected_samples/` |

### 7.1 配置系统（参数注册表单源，主线十二）

全量参数单源注册表（`ParamDef` 表）驱动 CLI 解析 / 配置文件 / `--help` /
schema 导出四处，消灭 help 与 config 文档漂移：

- **注册表**：`mj2x-cli/src/param_registry.{hpp,cpp}`——45+ 参数（三后端 +
  混淆 + 反侦测 + native 打包 + per-client ISA + intrinsic + 日志）；
  规范 ID `域.键` ↔ snake_case JSON 键 ↔ CLI flag 同源；枚举/范围/默认值/
  描述集中声明。
- **配置**：`--config <file>` 全量键 JSON（旧 15 键向后兼容）；未知键/类型/
  枚举/范围错误定位键名；合并语义 默认值 < 配置 < CLI（CLI 显式覆盖）。
- **Schema**：`--dump-config-schema` 输出 JSON Schema draft-07
  （`additionalProperties:false` + `description/default/enum/minimum/maximum/
  x-requires`）——GUI 配置工具 / VS Code `json.schemas` / ajv 直接消费。
- **模板**：`--dump-config-template` 输出全量键 + 默认值 + 注释的 JSONC 起点。
- 测试：`config_registry_test.cpp` 10 项（schema 键全覆盖、description 同源计数、
  模板全键、未知键/类型/枚举错误定位、config backend 驱动交叉校验、CLI 覆盖
  config、多族参数一次配置）。
- 文档：`doc/config_file.md`（键表）+ `doc_ai/config_system_design.md`（设计）。

## 8. mtest_set 集成（跨产品共享样本）

mj2x 消费 `mtest_set`（全仓库中性 Java 样本库）的 baseline、Wave 1
（G3 类型转换/G6 方法调用/G7 字段）和 Wave 2（G9 对象与类型/G10 异常与 monitor/
G11 反射）生成样本做 native 转换 E2E，建立双后端的 opcode/语义覆盖对齐。

- 中性事实源：`mtest_set/src/manifest/semantic_fixture_manifest.hpp`
- 消费：`mj2x/unittest/src/integration/mtest_set_native_matrix_test.cpp`
- 独立目标：`mj2x_integration_test`，不与 emitter/IR unit 混跑
- 当前矩阵：18 条 fixture x StackC/RegC = 36 条真实 E2E；每条均走
  `mj2x-cli --compile`、native library、`java -Xverify:all` 和 stdout 精确比较
- 跨项目样本访问：`test_helper` 的 `getProjectResource` / `getProjectResourcePath` 系列（按项目名定位）
- JDK/MSVC 探测：统一走 `libs/libjdk_finder`（与 `mj2x-cli --compile` 共用，不再各自硬编码路径）
- 覆盖映射：`../../doc_ai/jbc_sample_coverage.md`（global 文档）

与 `stackc_backend_e2e_test.cpp`（当前源码实际 30 个手写 inline Java case）互补：后者覆盖
monitor/exception/invokedynamic 等更广形状；共享矩阵逐步接管可双后端比较的 case。
Wave 1 首次运行发现并修复包内单 class 的 StackC cloader sidecar 错写到 package 子目录，
现在统一输出到 classpath 根 `mj2x/cloader/Loader.class`。Wave 2 首次运行发现并修复
RegC exception handler local 未通过异常边传递的问题，`g10.monitor` 的异常释放路径现已
完成 native/JVM 行为验证。Wave 3（#820）新增 G12 StringOps / G13 BitOps /
G14 DeepRecursion / G15 CollectionOps（吸收 bludnov/ttvar 语义覆盖面）。

### 8.1 用户自定义 intrinsic（issue #818）

三后端统一支持把 Java 静态调用点降级为用户 native symbol 直调：

- 配置：`--intrinsic-config <json>`（`intrinsics` 数组 + `sources` 数组）+ `--intrinsic-src <file>`（可重复）
- ABI：`passContext:true` 时第一参数 `JNIEnv* env`（三后端一致）
- RegC：`RegCIntrinsicRegistry` 用户条目，`UserIntrinsic` emitKind 恒保留 pending
  异常检查与 local-ref 帧；生成 C 头部发射 `extern <ret> <symbol>(...)` 声明
- StackC：`Impl::userIntrinsics`，`emitInvoke` 静态调用点匹配后从栈槽弹参直调；
  `mayThrow:true` 保留调用后 ExceptionCheck
- VMC4：`VmMethodRef.userSymbol` 参与去重，helper 解包 `regs[argIdx[]]` 直调
- sources 并入：`--compile` 直接模式追加进编译命令；`--generate-xmake` 复制到
  工程 `src/mj2x-crt/user_intrinsics/`（模板 glob 自动编译）
- 测试：`user_intrinsic_test`（发射形态 9 例）+ `user_intrinsic_e2e_test`
  （三后端执行矩阵，`my_m` 接管 `F.m` 验证 2003）

---

## 9. 验证入口

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x-crt
xmake build mj2x_unittest
xmake build mj2x_integration_test
xmake build mj2x_runtime_test
xmake run mj2x_crt_test
xmake run mj2x_unittest
xmake run mj2x_integration_test
xmake run mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

Linux CI 安装 OpenJDK 17，并构建、执行 `mtest_set_unittest`、`mj2x_unittest`
和 `mj2x_integration_test`；`mj2x-cli` 作为同一构建批次的依赖目标。

能力到测试的追溯见本文档各能力表的"验证"列与 `test_strategy.md`。

## 10. Validation maturity

- 首切片：`script/validate_release.ps1 -Layer mj2x -Mode release` 覆盖 build、unit、CLI smoke，并产出 `build/artifacts/windows/x64/<mode>/mj2x/mj2x-windows-x64-<mode>.zip` 与 `SHA256SUMS`。
- Windows 原生工具链实测：MSVC、Clang/Clang++、Zig、xmake 和 CMake 的 class/JAR、file/pipe 与 C++17 编译由 `CliTest.*` 覆盖（见 test_strategy §1.1）；Linux/macOS/GCC 的覆盖由对应平台 CI 提供。
- 尚未产品级：真实 protected sample corpus、跨 MSVC/GCC/Clang native compile gate、mjdbg observation report 仍需作为 release gate 固化。

# RegC 代码生成（RegIR → C）

> 设计文档：RegC 后端把寄存器 IR 翻译成 C 的**生成策略**与**设计原理**。
> 代码是实现，本文讲"为什么这么生成、各模块怎么处理"。
>
> 分工（每件事只有一处权威）：
> - **本文**：生成的整体设计——一个方法变成什么样的 C、转换边界（哪些不转）、各机制（引用管理、异常分发、缓存、绑定）的策略选择与原理。
> - `codegen_style.md`：生成的**命名**与**注释**规范、源码拼接工具（CSourceStream 三层模型）。
> - `regc_intrinsic.md`：JDK 方法调用级 **intrinsic 识别**与 shadow state 策略。
> - `ir_design.md` §13：L3 Register IR 的 lowering（SSA block arg → 边拷贝）。
>
> 入口：RegC 后端在 `backend/regc/`（emitter 主体、backend facade、runtime 模板、intrinsic 注册表）。

## 1. 设计哲学

RegC 的目标是把 Java 方法体翻译成**寄存器式 C**，同时消除 c2j 能识别的栈机锚点（`cstack[]`/`sp`/`union jvalue` 等）。三条贯穿全文的原则：

1. **无栈数组**：IR 操作数是"值"（虚拟寄存器），不是带下标的存储槽。生成 C 直接把每个寄存器映射成 C 局部变量，基本块映射成 `goto` 串联的标签块。这是一切设计的出发点——见 `ir_design.md` §13 的"无 JVM 栈槽"不变量。
2. **语义保真优先于性能**：每条 JVM 语义规则（整数溢出环绕、除零抛异常、无符号右移、引用同一性比较、NaN 比较）都在生成 C 里有对应处理，宁可多调一个 helper 也不静默改变语义。性能优化（intrinsic、cache CSE）只在**不破坏语义**的前提下做。
3. **fail-closed**：任何无法保证语义正确性的情况，保留字节码、不转 native，并留下可观测日志。绝不生成"看起来能跑但语义错了"的 C。

## 转换边界（哪些不转 native）

大部分 JVM 指令和普通方法调用都能转 native。下面是**有意保留字节码**的边界——这些是当前已知会破坏语义或独立根因未解的情况。每个 fail-closed 点都留可观测日志（`[RegCBackend] Skipping <method>: <stage> failed: <reason>`），不静默。

**方法级保留**（命中即整个方法不转）：

| 情况 | 原因 |
|---|---|
| 有逻辑构造器（局部变量表非空） | 搬运注入的 `__regc_init_*` 会被 `Class.getDeclaredMethods()` 反射计数（synthetic 亦可见），破坏精确方法数断言（如 JavaObfuscatorTest Test 2.1 Counter）；业界（JNIC/j2cc/native-obfuscator）均不转构造器逻辑。空构造器仍骨架化 |
| 构造器含 try-catch / 局部变量注解 / 骨架段含 FRAME/跳转 / 跨段 label 引用 | 搬运法保守边界（§9） |
| `abstract` / 已有 `native`（无字节码） | JVM 限制，不进入后端 |

> 历史 fail-closed 分组（`Thread`/`java.util.concurrent/*`、`System.exit`/
> `setSecurityManager`、反射组、IO 组、类加载组、自定义 bootstrap invokedynamic）
> 已全部放开：并发根因是旧全局 JNIEnv 在多线程 native 并发下互相覆盖，thread-local
> env/ctx 修复（2026-08-09）后消除；invokedynamic 自定义 bootstrap 由阶段3
> 运行时 call-site helper 支持（`regc_invokedynamic_strategy.md`）。

**`<init>`/`<clinit>` 不直接标 ACC_NATIVE**（JVM 部分实现拒收 native 构造器），但用搬运法把方法体搬进生成的静态方法、原骨架保留 super 调用，构造器/类初始化逻辑仍转 native（见 §9）。

**invokedynamic 展开形态**（在 SSA 前由 `InvokeDynamicPreprocessor` 把可静态解析的站点改写为普通字节码）：

| 形态 | 状态 |
|---|---|
| StringConcatFactory makeConcat[WithConstants] | ✅ 展开为 StringBuilder 链 |
| 无状态/捕获 lambda（合成类） | ✅ |
| 方法引用：实例/构造器/接口/super | ✅ |
| 擦除泛型（CHECKCAST/拆箱/装箱/POP） | ✅ |
| altMetafactory（serializable/marker/bridge） | ✅ |
| 基本类型加宽桥接（i2l 等） | ✅ |
| 装箱+加宽复合 | ✖ fail-closed |
| 基本类型收窄 | ✖ fail-closed |
| 自定义 bootstrap | ✅ 阶段3 运行时 call-site helper（2026-08-09，见 `regc_invokedynamic_strategy.md`） |

展开策略：可静态解析的站点（LMF/concat）在 SSA 前由 `InvokeDynamicPreprocessor` 改写为普通字节码，改写后走已有的 new/invoke/StringBuilder-intrinsic 路径。自定义 bootstrap（阶段3）无法静态展开，由 SSA 登记站点元数据 + finalize 生成文件级 bootstrap 函数（MethodHandles.lookup/privateLookupIn、MethodType、bsmArgs 重建、CallSite.getTarget 全局缓存），方法体经 invokeWithArguments 调用。

**已放开的边界**（曾经保留、现已转 native，仅留痕迹）：反射组（Class/reflect\*/getClass）、IO 组（InputStream/ByteArrayInputStream）、类加载（ClassLoader.loadClass 等）——引用管理（PushLocalFrame/PopLocalFrame）已吸收这些路径产生的 local ref。内部类 `$` 随 RegisterNatives 撤销 skip。大异常表（多 catch）lowering 三层无上限。

## 2. 一个方法的生成产物

每个成功 native 化的方法生成两层 C：

- **helper 函数** `regc_<owner>_<name>_<desc>`：`static`，接收虚拟寄存器作参数，函数体是 `goto` 串联的块，所有 JVM 操作降低为对 `_mj2x_regc_*` helper 或 intrinsic 的调用。它不是 JNI 导出，纯粹是寄存器式 C。
- **JNI wrapper** `__mj2x_c_<hash>`：`static ... JNICALL`，签名匹配 Java 描述符的 JNI 调用约定，负责设置 JNI 环境/上下文、做 jni↔regc 类型转换、转调 helper。它通过 RegisterNatives 动态绑定，不走 `Java_*` 符号导出（见 §7）。

helper 内部结构：参数与寄存器声明区 →（可选）引用帧入栈 → `goto` 入口块 → 各基本块（块标签 + 指令生成的 C + 终结器生成的 C）→ 单一 cleanup 出口（销毁 native 对象、引用帧出栈、返回）。

## 3. 指令生成策略

emitter 逐条消费 RegIR 指令。每类指令的核心问题是"如何在不引入栈机锚点的前提下保真表达 JVM 语义"。

### 3.1 纯计算（Copy / Unary / Binary / Compare / Convert）

这是最直接的部分——寄存器间的运算直接映射成 C 表达式。但有几个语义陷阱必须专门处理：

- **整数运算的溢出环绕**：Java 的有符号整数溢出是 well-defined 的环绕。生成 C 用**无符号算术**做加减乘（先 cast 到 `uint32_t`/`uint64_t`，运算后 cast 回），让 C 的有符号溢出 UB 变成与 Java 一致的环绕。
- **除法/取余**：Java 除零抛 `ArithmeticException`，`MIN/-1` 有特殊溢出处理。这些不能裸用 C 的 `/`/`%`（UB + 无异常），必须走 CRT 的 `mj2x_regc_div/rem_*` helper，由 CRT 负责抛异常和溢出处理。
- **无符号右移（>>>）**：先 cast 到无符号类型再 `>>`，再 cast 回。
- **引用比较（acmp）**：两个引用相等判定**必须用 `IsSameObject`**，不能用裸 `void*` 比较——不同的 local ref 句柄可能指向同一对象，裸比较会误判。
- **checkcast**：走带 class 缓存的 `_mj2x_regc_checkcast_cached`，失败由 helper 抛 `ClassCastException`。

### 3.2 字段 / 数组访问

字段和数组读写降低为 `_mj2x_regc_load/store_*` helper 调用。策略要点：

- **窄类型走窄 JNI accessor**：boolean/byte/char/short 字段用对应的 `GetBooleanField`/`GetByteField` 等，而非按 SSAType 宽化。这保证 `byte` 字段读出来是符号扩展正确的值。
- **byte[] 与 boolean[] 共用 accessor**：JVM 层 `baload`/`bastore` 同时覆盖两者，运行时 helper 按数组实际类分流。
- **缓存**：编译期已知的 owner/name/desc 走 TU 级缓存（见 §6），避免每次 `GetFieldID`；动态描述符走裸 helper。

### 3.3 方法调用

方法调用是最复杂的指令，因为它要同时处理：intrinsic 识别、参数物化、self-call 优化、按 receiver 类解析。

**调用生成的决策顺序**：
1. **先试 intrinsic**：`RegCIntrinsicRegistry` 按 owner/name/desc 识别 JDK 常用方法（Math、Integer/Long 位操作、parseInt、wrapper、Random、String/StringBuilder）。命中则生成 runtime 直调，绕过 JNI 派发（策略与 shadow state 见 `regc_intrinsic.md`）。
2. **self static call → 直接 C 调用**：同类同签名的静态调用，绕过 JNI helper 直接调 `regc_*`（C 函数递归）。这是免费的性能优化，且语义无损。
3. **否则 → `_mj2x_regc_call_*` 系列**：走 JNI `Call*MethodA`，参数由描述符驱动的 `va_list` 打包成 `jvalue[]`。

**实例方法解析的关键正确性问题**：jmethodID **必须按 receiver 的实际运行时类解析**，不能按声明 owner 解析。当 owner 是抽象类/接口时（如 `InputStream.read`），按 owner 解析拿到的是抽象条目，调用会触发 `AbstractMethodError` 甚至 JIT 内联路径崩溃。helper 对此的处理：仅当 receiver 类 == 声明 owner 时复用缓存的 id 并写回，子类 receiver 每次重新解析。

**resolved native dispatch（双槽缓存，issue #780）**：对已转换实现类（或 owner 未覆盖方法）的动态实例调用走 resolved dispatch——站点级缓存 `mj2x_regc_resolved_call_site_t` 持有 `{method, fn, implClass, receiverClass}` 完整组（receiverClass 为 global ref 钉住），快路径单 JNI 直调实现类函数。`_mj2x_regc_resolved_call_site_refresh` 只写槽 1，槽 0 保留上次命中：A/B 交替 receiver 时两槽各自命中，避免每轮 refresh（每轮 5-6 个 JNI）；命中槽 1 时两槽交换，让热 receiver 留在槽 0。method/fn/implClass/receiverClass 必须来自同一次解析，杜绝 methodID 与 receiver 错配。

**mutable string 参数物化**：当调用参数是 StringBuilder/StringBuffer 的 shadow state 时，先物化成 Java 句柄再传（见 §5），且整个调用被 `if (!_mj2x_regc_exception_check())` 守护，避免物化抛异常后继续执行调用。

### 3.4 指令级 intrinsic

RegIR 有独立的 `Intrinsic` 指令（区别于上面 Call 分支的方法调用级 intrinsic），覆盖几类无法表达为"方法调用"的 JVM 操作：

- `array.length` → `_mj2x_regc_array_length`
- `instanceof:<T>` → `_mj2x_regc_instanceof_cached`（带 class 缓存）
- `monitor.enter/exit` → `_mj2x_regc_monitor_*`
- `cmp.nan_greater`/`cmp.nan_less` → 三值比较（LCMP/FCMP/DCMP 系列），用 `((lhs>rhs)-(lhs<rhs))` 产生 -1/0/1，浮点额外加 NaN 分支（`nan_greater` 即 dcmpg 语义 NaN→1，`nan_less` 即 dcmpl 语义 NaN→-1）

## 4. 异常分发

JVM 的 try/catch 在 RegIR 里表示为 `RegExceptionRegion`（保护块集合 + handler 块 + 捕获类型 + 异常寄存器）。生成策略：

- **每条可能抛异常的指令后插入检查**：`if (_mj2x_regc_exception_check()) { ... }`。这是 fail-safe 的——宁可多检查（C 分支开销极小）也不漏掉异常传播。
- **异常检查稀疏化（issue #778 P2）**：SSA 阶段无法区分 intrinsic 与普通调用，统一标 mayThrow，导致热循环每轮都做 ExceptionCheck；emitter 在发射异常分发前用 `isPureRuntimeIntrinsic` 复判——`RuntimeFunction` emitKind 的纯计算 intrinsic（位操作/Math 等）发射为纯 C 表达式调用，不经过 JNI、不可能抛异常，省略该检查（与 `emitIntrinsicCall` 的发射行为一致，该分支无 native-state 回退）。Arithmetic 热循环探针 66x→2x。
- **按异常表顺序匹配**：取到 pending exception 后，按 region 顺序逐个 `_mj2x_regc_exception_matches`，命中则把异常装入 handler 的合成寄存器、复制 CFG 边、`goto` handler 块。空捕获类型视为 catch-all（finally）。
- **未命中则重抛**：没有任何 handler 接住时，`_mj2x_regc_throw` 重新抛出，保持 Java 的向上传播语义，然后走默认返回 + cleanup。
- **多 throw 站点**：用单调递增的 dispatch index 给每个异常临时量编号（`ex_<n>`），避免命名冲突。

## 5. 引用管理与 native 对象状态

### 5.1 JNI local ref 帧

native 方法在单一 JNI 帧内运行，所有产生 local ref 的操作（方法调用、对象/数组分配、class/string 字面量、ref 字段/数组读）返回的 local ref 会累积。反射密集方法会撑爆 JNI 默认 16 槽 local ref 表。

策略：对**含产 ref 操作的方法**，入口 `PushLocalFrame(16)`、单一 cleanup 出口 `PopLocalFrame`，批量释放本帧全部 local ref。REF 返回值通过 `PopLocalFrame` 的 result 参数 promote 到父帧。**纯标量方法（无产 ref 操作）不发射帧管理**，保留"零 JNI 开销"形态。缓存用的 class/method 全局引用（`NewGlobalRef`）不受帧影响。

### 5.2 native 对象 shadow state

某些 Java 对象在生成代码里以**原生 C 结构体**持有，绕过 JNI：StringBuilder/StringBuffer、包装类（Integer/Long/...）、Random。策略与逃逸规则见 `regc_intrinsic.md` §3——核心是：这些 shadow state 只在**不逃逸**（不出现在 return/调用参数/字段写/数组写/CFG 边拷贝）时保持 native 形态；逃逸时 mutable string 经 `materialize` 物化成 Java 句柄（结果缓存复用），其它 native 对象目前 fail-closed。emitter 在每个逃逸边界用 `rejectNativeObjectEscape` 守卫。

## 6. JNI 缓存（cache CSE）

编译期已知的 class descriptor、方法 owner/name/desc、字段 owner/name/desc 在整个 TU 内共享一份缓存（`RegCCachePool` 按 key 去重）。首次访问时 `FindClass`→`NewGlobalRef` 升级为全局引用，再 `GetMethodID`/`GetFieldID` 解析并写回缓存槽。全局引用生命周期 = 进程，不释放。

**动态描述符不缓存**。`NEW`/引用数组/多维数组/`CHECKCAST`/`INSTANCEOF`/字段/方法调用都在编译期拥有 descriptor，已由 pool 生成稳定 slot；走裸 `_mj2x_regc_find_class` 的只剩 standalone emitter、异常路径和未知运行时输入的 fail-safe fallback，不是热路径。给动态 descriptor 加全局缓存在正确性上站不住：以 descriptor 字符串为 key 会遗漏 **class-loader identity**——JNI `FindClass` 的结果取决于当前调用的关联加载器，全局强引用可能把另一个 loader 的同名类复用进来，还会阻止类及其 loader 卸载；改 weak global cache 又需要 loader identity、并发初始化、失效重试、pending exception 处理，不是不改变语义的微优化。未来若重新评估，须同时具备 owner loader identity、`jweak` 生命周期、线程安全初始化、pending exception 语义测试，以及命中率和 native E2E 的正收益证据。

## 7. RegisterNatives 动态绑定

RegC **放弃 `Java_<mangled>` 符号导出**。原因：JNI 符号查找链路脆弱，尤其内部类名里的 `$` 即便按规范转义也难可靠工作。替代策略：

- 每个 native 方法的 C 入口名是不透明哈希 `__mj2x_c_<fnv1a64>`（`static`，非 `JNIEXPORT`），与 JNI 符号查找完全脱钩。
- 每类一个 register 函数，构造 `JNINativeMethod[]` 后 `RegisterNatives` 绑定，`MonitorEnter/Exit` 守护。
- `JNI_OnLoad` 把一个桥接函数绑到 `mj2x/cloader/Loader.registerNativesForClass`；每个受保护类的 `<clinit>` 调用它触发本类惰性注册。
- clinit 注入顺序保证：`[NativeLoader.load, registerNativesForClass, ...用户 clinit...]`——先加载库、再绑定、绑定先于用户静态初始化（用户 clinit 可能调 native 方法）。

这彻底消除了符号名/导出的脆弱性，内部类的 `$` 不再需要任何特殊处理。

## 8. 生成 TU 与外部 runtime/crt 的边界

生成的 C `#include` 权威 ABI 头 `regc/mj2x_regc_crt.h`，与外部 runtime/crt 的分工：

- **TU 内联**（`_mj2x_regc_*`，每个生成 C 文件一份 `static` 定义）：字段/数组/分配/方法调用/异常分发 helper、parse/string 比较 helper、全局上下文（`_mj2x_regc_env`/`_mj2x_regc_ctx`）。这些是需要直接访问 JNI 的部分。
- **外部链接**（`mj2x_regc_*`/`mj2x_math_*` 等，由 mj2x-rt 源包提供）：context 管理、整数除余（含异常路由）、barrier、String/StringBuilder/StringBuffer ABI、标量 intrinsic（Math/Random/包装类）。这些是纯计算或可独立测试的部分。

边界演进方向：当前 TU 内联的部分（字段/数组/调用/异常）后期会迁入可分发的 CRT，让生成 C 更小、ABI 更稳定。`mj2x_regc_crt.h` 的注释标记了这一演进意图。

## 9. `<init>`/`<clinit>` 的搬运法

构造器和类初始化器**不能直接标 ACC_NATIVE**（JVM 部分实现拒收 native `<init>`），但它们的逻辑仍然要转 native。策略是**搬运法**：

- 把原方法体指令搬进一个新生成的同签名静态方法（`__regc_init_<hash>`/`__regc_clinit_<hash>`），这个生成方法正常走 native 编译。
- 原 `<init>`/`<clinit>` 骨架只保留"super 调用 + 对生成方法的调用"（`<clinit>` 还要保证注册时机在调用生成方法之前）。

所以构造器/类初始化的逻辑**确实转 native 了**，只是通过搬运而非直接标记。

**保守边界**（命中则不搬，保留字节码）：方法体含 try-catch / 局部变量表 / 局部变量注解；`<init>` 骨架段（切点 `INVOKESPECIAL <init>` 之前）含 FRAME 或跳转/switch；骨架段引用了克隆体段的 label（行号锚点除外）；或克隆体的跳转/switch 引用了骨架段 label。javac 常态（骨架段只有 LABEL/LINE 行号锚点）放行。局部变量表本身只是调试信息，但搬运会注入新方法导致反射方法数变化，故有逻辑构造器（LVT 非空）一律保留字节码——这是与反射计数兼容的取舍，见 `regc_native_coverage_analysis.md`。生成方法标记 `ACC_SYNTHETIC`（反射仍可见，语义正确标记）。

## 10. 相关文档

| 文档 | 覆盖 |
|---|---|
| 本文 | 生成策略、转换边界、各模块处理原理、引用/异常/缓存/绑定设计 |
| `codegen_style.md` | 命名（标识符表）+ 注释（分级溯源）+ 源码拼接（CSourceStream） |
| `regc_intrinsic.md` | JDK intrinsic 识别、shadow state 与逃逸策略 |
| `ir_design.md` §13 | L3 Register IR（SSA→RegIR lowering） |

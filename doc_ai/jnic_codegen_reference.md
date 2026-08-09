# JNIC 代码生成细节参考（3.5.1 抓源码实证）

> 本文档记录对 JNIC 3.5.1 生成的 C 源码的逐模式分析，作为 JVM→C 直译技术的**完整代码
> 生成细节参考**。所有结论均来自对真实产物 `jnic.c`（224KB / 5950 行，输入 TEST.jar 37 类）
> 的逐行取证，不是猜测。
>
> 素材：`jnic.c` 已归档于
> `C:\Users\Canrad\AppData\Local\Temp\opencode\jnic_capture\jnic13438505281557630243_20260809_101838_542\jnic13438505281557630243\jnic.c`
>
> JNIC 3.7.0 的 DLL 逆向对比见 `jnic370_reverse_notes.md`（性能更新点汇总）。

## 1. 文件总览

```
Auto-generated JNIC implementation file
#include <jni.h>
#include <stdatomic.h>   // 原子缓存
#include <string.h>
#include <stdarg.h>
#include <math.h>
```

内容顺序（固定）：
1. 全部 `cached_c_N` 缓存结构 + 初始化函数（0..70，按首次引用顺序编号）
2. 全部方法体 `jnic_0..jnic_49`（按注册表顺序编号）
3. 每类一个 `Java_...__00024jnicLoader` 注册函数，跟在对应方法体后面

## 2. 类缓存（cached_c_N）

### 结构体

```c
struct cached_c_36 {
    jclass _Atomic clazz;    // 原子全局引用（cache clazz 非空 = 已初始化）
    jfieldID id_0;           // 字段 ID，编号=该类上字段引用顺序
    jmethodID id_1;          // 方法 ID，编号=该类上方法引用顺序
    jobject lookup;          // 混淆所需 MethodHandles.Lookup（仅部分类有）
};
```

- `id_N` 编号按**该类被引用的顺序**分配，与字段/方法种类无关（`id_0` 可能是 static field，
  `id_1` 可能是 instance method，混合编号）
- `jobject lookup` 字段存在与否取决于该类是否参与 invokedynamic（需要 Lookup 对象）

### 初始化函数（线程安全单例）

```c
static struct cached_c_36* c_36_(JNIEnv *env) {
    static struct cached_c_36 cache;
    static atomic_flag lock;
    if (atomic_load_explicit(&cache.clazz, memory_order_acquire)) return &cache;
    jclass clazz = (*env)->FindClass(env, "java/lang/Integer");
    while (atomic_flag_test_and_set(&lock)) {}          // 自旋锁
    if (!cache.clazz) {                                  // 双检锁
        cache.id_0 = (*env)->GetStaticFieldID(env, clazz, "TYPE", "Ljava/lang/Class;");
        cache.id_1 = (*env)->GetStaticMethodID(env, clazz, "valueOf", "(I)Ljava/lang/Integer;");
        clazz = (*env)->NewGlobalRef(env, clazz);        // 升级全局引用
        atomic_store_explicit(&cache.clazz, clazz, memory_order_release);
    }
    atomic_flag_clear(&lock);
    return &cache;
}
```

要点：
- `FindClass` 在锁外执行（可能抛异常，调用者负责检查）
- `id_*` 在锁内填充，但**用 `cache.clazz` 非空作为"整个 cache 就绪"的发布标志**
- ID 缓存不单独做原子，靠 clazz 的 release/acquire 传递可见性
- 只缓存 jclass/jmethodID/jfieldID，**不缓存字符串**（字符串每次 NewString）

### 特殊缓存

- 类缓存 ID 编号是全局的（c_0_..c_70_），与类顺序无关，按**首次出现**编号
- `c_1_` 是固定的（`MethodHandles.lookup()` 的 owner，被所有 loader 复用）

## 3. 方法体（jnic_N）

### 签名与返回类型映射

| Java 返回 | C 签名 | 例 |
|---|---|---|
| void | `void JNICALL jnic_N(JNIEnv *env, jobject thisObj[, args])` | `jnic_1` |
| int | `jint JNICALL jnic_N(JNIEnv *env, jobject thisObj, jint arg0, ...)` | `jnic_0` |
| long | `jlong JNICALL ...` | — |
| float/double | `jfloat/jdouble JNICALL ...` | — |
| boolean | `jboolean JNICALL ...` | — |
| 引用 | `jobject JNICALL ...` | `jnic_37` |

- **static 方法也带 `jobject thisObj`**（统一签名，static 时忽略）
- 参数从 `arg0` 编号，与局部槽 1..N 对应
- 全部 `JNICALL` 调用约定

### 局部变量区（固定声明块）

```c
jvalue local0; memset(&local0, 0, sizeof(jvalue));   // 局部槽 0（this 或 arg0）
jvalue local1; memset(&local1, 0, sizeof(jvalue));
...
jvalue stack0; memset(&stack0, 0, sizeof(jvalue));   // 操作数栈槽
jvalue stack1; memset(&stack1, 0, sizeof(jvalue));
...
jvalue temp0; memset(&temp0, 0, sizeof(jvalue));     // 临时槽（固定 4 个）
jvalue temp1; memset(&temp1, 0, sizeof(jvalue));
jvalue temp2; memset(&temp2, 0, sizeof(jvalue));
jvalue temp3; memset(&temp3, 0, sizeof(jvalue));
```

- `local_N`：Java 局部变量槽，数量 = max_locals（含 this/args）
- `stack_N`：操作数栈，数量 = max_stack
- `temp0..temp3`：**固定 4 个临时槽**（无论方法多大都只有 4 个），用于跨指令暂存
- `jvalue` 是 union，`.i/.j/.f/.d/.l/.s/.b/.c/.z` 按类型取字段

### 参数装载（方法序言）

```c
local0.l = thisObj;    // 槽 0 固定 thisObj
local1.i = arg0;       // 槽 1 = arg0
local2.i = arg1;       // 槽 2 = arg1
```

- 槽类型按 descriptor 决定 `.l/.i/.j/.f/.d` 等
- 引用类型参数直接 `.l`（JNI 引用就是指针）

## 4. 指令翻译对照表（逐指令）

### 4.1 常量

| 指令 | 翻译 |
|---|---|
| ICONST_M1/0..5 | `stackN.i = -1;` / `stackN.i = 0;` ... |
| LCONST_0/1 | `stackN.j = 0LL;`（无后缀时 `stackN.j = 0;`） |
| FCONST_0..2 | `stackN.f = 0.0f;` / `1.1f` 等 |
| DCONST_0/1 | `stackN.d = 0.0;` / `1.0;` |
| BIPUSH/SIPUSH | `stackN.i = 100;` / `stackN.i = 1024;` |
| LDC int | `stackN.i = 42;` |
| LDC float/double | `stackN.f = 1.1f;` `stackN.d = 1.0E-18;`（科学计数法直出） |
| LDC long | `stackN.j = 50LL;`（>32 位加 LL 后缀） |
| LDC String | `stackN.l = (*env)->NewString(env, (unsigned short[]) {70, 65, 73, 76}, 4);` |
| LDC Class | `stackN.l = c_N_(env)->clazz;`（走缓存） |
| ACONST_NULL | `stackN.l = NULL;` |

### 4.2 局部变量

| 指令 | 翻译 |
|---|---|
| XLOAD | `stackN.i = localM.i;`（类型对应 `.i/.j/.f/.d/.l`） |
| XSTORE | `localN.i = stackM.i;` |
| IINC | `localN.i += 1;` / `localN.i -= 1;` |

### 4.3 栈操作

| 指令 | 翻译 |
|---|---|
| DUP | `tempN.l = stackM.l; stackM.l = tempN.l; stackN.l = tempN.l;` |
| SWAP | 同 DUP 的 temp 中介模式 |

DUP 的完整模式（对象）：入栈 `stack0.l = local0.l` 后：
```c
temp0.l = stack0.l;
stack0.l = temp0.l;
stack1.l = temp0.l;
```
即：保存原值到 temp，再复制到下一槽。

### 4.4 算术（整数）

| 指令 | 翻译 |
|---|---|
| IADD | `stack0.i = (unsigned int) stack0.i + stack1.i;` |
| IMUL | `stack0.i = stack0.i * stack1.i;` |
| ISUB | `stack0.i = stack0.i - stack1.i;` |
| 其他 | 直接 C 运算符，int 一律 `(unsigned int)` 包裹（无符号环绕语义） |

- **iadd/长整型环绕语义**：int 运算统一 `(unsigned int)` 前缀保证 JVM 无符号环绕；
  long 运算无前缀（C 的 unsigned long long 不隐式，但 JVM long 也是补码环绕，
  实际用 `jlong` 直接算）
- **比较指令**：`if (stack0.i != 0) goto L0;` 直接用 C 关系运算符 + goto

### 4.5 浮点比较（最精细的模式）

| 指令 | 翻译 |
|---|---|
| DCMPL | `stack0.i = stack0.d < stack1.d ? -1 : stack0.d > stack1.d ? 1 : stack0.d == stack1.d ? 0 : 1;` |
| DCMPG | 同上但 NaN 分支返回 1 |
| FCMPL | `stack0.i = stack0.f < stack1.f ? -1 : stack0.f > stack1.f ? 1 : stack0.f == stack1.f ? 0 : -1;` |
| FCMPG | NaN 分支返回 1 |

注意：
- NaN 处理：`dcmpg` NaN→1，`dcmpl` NaN→-1
- 比较结果**直接三元表达式**，不是调用
- 浮点转换：`(jfloat) stack0.d`（d2f）、`(jdouble) stack0.i`（i2d）直接 C 强转

### 4.6 字段访问

| 指令 | 翻译 |
|---|---|
| GETFIELD | `stackN.i = (*env)->GetIntField(env, stackM.l, c_K_(env)->id_N);` |
| GETSTATIC | `stackN.i = (*env)->GetStaticIntField(env, c_K_(env)->clazz, c_K_(env)->id_N);` |
| PUTFIELD | `(*env)->SetIntField(env, stackM.l, c_K_(env)->id_N, stackN.i);` |
| PUTSTATIC | `(*env)->SetStaticIntField(env, c_K_(env)->clazz, c_K_(env)->id_N, stackN.i);` |

类型映射：`Get<Type>Field` / `Set<Type>Field`（Int/Long/Double/Float/Boolean/Char/Short/
Byte/Object），引用走 Object。

### 4.7 方法调用

| 指令 | 翻译 |
|---|---|
| INVOKEVIRTUAL | `stack0.i = (*env)->CallIntMethod(env, stack0.l, c_K_(env)->id_N, args...);` |
| INVOKESTATIC | `(*env)->CallStaticVoidMethod(env, c_K_(env)->clazz, c_K_(env)->id_N, args...);` |
| INVOKESPECIAL | `(*env)->CallNonvirtualVoidMethod(env, stackN.l, c_K_(env)->clazz, c_K_(env)->id_N, args...);` |
| INVOKEINTERFACE | `(*env)->CallVoidMethod(env, stackN.l, c_K_(env)->id_N, args...);`（同 virtual） |

- 返回类型决定 `Call<Type>Method`（Void/Int/Long/Double/Float/Boolean/Object/...）
- `INVOKESPECIAL` 用 `CallNonvirtual` + **显式 clazz**（防重写）
- 参数直接按调用栈顺序排列传参

### 4.8 对象创建

```c
// NEW + DUP + INVOKESPECIAL <init>
stack0.l = (*env)->AllocObject(env, c_60_(env)->clazz);
if ((*env)->ExceptionCheck(env)) { return; }
temp0.l = stack0.l;
stack0.l = temp0.l;
stack1.l = temp0.l;
    (*env)->CallNonvirtualVoidMethod(env, stack1.l, c_60_(env)->clazz, c_60_(env)->id_0);
```

- `NEW` → `AllocObject`（不调用构造器）
- `<init>` 调用用 `CallNonvirtualVoidMethod` + 显式 clazz

### 4.9 数组

| 指令 | 翻译 |
|---|---|
| NEWARRAY int[] | `stackN.l = (*env)->NewIntArray(env, stackM.i);`（类型对应 NewByte/Short/...） |
| ANEWARRAY | `stackN.l = (*env)->NewObjectArray(env, stackM.i, c_K_(env)->clazz, NULL);` |
| MULTIANEWARRAY | `(*env)->NewObjectArray(env, dim, c_K_(env)->clazz, NULL);` 逐维嵌套 |
| IALOAD | `stackN.i = (*env)->GetIntArrayRegion(...)` — 见下 |
| IASTORE | `(*env)->SetIntArrayRegion(...)` |
| AALOAD | `stackN.l = (*env)->GetObjectArrayElement(env, stackM.l, stackK.i);` |
| AASTORE | `(*env)->SetObjectArrayElement(env, stackM.l, stackK.i, stackN.l);` |
| ARRAYLENGTH | `stackN.i = (*env)->GetArrayLength(env, stackM.l);` |

**基本类型数组创建**（实证，两处）：`byte[]` 用法
```c
stack0.i = 1024;
stack0.l = (*env)->NewByteArray(env, stack0.i);   // new byte[1024]
if ((*env)->ExceptionCheck(env)) { return temp0.l; }
local2.l = stack0.l;
```
其余基本类型对应 `NewShortArray/NewIntArray/NewLongArray/NewFloatArray/NewDoubleArray/NewCharArray/NewBooleanArray`。

**基本类型数组读写**：TEST.jar 无实证（只有创建+整体传参），但 JNIC 模板
（Open-MyJ2c 同源 Snippets）使用 `GetXxxArrayRegion/SetXxxArrayRegion`。
引用数组读写（实证）：
```c
stack0.l = (*env)->GetObjectArrayElement(env, stackM.l, stackK.i);  // AALOAD
(*env)->SetObjectArrayElement(env, stackM.l, stackK.i, stackN.l);   // AASTORE
```

### 4.10 类型检查

| 指令 | 翻译 |
|---|---|
| INSTANCEOF | `if ((*env)->IsInstanceOf(env, stack0.l, c_K_(env)->clazz)) { ... }` |
| CHECKCAST | 无显式指令，隐式（后续 IsInstanceOf 检查兜底） |
| ATHROW | `if (stack0.l == NULL) (*env)->ThrowNew(env, c_K_(env)->clazz, ""); else (*env)->Throw(env, stack0.l); return;` |

### 4.11 控制流

```c
// if_icmpne L0 → 
if (stack0.i != stack1.i) goto L0;
// if_icmplt → if (stack0.i < stack1.i) goto L0;
// ifeq → if (stack0.i != 0) goto L0;  (注意：!= 0 即 0 时跳转)
// goto → if (1) goto L0;
// 无条件跳转统一 if (1) goto L0;
```

- **无条件 goto 输出为 `if (1) goto L0;`**（生成器不输出裸 goto）
- 标签输出 `L0:;`（带空语句，避免"label at end of compound statement"警告）
- if 条件与字节码语义对齐：`ifne` = 非零跳 → `if (stack0.i != 0) goto L0;`

### 4.12 switch

TEST.jar 无 tableswitch/lookupswitch 实例（全文件无 `switch(` 出现），但有对应模板
（LookupSwitchHandler.java / TableSwitchHandler.java 存在于 Open-MyJ2c 同源模板），
推断为 C `switch(stackN.i) { case ...: goto Lx; }` 形态。

### 4.13 字符串拼接（invokedynamic StringConcatFactory）

`"a" + b` 形态（cache c_70_ 是 StringBuilder）：
```c
// 使用 StringBuilder 缓存
stackN.l = (*env)->CallObjectMethod(env, stackM.l, c_70_(env)->id_1, arg);  // append
...
stackN.l = (*env)->CallObjectMethod(env, stackM.l, c_70_(env)->id_3);       // toString
```
缓存结构 `cached_c_70` 含：`<init>()V`、`append(String)`、`append(J)`、`toString()`。

### 4.14 invokedynamic（完整运行时桥接）

JNIC 3.5.1 对 invokedynamic 采用**运行期 CallSite 桥接**（非编译期展开）：

```c
{
    static jobject indy;
    if(indy == NULL) {
        jobject callSite = (*env)->CallStaticObjectMethod(env, c_12_(env)->clazz, c_12_(env)->id_0,
            c_8_(env)->lookup,                                  // MethodHandles.lookup()
            (*env)->NewString(env, (unsigned short[]) {114, 117, 110}, 3),  // 名字 "run"
            methodType,                                          // MethodType 构造
            ... implMethod 参数 ...);
        indy = (*env)->NewGlobalRef(env, callSite);
        (*env)->DeleteLocalRef(env, callSite);
    }
    temp0.l = (*env)->CallObjectMethod(env, indy, c_13_(env)->id_0);  // CallSite.getTarget()
    temp1.l = (*env)->NewObjectArray(env, 1, c_7_(env)->clazz, NULL); // Object[] args
    (*env)->SetObjectArrayElement(env, temp1.l, 0, stack1.l);
    stack1.l = (*env)->CallObjectMethod(env, temp0.l, c_14_(env)->id_0, temp1.l);  // invokeExact(Object[])
    stack1.l = stack1.l;
}
```

支撑缓存：
- `c_12_`：`LambdaMetafactory.metafactory(...)`
- `c_10_`：`MethodType.fromMethodDescriptorString(...)`
- `c_13_`：`CallSite.getTarget()`
- `c_14_`：`MethodHandle.invokeExact(...)`
- `c_11_`：`MethodHandles.Lookup.findStatic/findVirtual(...)`（`id_0`=findStatic、`id_1`=findVirtual，实证两处站点分别使用）

metafactory 实参顺序（实证，以 Runnable lambda 为例）：
```c
CallStaticObjectMethod(c_12_, c_12_->id_0,      // LambdaMetafactory.metafactory
    c_8_->lookup,                               // 1. MethodHandles.lookup()
    NewString({114,117,110},3),                 // 2. 接口方法名 "run"
    MethodType("(Lpack/tests/basics/runable/Exec;)Ljava/lang/Runnable;"),  // 3. 签名类型(带 this)
    MethodType("()V"),                          // 4. 实现方法类型
    CallObjectMethod(c_8_->lookup, c_11_->id_0, // 5. lookup.findStatic(implClass, "doAdd", MethodType("()V"))
        implClass, NewString("doAdd"), MethodType("()V")))
```
实例方法 lambda 的第 5 参改用 `c_11_->id_1`（findVirtual）。

要点：
- **每个 invokedynamic 站点一个静态 `indy` 单例**（方法内 static jobject），函数内跨调用持久
- 首次执行完整跑 metafactory，之后只 `getTarget()` + `invokeExact(Object[])`
- 参数装载：`NewObjectArray(1, Object.class)` + `SetObjectArrayElement`，**invokeExact 以 Object[] 形式接参**
- `stack1.l = stack1.l;` 自赋值收尾（生成器风格，无实际效果）
- 与编译期展开相比，运行期桥接有性能开销但语义完整

### 4.15 异常处理（完整实证）

**每个可能抛异常的调用点后**统一跟一个分发块。单 catch 类型：

```c
L0:;
stack0.l = (*env)->GetStaticObjectField(env, c_5_(env)->clazz, c_5_(env)->id_0);
if ((*env)->ExceptionCheck(env)) {
    stack0.l = (*env)->ExceptionOccurred(env);
    if ((*env)->IsInstanceOf(env, stack0.l, c_6_(env)->clazz)) {  // 匹配 catch 类型
        (*env)->ExceptionClear(env);
        if (1) goto L10;    // 跳 catch 块
    }
    return;                // 不匹配：异常原样保留，返回 JVM 向上抛
}
```

**多 catch 类型**：同一分发块内依次 IsInstanceOf，每个匹配对应不同 label（实证两个 catch：
`InterruptedException`→L3/L7、`Exception`→L10）：

```c
if ((*env)->ExceptionCheck(env)) {
    stack0.l = (*env)->ExceptionOccurred(env);
    if ((*env)->IsInstanceOf(env, stack0.l, c_16_(env)->clazz)) {  // catch 1
        (*env)->ExceptionClear(env);
        if (1) goto L3;
    }
    if ((*env)->IsInstanceOf(env, stack0.l, c_6_(env)->clazz)) {   // catch 2
        (*env)->ExceptionClear(env);
        if (1) goto L10;
    }
    return;
}
```

**catch 体结构**（实证，try-catch-finally 组合）：

```c
L6:;                     // try 正常路径结束
if (1) goto L8;          // 跳过第一个 catch 体
L7:;                     // catch(InterruptedException) 入口
local4.l = stack0.l;     // 异常对象存入局部槽 4
L8:;                     // 与 try 正常路径汇合（catch 体尾部共用）
...
L9:;                     // 汇合路径结束
if (1) goto L11;         // 跳过 finally
L10:;                    // finally 异常路径入口（各分发点 goto L10 的目标）
local4.l = stack0.l;     // 保存 pending 异常
...                      // finally 体（对未决异常路径执行）
L11:;                    // finally 之后统一汇合
```

要点：
- **catch 入口 label 处 `localK.l = stack0.l;`** 把异常对象写入对应局部槽（ExceptionOccurred 的返回值即异常引用）
- 多个 catch 体呈"线性 fallthrough"布局：前一个 catch 体尾部直接落入下一个 catch 体/汇合点，正常路径用 `if (1) goto` 跳过
- **finally 是内联双入口代码段**：正常路径 `goto L_afterFinally` 跳过；异常路径从各分发点 `goto L_finally`（finally 入口先把异常入槽），finally 结束后落入统一汇合点
- `return` 时若仍有 pending 异常（catch 分发不匹配），JNI 调用返回后 JVM 自动抛出——**JNIC 不做异常包装/重抛**，完全依赖 JNI 的 pending exception 机制
- 方法**没有** method-level 统一 try 包裹；分发块逐点内联，未匹配异常直接裸 return

### 4.16 返回

```c
return;                 // void
return stack0.i;        // int / boolean（jboolean 即 int）
return stack0.j;        // long
return stack0.l;        // 引用（jobject 就是指针）
return temp0.l;         // 异常路径返回默认值（temp 已 memset 0 = Java 默认值）
```

- 异常路径返回 `temp0.x`（int→`temp0.i`、引用→`temp0.l`，jvalue 已 memset 0 = Java 默认值/ NULL）

## 5. 注册机制（__00024jnicLoader）

```c
JNIEXPORT void JNICALL Java_pack_tests_basics_cross_Abst1__00024jnicLoader(JNIEnv *env, jclass clazz) {
    c_0_(env)->lookup = (*env)->NewGlobalRef(env, (*env)->CallStaticObjectMethod(env,
        c_1_(env)->clazz, c_1_(env)->id_0));    // 初始化本类的 MethodHandles.Lookup
    JNINativeMethod table[] = {
        {"mul", "(II)I", (void *) &jnic_0},
    };
    (*env)->RegisterNatives(env, clazz, table, 1);
}
```

- **每类一个 loader 导出函数**，名字 `Java_<internal>__00024jnicLoader`
- `<clinit>` **重命名为 `$jnicClinit`**（`$` 防冲突），注册为普通方法（实证 4 处：
  `{"$jnicClinit", "()V", (void*)&jnic_5}` 等）
- **构造器 `<init>` 不生成方法体、不进注册表**（TEST.jar 实证：50 个注册项无一为
  `<init>`；`<init>` 仅作为调用目标被 `GetMethodID` 缓存，构造逻辑仍由 JVM 执行）
- `c_1_` 是所有 loader 共用的 `MethodHandles.lookup()` 提供者

### 方法编号规则

- `jnic_N` 全局递增编号（0..49），与注册表顺序一致
- `cached_c_N` 的 N 是类缓存编号（0..70），独立于 jnic_N
- 编号顺序 = 字节码中首次出现顺序（方法体按其所在类注册表序，缓存按首次引用序）

### lambda 合成方法

lambda 语法糖方法（`lambda$run$0`）与普通方法同等对待：
```c
/* lambda$run$0(Lpack/tests/basics/runable/Exec;)V */
void JNICALL jnic_4(JNIEnv *env, jobject thisObj, jobject arg0) { ... }
```
- 照常生成方法体、进入注册表（`{"lambda$run$0", "...", (void*)&jnic_4}`）
- lambda 引用自己的方法体（`<init>` 通过 indy 运行时桥接调用）

## 6. 其他发现

### 6.1 字符串构造

所有字符串字面量 → `NewString(env, (unsigned short[]) {码元序列}, 长度)`：
- UTF-16 码元数组，字面内联（大字符串会很长）
- 每次执行都构造新 String（无缓存）

### 6.2 空检查

- 引用算术前无显式 NPE 检查——**依赖 JNI 调用的 ExceptionCheck**（GetField 等对 NULL
  receiver 会抛 NPE 到 ExceptionCheck 捕获）
- 部分场景有显式 `if (stack0.l == NULL)`（ATHROW 时）

### 6.3 线程/并发

- 无任何 monitor 特殊处理（MONITORENTER/EXIT 在 TEST.jar 中无实例，模板存在）

### 6.4 无优化特征

- 无寄存器分配：所有值都经 stack_N 显式搬运
- 无死代码消除：`stack1.l = stack1.l;` 这种自赋值都保留
- 无常量折叠：`(unsigned int) stack0.i + stack1.i` 每次重算
- 无数组边界检查优化
- **无异常优化**：每个 JNI 调用点都带完整 ExceptionCheck 分发（包括不会抛异常的
  `NewString`、纯算术后的检查——算术后无检查，但常量 LDC 后有）
- 无 finally 提取：finally 体逐份内联

### 6.5 覆盖统计（TEST.jar 37 类）

- 50 个方法全部生成，无 inline/内建替代
- `<clinit>` 改名 `$jnicClinit` 注册；lambda 合成方法照常生成
- **构造器不生成**（保留 Java 实现）

## 7. 生成器行为总结

| 观察 | 结论 |
|---|---|
| temp 槽固定 4 个 | 生成器把跨指令值统一压 temp0-3，不做精细 liveness |
| 每个 JNI 调用后 ExceptionCheck | 保守异常策略，性能开销大 |
| 无条件跳转 `if (1) goto` | 生成器风格统一 |
| 方法签名统一带 thisObj | 简化注册表（static 也兼容） |
| 类缓存原子化 | 线程安全但 FindClass 锁外执行 |
| invokedynamic 运行期桥接 | 不做编译期展开 |
| <clinit> 改名 $jnicClinit | 注册为普通方法（构造器则完全不生成） |
| 异常靠 JNI pending 机制 | 不包装不重抛，catch 靠逐点 IsInstanceOf 分发 |
| finally 双入口内联 | 正常路径跳过、异常路径经 localK.l=stack0.l 接管 |

## 8. 规模参考

TEST.jar（37 类 / 55 方法）产物：
- `jnic.c` 224KB / 5950 行
- 70 个缓存结构、50 个方法体、30 个 loader 函数
- 652 处 ExceptionCheck、1042 处 if/goto/函数边界
- 单方法最大：`run()`（jnic_3）含 3 个 invokedynamic 站点 + 2 catch + finally，约 200 行 C

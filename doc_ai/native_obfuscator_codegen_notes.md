# native-obfuscator 生成产物行为特征（逆向参考）

> 本文档基于**真实生成产物**描述 native-obfuscator（radioegor146 原版）输出代码的
> 行为特征——产物形态、类改写、运行时加载/注册/缓存行为、可识别指纹。
> 样本：原版生成器（3.5.4r，平台 hotspot）对 snake.jar（7 类）的完整输出。
> 商业变体"幻影盾"的授权验证情报见 `phantom_shield_codegen_notes.md`。

## 1. 产物形态

```
输出目录/
├── cpp/                                  # C++ 工程
│   ├── native_jvm.cpp/hpp                运行时框架（utils）
│   ├── native_jvm_output.cpp/hpp         JNI_OnLoad + 注册表
│   ├── string_pool.cpp/hpp               字符串池
│   ├── CMakeLists.txt                    C++17，MSVC/GNU/Clang
│   └── output/                           每类一个独立文件 + 隐藏类数据
│       ├── Main_2.cpp/hpp                <类名>_<序号>（类索引序号）
│       ├── ThreadsController_4.cpp       （214 KB，游戏主循环）
│       └── data_native0_hidden_Hidden0.cpp/hpp   隐藏类字节码
└── snake.jar                             输出 jar（改写类 + Loader）
```

- **每类一个独立 .cpp/.hpp**（`.cpp` 含全部翻译方法 + 注册函数；`.hpp` 只声明
  `__ngen_register_methods`）。
- 隐藏类（special_clinit/mhinvoke 等）不生成 .class 进 jar（非 Android），而是
  `class_data[jbyte数组]` 字节码 + 运行期 `DefineClass` 注入。
- 输出 jar 只含：改写类 + `<nativeN>/Loader.class` + 原资源（**DLL 是外部编译后
  手动放入 jar**）。

## 2. 每个翻译类的 C 代码行为

```cpp
// Main                                              ← 类名注释
namespace native_jvm::classes::__ngen_Main_2 {       ← 命名空间 = 类名_序号

    char *string_pool;                               // 全局池指针
    jstring cstrings[2];                             // 类名（点分）jstring 缓存
    std::mutex cclasses_mtx[1];  jclass cclasses[1]; // 类缓存（弱引用）
    jmethodID cmethods[5];                           // 方法 ID 缓存

    // main([Ljava/lang/String;)V                    ← 方法注释
    void JNICALL __ngen_native_main1(JNIEnv *env, jclass clazz, jarray arg0) {
        jobject classloader = utils::get_classloader_from_class(env, clazz);
        ...
        jvalue cstack0..2 = {};  jvalue clocal0..1 = {};   // 槽数与 maxStack/maxLocals 对应
        std::unordered_set<jobject> refs;                  // 引用跟踪

        // LABEL L1; Stack: 0                         ← 每行注释：标签/指令/栈深
        L1: if (env->ExceptionCheck()) { return; }
        // NEW Window; Stack: 0
        if (!cclasses[0] || IsSameObject(cclasses[0], NULL)) {  // 类缓存：双检锁
            cclasses_mtx[0].lock(); ... FindClass via classloader ... NewWeakGlobalRef
            ... }
        if (jobject obj = env->AllocObject((cclasses[0]))) { cstack0.l = obj; refs.insert(obj); }
        // INVOKESPECIAL Window.<init>()V; Stack: 2
        if (!cmethods[0]) { cmethods[0] = env->GetMethodID((cclasses[0]), <池偏移>, <池偏移>); }
        if (cstack1.l == nullptr) throw_re(NPE, "<池偏移>", 8);
        else env->CallNonvirtualVoidMethod(...);
        ...
        // ALOAD 1; Stack: 0
        cstack0.l = clocal1.l; refs.insert(cstack0.l);
    }
    void __ngen_register_methods(JNIEnv *env, jclass clazz) {
        string_pool = string_pool::get_pool();
        // cstrings 初始化：NewStringUTF → intern → NewGlobalRef
        JNINativeMethod __ngen_methods[] = {
            { <池偏移>, <池偏移>, (void *)&__ngen_native_main1 }, ... };
        env->RegisterNatives(clazz, __ngen_methods, N);
        // （如有）隐藏方法注册：FindClass + __ngen_hidden_methods
    }
}
```

行为要点：
- **缓存全部内联在调用点**（每处方法调用都带一段双检锁类缓存 + ID 缓存代码）——
  产物膨胀但无需缓存函数表（幻影盾将这段封装成 `_CacheClass0` 等函数，见变体文档）。
- **类缓存用弱引用**（NewWeakGlobalRef），允许类卸载。
- **类解析走 classloader**：`utils::find_class_wo_static(env, classloader, cstrings[点分名])`
  （loadClass 语义，异常转 NoClassDefFoundError）；数组类走 FindClass。
- **引用管理**：每个 jobject 结果都 `refs.insert(...)`，方法出口统一清理；
  局部引用不依赖 JVM 引用表自然回收。
- **异常检查逐点**：每个 JNI 调用后 `if (env->ExceptionCheck()) return;`
  （TRY 块内则是进捕获块：`goto L_CATCH_N`）。
- **空指针前置**：方法调用/字段访问前 `if (ref == nullptr) throw_re(NPE, "<操作名> npe", <行号>)`。
- **源行号残留**：`// Line 8; Stack: 0` 注释保留字节码行号。

## 3. 方法翻译行为（指令 → C）

| 指令 | 生成行为 |
|---|---|
| 常量 | `cstackN.i = 5;`（数字明文）；字符串 → `cstrings[N]` 或池偏移（见 §6） |
| 栈操作 | `DUP → cstackN = cstackM;`（整槽拷贝）；`POP → ;`（靠栈指针覆盖） |
| 算术 | 直接运算；`IDIV/IREM` 带 `-1 && INT_MIN` 特判与除零 throw_re；`FREM → std::fmod` |
| 移位 | `<< (0x1f & n)`；无符号右移 `(jint)(((uint32_t)x) >> (((uint32_t)n) & 0x1f))` |
| 比较 | 三目；FCMPL/G NaN 语义 `value1>value2?1:(==?0:(<?-1:±1))` |
| 分支 | `if (cond) goto L_N;`；`goto → goto L_N;` |
| switch | `switch (cstackN.i) { case k: goto L; default: goto L; }` |
| 数组 | `Get*ArrayRegion(arr, idx, 1, &槽)`（Region 单元素）；boolean/byte 走 `utils::baload/bastore`；负尺寸 throw_re(NASE) |
| 字段 | `Get*/Set*Field(..., <cmethods/cfields 缓存>)`；实例访问前 null 检查 |
| 方法调用 | `Call*Method`；`INVOKESPECIAL → CallNonvirtual*Method`（带 `$class_ptr`）；对象返回 `refs.insert` |
| invokedynamic | 运行时自行调 bootstrap（见 §5） |
| multianewarray | `utils::create_multidim_array(env, classloader, ...)` |
| monitor | `env->MonitorEnter/Exit`（null 检查） |

## 4. 类改写行为（javap 实证）

翻译类改写后：

```java
public class Main {
  public static native void main(String[]);      // 原方法 → native（保留签名/访问标志）
  static {};                                     // clinit 重写为 3 条：
      ldc  2                                     //   类索引
      ldc  class Main
      invokestatic native0/Loader.registerNativesForClass:(ILjava/lang/Class;)V
      ldc  class Main
      invokestatic native0/hidden/Hidden0.special_clinit_2_20:(Ljava/lang/Class;)V
      return
}
```

- 原方法 `access |= ACC_NATIVE`；`<init>` **不翻译**（保留 Java）。
- **clinit 原逻辑整体移入隐藏类 native 方法**（`special_clinit_<类>_<方法>`），
  Java clinit 只剩"注册 + 调用隐藏代理"两件事。
- 接口默认方法 → 转发到隐藏类 `interfacestatic_*`。
- 类文件版本强制 52（Java 8）。
- 隐藏类（`native<N>/hidden/HiddenN`）带 `LambdaForm$Hidden` +
  `jdk/internal/vm/annotation/Hidden` 注解（class_data 常量池可见）。

## 5. invokedynamic / MethodHandle 行为

- invokedynamic 在字节码层面展开（翻译前）：**运行时自己调 bootstrap**
  （参数查表/装箱 → INVOKESTATIC bsm → getTarget），随后
  - hotspot 平台：走 `utils::link_call_site`（JVM 内部 linkCallSite）+ 隐藏方法
    `mhinvoke`/`invokereverse`（包装 MethodHandle.invoke）；
  - std_java/android：`MethodHandle.invokeWithArguments(Object[])`。
- 翻译代码中 `MethodHandles.lookup()`/`getClassLoader()`/`getClass()` 被替换为
  方法体局部变量（`jobject lookup = nullptr;` 惰性初始化）。
- **识别特征**：产物中出现 `java/lang/invoke/CallSite`、`getTarget`、
  `LambdaMetafactory`、`utils::link_call_site` 的字符串/调用。

## 6. 字符串池行为

```cpp
namespace native_jvm::string_pool {
    static char pool[N] = { 109, 97, 105, 110, 0, 40, 91, 76, ... };  // "main\0([Ljava/lang/String;)V\0..."
    char *get_pool() { return pool; }
}
```

- **单一连续 ASCII 池**：所有字符串（方法名/描述符/类名/字面量/错误消息）按序拼接，
  NUL 分隔，访问 `((char *)(string_pool + <offset>LL))`。
- 池内可读到全部方法名、描述符、异常消息、字符串字面量——**逆向时直接转储即可
  还原符号表**（数值数组 → ASCII）。
- 无加密（仅防 strings 扫描）。

## 7. 注册与加载行为链

```
类 <clinit> → Loader.registerNativesForClass(类索引, 类)
            → native register_for_class → reg_methods[类索引](env, clazz)
            → 该类的 __ngen_register_methods → RegisterNatives

JNI_OnLoad → prepare_lib → utils::init_utils
            → [reg_methods 填表 + DefineClass 注入隐藏类]
            → RegisterNatives(Loader, {registerNativesForClass, ...})

Loader.<clinit>（Java）→ os.name/os.arch 判定 → 读 /native<N>/<平台>-<扩展>
            → 临时文件 → System.load
```

- **注册延迟到类加载时**（clinit 触发），不是 JNI_OnLoad 一次性注册。
- 平台资源命名：`x64-windows.dll` / `x64-linux.so` / `x64-macos.dylib` / arm 变体。
- Loader 类名 `native<N>/Loader`（N 自动避让已有目录）；`registerNativesForClass`
  是唯一 native 注册入口。

## 8. 逆向识别指纹（拿到未知产物如何认）

| 特征 | 说明 |
|---|---|
| 符号/命名 | `native_jvm::classes::__ngen_<类>_<N>` 命名空间、`__ngen_native_<名><N>` 方法、`__ngen_register_methods` |
| 每行注释 | `// LABEL L1; Stack: N`、`// Line N; Stack: N`（**生成器不清理注释**） |
| 缓存模式 | 调用点内联双检锁 + `NewWeakGlobalRef` + `cmethods[i]` 惰性 GetMethodID |
| 字符串池 | `string_pool + <N>LL` 形态（唯一无 `((char *)` 转换则少见） |
| 隐藏类 | `native<N>/hidden/Hidden0` + DefineClass 注入 + `special_clinit_*`/`interfacestatic_*` 方法名 |
| 引用管理 | `std::unordered_set<jobject> refs` + `utils::clear_refs` |
| 异常消息 | `"<操作> npe"`（如 `IALOAD npe`）、`"cannot cast to "`、`classloader == null` |
| Loader | `registerNativesForClass` native + os/arch 分发的资源加载 |
| 框架 | `utils::init_utils`、`find_class_wo_static`、`throw_re(env, cls, msg, line)` |

## 9. 行为差异：原版 vs 幻影盾变体（简表）

| 行为 | 原版 | 幻影盾（snake 样本） |
|---|---|---|
| 文件组织 | 每类独立 .cpp/.hpp | **合并为单文件**（output.cpp 1.66MB + inline.cpp） |
| 类名 | 保留原类名 | **重命名** `skidonion/nrgui/<单字母>` |
| 方法名 | `__ngen_native_<原方法名><N>` | `__ngen_native_<字母><N>`（混淆） |
| 缓存代码 | 调用点内联展开 | 封装为 `_CacheClass0/1`、`_CacheMethod` 等函数 |
| 字符串池 | 明文 ASCII | 同（明文） |
| Loader 名 | `native<N>/Loader` + `registerNativesForClass` | `skidonion/nrgui/___` + `___` |
| 授权 | 无 | 授权方法提取到 inline 层 + Ed25519 验签 + 云心跳（见变体文档） |
| 隐藏类 | `native<N>/hidden/Hidden0` | 同机制（`__ngen_data_` + DefineClass） |

# native-obfuscator 代码生成深度分析（re146 / radioegor146 原版）

> 本文档对开源 native-obfuscator（github.com/radioegor146/native-obfuscator）的
> **生成器源码**做逐模块分析，还原 Java 字节码 → C++ 的完整翻译机制。
> 所有结论来自源码（obfuscator/src/main/）与生成模板（resources/sources/），
> 非猜测。商业变体"幻影盾"的生成产物分析见 `phantom_shield_codegen_notes.md`。
>
> 源码位置：`E:\canrad\reverse\jvm\native_obf\native-obfuscator`（master @ e481761，
> tag 至 3.5.4r）

## 1. 架构总览

```
src/main/java/by/radioegor146/
├── NativeObfuscator.java            主流程（jar 逐类处理、类改写、产物组装）
├── MethodProcessor.java             方法翻译主循环（opcode→snippet、栈指针）
├── MethodContext.java               翻译上下文（输出缓冲/缓存/标签池）
├── Snippets.java                    模板渲染（cppsnippets.properties）
├── NodeCache.java                   类/方法/字段/字符串缓存表（全局编号）
├── CachedMethodInfo/CachedFieldInfo 方法/字段缓存键
├── CatchesBlock.java                异常捕获块集合（去重/链式）
├── LabelPool.java                   Label → "L%d" 编号
├── HiddenMethodsPool.java           隐藏方法池（mhinvoke 等）
├── HiddenCppMethod.java             隐藏方法 ↔ C 函数绑定
├── Platform.java                    STD_JAVA / HOTSPOT / ANDROID
├── bytecode/                        预处理（翻译前改写字节码）
│   ├── PreprocessorRunner.java
│   ├── PreprocessorUtils.java       magic 方法占位符
│   ├── IndyPreprocessor.java        invokedynamic 展开
│   ├── LdcPreprocessor.java
│   ├── MethodHandleUtils.java
├── instructions/                    15 类指令 handler（见 §4）
├── source/                          产物组装
│   ├── ClassSourceBuilder.java      每类 .cpp/.hpp（缓存数组+注册函数）
│   ├── MainSourceBuilder.java       native_jvm_output.cpp（reg_methods）
│   ├── StringPool.java              字符串池构建
│   └── CMakeFilesBuilder.java
├── special/                         <clinit>/普通方法/接口方法处理
└── compiletime/                     Loader 类（Java 侧加载器模板）
src/main/resources/sources/
├── cppsnippets.properties           ★ 574 行指令→C++ 模板表
├── native_jvm.cpp/hpp               运行时框架（utils）
├── native_jvm_output.cpp/hpp        JNI_OnLoad 模板
├── string_pool.cpp/hpp              字符串池模板
└── CMakeLists.txt
```

**核心设计**：翻译 = **模板 + 替换**。每个 JVM 指令对应 `cppsnippets.properties`
中一段 C++ 模板（key = ASM Opcodes 常量名），handler 填充占位符（栈槽号/标签/
字符串池偏移/缓存数组下标）后拼进方法体。

## 2. 指令翻译机制（MethodProcessor + Snippets）

### 2.1 opcode → 模板名

```java
// MethodProcessor 静态块：反射枚举 ASM Opcodes 全部常量
for (Field f : Opcodes.class.getFields())
    INSTRUCTIONS.put((int) f.get(null), f.getName());   // 例如 182 → "INVOKEVIRTUAL"
```
模板 key 与 JVM 指令名一一对应；特殊指令由 handler 动态拼接后缀
（如 `INVOKEVIRTUAL_<返回值sort>`、`GETSTATIC_<字段sort>`、`NEWARRAY_<atype>`）。

### 2.2 类型系统映射

```java
CPP_TYPES = {void, jboolean, jchar, jbyte, jshort, jint, jfloat, jlong, jdouble, jarray, jobject, jobject}
            // sort 0..11（ASM Type）
TYPE_TO_STACK = {1,1,1,1,1,1,1,2,2,0,0,0}   // 类型 → 栈槽数（long/double 占 2）
```

### 2.3 栈指针（stackPointer）维护

每个 handler 实现 `getNewStackPointer(node, sp)`：按指令语义增减栈深
（ILOAD +1、LSTORE -2、INVOKEVIRTUAL = -1 - 参数槽和 + 返回槽…）。翻译时每行
注释输出当前栈深，模板用 `$stackindexN`（sp-5..sp+5）定位操作数。

### 2.4 handler 管线（GenericInstructionHandler.accept）

```
1. 收集该指令位置上"活跃"的 try/catch 块（按 tryCatch 类型去重）
   ——LabelHandler 在进入 try start 时加入 tryCatches、到 end 时移除，
   指令的 trycatchhandler 即"当前捕获集合"的 C++ 片段：
     · 无捕获：   if (env->ExceptionCheck()) { return (rettype)0; }
     · 有捕获：   if (env->ExceptionCheck()) { jthrowable exception = ...; cstack0.l = exception;
                  goto L_CATCH_N; }
2. 生成属性：line / rettype / stackindex±5 / trycatchhandler
3. process(context, node)（handler 专属：填充 operand/缓存/参数等 props）
4. 渲染：context.output.append(snippets.getSnippet(instructionName, props))
```

## 3. 指令模板要点（cppsnippets.properties 摘录）

| 类 | 模板模式 |
|---|---|
| 常量 | `ICONST_1 → cstack$stackindex0.i = 1;`；`LDC_STRING → cstack0.l = $cst_ptr;`（字符串池指针） |
| 加载/存储 | `ILOAD → cstack0.i = clocal$var.i;`；`ASTORE → clocal$var.l = cstackm1.l; refs.insert(...)` |
| 数组 | `*ALOAD → if (ref==nullptr) throw_re(NPE,"IALOAD npe",line); else GetIntArrayRegion(arr, idx, 1, &cstackm2.i);`（**Region 单元素批量接口**）；boolean/byte 二义统一走 `utils::baload/bastore` |
| 算术 | `IADD → cstackm2.i = cstackm2.i + cstackm1.i;`；`IDIV →` 特判 `-1 && INT_MIN` 与除零 throw_re；`IREM` 除零检查；`FREM → std::fmod` |
| 移位 | `ISHL → << (0x1f & n)`；`IUSHR → (jint)(((uint32_t)x) >> (((uint32_t)n) & 0x1f))` |
| 比较 | `LCMP → (a==b)?0:(a>b?1:-1)`；`FCMPL/FCMPG → value1>value2?1:(==?0:(<?-1:±1))`（NaN 语义） |
| 栈操作 | `DUP → cstack0 = cstackm1;`（整槽拷贝）；`DUP_X1/DUP_X2/DUP2_X1/DUP2_X2` 旋转；`SWAP → std::swap`；`POP/POP2 → ;`（空操作，靠指针覆盖） |
| 分支 | `IF_ICMPxx → if (cstackm2.i cmp cstackm1.i) goto $label;`；`IF_ACMPEQ → env->IsSameObject(...)` |
| switch | `TABLESWITCH_START/PART/DEFAULT/END → switch (cstackm1.i) { case $index: goto $label; ... }` |
| new | `NEW → if (jobject obj = env->AllocObject($desc_ptr)) { cstack0.l = obj; refs.insert(obj); }` |
| 字段 | `GETSTATIC_<sort>`/`GETFIELD_<sort>`/`PUT*`：按 sort 选 Get/Set 方法；**GETFIELD/PUTFIELD 前空指针检查** |
| 方法调用 | `INVOKEVIRTUAL_<sort>`/`INVOKEINTERFACE_<sort>`/`INVOKESPECIAL_<sort>`/`INVOKESTATIC_<sort>`：按返回值 sort 选 Call*Method；invokespecial 用 `CallNonvirtual*Method`；**object 返回 refs.insert** |
| checkcast | `if (ref != nullptr && !IsInstanceOf(ref, $desc_ptr)) throw_re(CCE, "cannot cast to "+desc, line)` |
| athrow | `if (ref==nullptr) throw_re(NPE) else { env->Throw(ref); }` |
| monitor | `MonitorEnter/MonitorExit`（null 检查） |
| multianewarray | `utils::create_multidim_array(env, classloader, count, required_count, desc, line, dims)` |

## 4. 缓存机制（NodeCache + 生成代码）

每类一个 namespace，四个扁平缓存数组（按全局 ID 编号，`NodeCache.getId` 惰性分配）：

```cpp
jstring  cstrings[N];                    // 类名（点分）jstring，intern+NewGlobalRef
std::mutex cclasses_mtx[N];  jclass cclasses[N];
jmethodID cmethods[N];  jfieldID cfields[N];
```

- **类缓存**（方法调用点内联生成，非独立函数）：
  ```cpp
  if (!cclasses[7] || env->IsSameObject(cclasses[7], NULL)) {
      cclasses_mtx[7].lock();
      if (!cclasses[7] || ...) { if (jclass clazz = <类获取>) cclasses[7] = (jclass) env->NewWeakGlobalRef(clazz); }
      cclasses_mtx[7].unlock(); <异常检查>
  }
  ```
  双检锁 + **NewWeakGlobalRef（弱引用）**。类获取分两种：
  `getClassGetter`：数组类 `env->FindClass(desc)`；普通类
  `utils::find_class_wo_static(env, classloader, <cstrings 点分名>)`（classloader.loadClass，
  异常转 NoClassDefFoundError）。
- **方法/字段 ID**（调用点内联）：
  ```cpp
  if (!cmethods[9]) { cmethods[9] = env->GetStaticMethodID(<类>, <名>, <描述符>); }
  ```
  名称/描述符来自字符串池偏移。
- **cstrings**：在 `__ngen_register_methods` 里统一初始化（NewStringUTF → intern →
  NewGlobalRef），用于 classloader 加载路径的类名。

## 5. 方法翻译全流程（MethodProcessor.processMethod）

```
1. 特殊处理器分发：<init> 跳过；<clinit> → ClInit；其他 → Default
2. 方法名：__ngen_ + (native_<名><idx> | special_clinit_<class>_<idx> | interfacestatic_...)
3. 签名：<ret> JNICALL 名(JNIEnv*, [jobject ignored_hidden,] jclass|jobject obj, <args>)
   · 非 static 开头注入：
       clazz = get_class_from_object(obj) → classloader = get_classloader_from_class(clazz)
       → (classloader==nullptr → FatalError("classloader == null"))
       → clazz = find_class_wo_static(classloader, <本类点分名>)   // 重新解析本类
4. try/catch 涉及的异常类预缓存（WeakGlobalRef）
5. 声明 cstack0..maxStack-1、clocal0..maxLocals-1（用 ASM maxStack/maxLocals）
   + std::unordered_set<jobject> refs
6. 参数 → clocal（LOCAL_LOAD_ARG_<sort> 模板，refs 跟踪引用）
7. 逐指令循环：// <insn>; Stack: N 注释 → accept() 输出模板代码 → 更新栈指针
8. 尾部 return (rettype)0
9. catch 块展开：L_CATCH_N: → TRYCATCH_CHECK_STACK（IsInstanceOf 匹配→goto handler）
   → 不匹配链到下一个 L_CATCH 或 TRYCATCH_END_STACK（重抛）
10. 清理 ASM 元数据（localVariables/tryCatchBlocks），postProcess 改写 Java 方法
```

## 6. try/catch 展开（顺序模拟）

- LabelHandler 维护"活跃 try 集合"：label 为某 try 块 start 时加入 `tryCatches`，
  为 end 时移除；每条指令据此生成 trycatchhandler 前缀（进捕获块或仅异常检查）。
- 捕获块以 `CatchesBlock`（异常类型序列）为键去重，链式生成：
  `L_CATCH_0: if (IsInstanceOf(cstack0.l, <类>)) goto <handler>; goto L_CATCH_1;` 最后
  `L_CATCH_N: env->Throw(cstack0.l); return (rettype)0;`（不匹配重抛）。

## 7. invokedynamic 全链路（IndyPreprocessor 字节码层展开）

invokedynamic **不直接翻译**——翻译前在字节码层面展开为普通指令序列：

```
[参数装箱入 Object[]]
→ push lookup（预处理替换的 LOOKUP_LOCAL magic 方法）
→ push name / MethodType（MethodHandleUtils 生成 ldc）
→ push bsmArgs（String/Type/Integer/Long/Float/Double/Handle/Object[] 递归展开，装箱）
→ INVOKESTATIC <bsm 所在类>.<bsm 名>（运行时调 bootstrap！）
→ CHECKCAST CallSite → INVOKEINTERFACE CallSite.getTarget
→ [调用目标 MethodHandle]
分支：STD_JAVA/ANDROID → MethodHandle.invokeWithArguments(Object[])
      HOTSPOT → 替换为魔法方法 native/magic/1/invoke/... （→ MethodHandler 换隐藏方法 mhinvoke）
整个 bootstrap 段包 try/catch → BootstrapMethodError（bsme 块）
```

- **魔法方法占位**（PreprocessorUtils）：`MethodHandles.lookup()` /
  `getClassLoader()` / `getClass()` 调用在预处理中替换为 magic owner
  （`native/magic/1/lookup/obfuscator<随机>.a`）→ MethodHandler 识别后直接输出
  `cstackN.l = lookup/classloader/clazz;`（方法体局部缓存变量）。
- **HOTSPOT 专用**：`link_call_site` 走 `utils::link_call_site(...)`（native 侧用
  JVM 内部 API linkCallSite），返回 MemberName 数组；invoke 走隐藏方法。

## 8. 隐藏方法池（HiddenMethodsPool）

运行时需要的 JVM 内部调用被包装成**隐藏类**（`LambdaForm$Hidden` +
`jdk/internal/vm/annotation/Hidden` 注解的 native 方法）：
- `mhinvoke`：包装 `MethodHandle.invoke`（invokeExact/invoke 翻译目标）
- `invokereverse`：invoke 逆序参数包装
- `special_clinit_*`：<clinit> 原逻辑代理
- `interfacestatic_*`：接口方法代理

非 Android 平台：隐藏类字节码 → `native_jvm::data::__ngen_<类>::class_data[]`
（jbyte 数值数组）+ `DefineClass` 注入；Android：直接写 .class 进 jar。

## 9. 类改写（postProcess）

| 场景 | 改写 |
|---|---|
| 普通方法 | `access |= ACC_NATIVE`，注册名 `__ngen_native_<名><idx>` |
| `<clinit>` | 清空指令 → `Loader.registerNativesForClass(classIndex, 类.class)` + `special_clinit_N_M(类)`（隐藏类 native 调用）；原逻辑翻译成隐藏类 native 方法 |
| 接口方法 | 原方法改为转发（调隐藏类 native `interfacestatic_N_M`），隐藏类内翻译 |
| 类版本 | `classNode.version = 52`（强制 Java 8） |

## 10. 注册与装载

```
每类 .cpp：
  void __ngen_register_methods(JNIEnv*, jclass) {
      string_pool = string_pool::get_pool();
      [cstrings 初始化（intern + global）]
      JNINativeMethod __ngen_methods[] = { {池偏移名, 池偏移描述符, &__ngen_native_*} };
      env->RegisterNatives(clazz, __ngen_methods, N);
      [hidden 类方法注册：FindClass + __ngen_hidden_methods]
  }
native_jvm_output.cpp（模板 + MainSourceBuilder 填充）：
  reg_method reg_methods[N];                       // 类注册函数表
  void register_for_class(JNIEnv*, jclass, jint id, jclass clazz) { reg_methods[id](env, clazz); }
  void prepare_lib(JNIEnv*) {
      utils::init_utils(env);
      [reg_methods 赋值 + DefineClass 隐藏类]
      RegisterNatives(<nativeDir>/Loader, {"registerNativesForClass", "(ILjava/lang/Class;)V", &register_for_class}, 1);
  }
  JNI_OnLoad → prepare_lib → JNI_VERSION_1_8
Java 侧 Loader（LoaderUnpack 模板，改包名 native<N>/Loader）：
  static { os/arch 判断 → 读 /native<N>/<平台>-<扩展> → temp 解压 → System.load }
  native void registerNativesForClass(int, Class)
```

调用链：类 `<clinit>` → `Loader.registerNativesForClass` → `register_for_class` →
`reg_methods[id]`（对应类的 `__ngen_register_methods`）→ RegisterNatives。

## 11. 字符串池（StringPool）

- 全部字符串（方法名/描述符/类名/字面量/错误消息）惰性入池：offset 递增
  （Modified-UTF-8 字节数 + 1 个 NUL），引用 `((char *)(string_pool + <offset>LL))`。
- 池实现：`static char pool[N] = { 109, 97, 105, 110, 0, ... }`（明文数值数组，
  NUL 分隔），`get_pool()` 返回指针。
- 仅防 strings 扫描，无加密（对比 JNIC 3.7.0 stringObf=true 的 chacha20）。

## 12. 生成产物结构（与原版输出对比）

原生版输出目录：`cpp/`（native_jvm.* 框架模板 + string_pool.cpp +
native_jvm_output.cpp + CMakeLists.txt）+ `cpp/output/`（**每类独立
`<类名>_<idx>.cpp/.hpp`** + 隐藏类 data_*.cpp/.hpp）+ 输出 jar（改写类 + native<N>/
资源 + Loader）。

注意：商业变体"幻影盾"将**每类独立文件合并为单个 output.cpp** 并追加授权层
（Ed25519 验签/云验证），见 `phantom_shield_codegen_notes.md`。

## 13. 关键设计小结

1. **模板驱动**：指令语义全部集中在一张 properties 表，handler 只做参数定位，
   扩展新指令只需加模板。
2. **栈机直译**：cstack/clocal 固定槽 + 编译期栈指针，无真栈、无寄存器分配。
3. **引用管理**：所有 jobject 进 `refs` 集合，方法出口统一 DeleteLocalRef
   （JNIC 无此机制，靠 JVM 局部引用表自然回收）。
4. **弱引用类缓存**：NewWeakGlobalRef（类可被卸载）；JNIC 用强引用 NewGlobalRef。
5. **字节码层预处理优先**：invokedynamic/lookup 等复杂构造在翻译前改写为普通指令，
   翻译器保持简单。
6. **隐藏类承载 JVM 内部语义**：mhinvoke/link_call_site/special_clinit 均通过
   带 Hidden 注解的隐藏类 native 方法实现。
7. **clinit 作为注册钩子**：每个翻译类的 <clinit> 都变成
   `Loader.registerNativesForClass(id, clazz)` + 原逻辑的隐藏代理调用。

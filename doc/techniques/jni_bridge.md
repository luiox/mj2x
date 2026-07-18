# JNI 桥接层

通过 JNI（Java Native Interface）在原生代码和 JVM 之间传递数据、调用方法和维护引用。

## 问题

JVM 字节码编译为原生代码后，仍然需要与 JVM 交互：

- 调用未被编译的 JDK 方法
- 访问 Java 对象的字段
- 处理字符串（`java.lang.String` 的 UTF-16 编码）
- 管理 Java 对象的引用（防止 GC 回收）
- 响应异常

JNI 是 Java 标准定义的 native ↔ JVM 边界协议，所有 JVM 实现都支持。

## 引用管理

### 本地引用（LocalRef）

JNI 函数返回的引用默认是本地引用。它们在 native 方法返回时被释放。

```c
// 每次 JNI 调用返回一个本地引用
jclass clazz = (*env)->FindClass(env, "java/lang/String");
jmethodID mid = (*env)->GetMethodID(env, clazz, "length", "()I");

// 方法返回后，clazz 和 mid 可以被释放
// 但 mid 是 jmethodID（指针），不是引用，不需要释放
```

本地引用的容量有限（通常 16~64 个 slot）。在循环或深层调用链中需要及时释放：

```c
for (int i = 0; i < 1000; i++) {
    jstring s = (*env)->NewString(env, ...);
    // 使用 s
    (*env)->DeleteLocalRef(env, s);  // 手动释放，避免溢出
}
```

### 批量释放

大量 JNI 调用产生的本地引用可以通过 `PopLocalFrame` 批量释放：

```c
// 创建新的本地引用帧
if ((*env)->PushLocalFrame(env, 128) == 0) {
    for (int i = 0; i < many; i++) {
        jstring s = (*env)->NewString(env, ...);
        // 使用 s
    }
    // 一次释放帧内所有引用
    (*env)->PopLocalFrame(env, NULL);
}
```

### 全局引用（GlobalRef）

需要跨方法调用或跨线程保留的引用必须提升为全局引用：

```c
// 创建全局引用（不会被 GC 回收，直到手动释放）
static jclass cachedClass = NULL;

jclass getCachedClass(JNIEnv* env) {
    if (cachedClass == NULL) {
        jclass local = (*env)->FindClass(env, "com/example/Foo");
        cachedClass = (*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    return cachedClass;
}
```

### 弱全局引用（WeakGlobalRef）

弱全局引用允许引用对象但不阻止 GC 回收：

```c
static jweak cachedObj = NULL;

jobject getCachedObj(JNIEnv* env) {
    if (cachedObj == NULL) {
        jobject local = createObject(env);
        cachedObj = (*env)->NewWeakGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }

    // 每次使用前检查是否已被 GC 回收
    jobject obj = (*env)->NewLocalRef(env, cachedObj);
    if (obj == NULL) {
        // 对象已被回收，重新创建
        cachedObj = NULL;
        return getCachedObj(env);
    }
    return obj;
}
```

## 字段与方法 ID 缓存

`GetFieldID` / `GetMethodID` 需要反查字符串，性能开销大。编译后的原生代码应将 ID 缓存为全局静态变量：

```c
// 惰性缓存模式
static jmethodID MID_run = NULL;

jmethodID getRunMethod(JNIEnv* env) {
    if (MID_run == NULL) {
        jclass clazz = (*env)->GetObjectClass(env, obj);
        MID_run = (*env)->GetMethodID(env, clazz, "run", "()V");
        // jmethodID 是裸指针，不需要 DeleteLocalRef
        // 但 clazz 是本地引用，需要释放
        (*env)->DeleteLocalRef(env, clazz);
    }
    return MID_run;
}
```

缓存 ID 不会失效（类卸载时才会无效，但类卸载在大多数 JVM 实现中罕见）。字段和方法 ID 不涉及 GC 安全，不需要作为引用管理。

## 字符串编码

Java 的 `String` 内部是 UTF-16 编码，C 代码通常用 UTF-8 或 wchar_t：

### Java → C

```c
// GetStringChars → 直接访问 UTF-16（可能返回拷贝）
const jchar* chars = (*env)->GetStringChars(env, str, NULL);
// 处理 UTF-16 字符...
(*env)->ReleaseStringChars(env, str, chars);

// GetStringUTFChars → 转换为 UTF-8
const char* utf8 = (*env)->GetStringUTFChars(env, str, NULL);
printf("%s\n", utf8);
(*env)->ReleaseStringUTFChars(env, str, utf8);
```

### C → Java

```c
// 从 UTF-16 创建
jstring str = (*env)->NewString(env, chars, len);

// 从 UTF-8 创建
jstring str = (*env)->NewStringUTF(env, "hello");
```

编译后的原生代码中，字符串常量会频繁创建。可以通过字符串池去重减少 JNI 调用：

```c
// 编译时预计算字符串的哈希，运行时查池
jstring getPooledString(JNIEnv* env, int poolIndex) {
    static jstring pool[256];
    if (pool[poolIndex] == NULL) {
        const char* s = stringTable[poolIndex];
        pool[poolIndex] = (*env)->NewStringUTF(env, s);
        pool[poolIndex] = (*env)->NewGlobalRef(env, pool[poolIndex]);
    }
    return pool[poolIndex];
}
```

## 异常处理

JNI 调用抛出异常后，后续大部分 JNI 函数不能正常执行。需要检查并在适当时机处理：

```c
// 调用可能抛异常的方法后
(*env)->CallVoidMethod(env, obj, mid);

// 检查异常
if ((*env)->ExceptionCheck(env)) {
    // 处理：清除异常或一路抛到 JVM
    (*env)->ExceptionDescribe(env);  // 打印
    (*env)->ExceptionClear(env);     // 清除
    return;  // 返回后让调用方处理
}
```

编译后的代码入口应该检查异常并回传到 JVM：

```c
JNIEXPORT void JNICALL Java_com_example_Foo_run(JNIEnv* env, jobject this) {
    // 生成的主代码
    if (exceptionPending) {
        return;  // JVM 会自动从 JNI 返回后重抛异常
    }
}
```

## 线程与 JNIEnv

`JNIEnv` 是线程局部的 —— 每个 Java 线程有独立的 `JNIEnv` 指针。生成的代码需要：

1. 从函数参数接收 `JNIEnv*`
2. 如果需要在工作线程中调用 JNI，先通过 `JavaVM->AttachCurrentThread` 获取 `JNIEnv*`

```c
// 在工作线程中
JavaVM* jvm = getGlobalJvm();
JNIEnv* env;
(*jvm)->AttachCurrentThread(jvm, &env, NULL);
// 使用 env
(*jvm)->DetachCurrentThread(jvm);
```

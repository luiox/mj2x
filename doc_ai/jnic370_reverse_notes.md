# JNIC 3.7.0 逆向笔记

> 本文档记录对 JNIC 3.7.0 的逆向取证结果，含两条独立取证线：
>
> **静态线**：对引擎 DLL `jnic37_native.dll`（7558656 B，PE64）的 IDA 分析。
> **动态线**：通过破解版（frida + licence 工厂替换）驱动 3.7.0 生成器对
> TEST.jar / 自研 jar 实际生成，**抓取生成中间产物 `jnic.c`（C 源码）与编译产物**，
> 直接读源码确证生成机制。
>
> 素材位置：
> - 引擎 DLL：`C:\Users\Canrad\AppData\Local\Temp\opencode\jnic37_native.dll` + `.i64`（IDA 9.0）
> - 破解包：`E:\canrad\reverse\jvm\native_obf\jnic3.7.0\jnic370.zip`（jnic370.jar + patcher/autopatch.py + patch-cagent.dll）
> - 3.7.0 生成源码（stringObf=true，HelloWorld 3 方法）：`C:\Users\Canrad\AppData\Local\Temp\opencode\jnic_test\caught\jnic7155671103524183033.c`（29.7 KB）
> - 3.7.0 生成源码（stringObf=false，TEST.jar 31 类 50 方法）：`...\caught\jnic10602900925539798838.c`（268 KB）
> - 3.5.1 生成源码（同输入同配置）：`...\gen_c\jnic.c`（224 KB，已归档 jnic_capture/）

## 1. 引擎 DLL 结构总览（静态线）

| 项 | 值 |
|---|---|
| 大小 | 7558656 B（.text 6.56 MB / .rdata ~400 KB / .data 128 KB） |
| 编译 | x64，C（生成代码为 GCC/zig 风格：`__attribute__`/`__builtin_expect`） |
| 导入 | 仅 KERNEL32 / msvcrt / SHELL32——**零 JNI import**（全部经 `env->functions` 间接调用） |
| 函数数 | 2440（约 2345 个匿名 `sub_`） |
| 导出 | 94 个 `Java_dev_jnic_aN__00024jnicLoader` + `JNI_OnLoad` |
| JNI_OnLoad | 单函数 553 KB，集中式注册入口 + 字符串密钥池生成 |
| 壳 | 无加壳；`.rdata` 含加密常量池（运行时解密） |

## 2. 破解验证（动态线）

破解包 `jnic370.zip` 的机制（`patcher/autopatch.py`，frida 脚本）：
- `GetCommandLineW` hook：把命令行中 `-agentpath/-javaagent/-agentlib` 伪造成
  `-xgentpath/-xavaagent/-xgentlib`——**在更底层欺骗 JNICLoader 的 agent 检测**
  （JNICLoader 读 `getInputArguments()`，见下）。
- `jcmd JVMTI.agent_load` 动态 attach 亦可（不残留命令行参数）。
- 等 JVM 创建后 hook `PushLocalFrame`，在特定返回地址（RVA 0x4547e2，工厂调用前的
  PushLocalFrame）触发 `hookFactory()`。
- **`Interceptor.replace` licence 工厂函数（RVA 0x4818d8 = 静态分析中的 `sub_1804818D8`）**：
  不再调用原工厂，直接构造 `dev/jnic/a3` 对象（三个 String：`Crack by AirFoundation.Tech` /
  `2099-12-31` / `external-runtime-test`）返回。a3 即 licence 数据类。
- NOP 掉 3 个 BK throw 点（RVA 0x17d9b / 0x17db0 / 0x18168）。
- hook `k` 类 `<clinit>`（RVA 0x121c7c），重建 `k.h/k.i` 两个翻译器集合（19 个类）。

验证通过输出：
```
INFO: Licence: Crack by AirFoundation.Tech (small developer)
INFO: Expires: 2099-12-31
```
**结论：licence 验证逻辑全部在引擎 DLL native 侧**（jar 字节级未被修改），
验证链 `be.main → a.a → bc.a → bc.a → PrintStream.println`（错误输出），
工厂 `sub_1804818D8` 是验证出口。

## 3. 3.7.0 生成源码深度分析（核心）

用破解版驱动 3.7.0 对 TEST.jar（31 类 50 方法）与 HelloWorld（1 类 3 方法）实际生成，
在 `Saving translated sources` → `Compiling sources` 间隙抢抓中间产物 `jnic.c`。
生成器同时输出 `jni.h`（31613 B）与 `crypto.h`（32989 B，Monocypher 库）。

### 3.1 文件结构（stringObf=true 版，29.7 KB / 438 行）

```c
/* Auto-generated JNIC implementation file */
#include <jni.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

const uint8_t *magic_constant = (uint8_t*) " jnic.dev v3.7.0";   // 版本标记
#include <crypto.h>                          // Monocypher（chacha20/sha512/hmac）
static uint8_t *jnic_buf;                    // 字符串解密密钥池（运行时生成）

static inline void* jnic_decrypt(char *a, size_t offset, const size_t len) {
    for(size_t i = 0; i < len; i++) a[i] ^= jnic_buf[offset + i]; return a;
}
```

内容顺序：全部类/成员缓存函数 → 全部方法体 `jnic_N` → 每类一个
`Java_<类>__00024jnicLoader` 注册函数 → `JNI_OnLoad`。

### 3.2 字符串加密三档（与 stringObf / 版本有关）

| 档位 | 形态 | 说明 |
|---|---|---|
| 3.5.1 stringObf=false | `NewString(env, (unsigned short[]) {80,65,83,83}, 4)` | UTF-16 code units 数值数组（"PASS"），防 strings 扫描，无密钥 |
| 3.7.0 stringObf=false | 同左（与 3.5.1 完全一致） | — |
| 3.7.0 stringObf=true | `NewString(env, jnic_decrypt((char[]) {-126,-84,...}, 1104, 17), 8)` | **chacha20 密钥流 XOR**，见 3.3 |

数字常量**不加密**（`stack2.i = 7;` 明文）。字符串常量（类名/方法名/描述符/
字面量）全部经 `jnic_decrypt`。

### 3.3 chacha20 字符串加密机制（3.7.0 stringObf=true）

- **密钥流生成**（`JNI_OnLoad`，jnic.c 尾部）：
  ```c
  JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
      JNIEnv *env; (*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6);
      jclass loader = (*env)->FindClass(env, "dev/jnic/qmwBAw/JNICLoader"); // 随机包名
      jfieldID fid = (*env)->GetStaticFieldID(env, loader, "z", "Ljava/nio/ByteBuffer;");
      jobject buf = (*env)->GetStaticObjectField(env, loader, fid);
      uint8_t *java_buf = (*env)->GetDirectBufferAddress(env, buf);  // Java 端 DirectByteBuffer
      uint8_t key[32] = {15, 28, 124, -112, ...};                    // 每次生成随机，硬编码
      struct chacha20_context c;
      chacha20_init_context(&c, key, java_buf + 32, 0);              // nonce = buffer 内容 +32
      jnic_buf = malloc(1877);                                       // 密钥池
      chacha20_write_stream(&c, jnic_buf, 1877);                     // 生成 1877 B 密钥流
      return JNI_VERSION_1_6;
  }
  ```
- **加密时机（生成器侧）**：生成器以同一 key+nonce 生成 1877 B 密钥流，与字符串
  UTF-16 字节逐字节 XOR → 写成 C 数组字面量 + 记录 offset。**offset 即字符串在
  密钥流中的位置**，相邻字符串的 offset 区间连续（如 0→46→88→144…→1024→1050→
  1877），`jnic_buf = malloc(1877)` 恰为最大 offset+len。
- **key 每次生成随机**（两次运行 key 数组不同），nonce 由 Java 端 `JNICLoader.z`
  （DirectByteBuffer，小端 64 B）提供（`java_buf+32` 起）。
- **安全性**：即使拿到 DLL + jnic.c + .dat 也无法离线解出字符串——key 只存在于
  生成时的 C 源码，nonce 在 Java 端运行时给出。

### 3.4 类/成员缓存（3.7.0，无锁）

与 3.5.1 的 `cached_c_N` 合并结构 + 自旋锁不同，3.7.0 拆分为**每成员独立函数**：

```c
__attribute__((const)) static jclass c_9(JNIEnv *env) {
    static _Atomic jclass clazz;
    jclass cached = atomic_load_explicit(&clazz, memory_order_relaxed);
    if (__builtin_expect(cached != 0, 1)) return cached;      // 快路径
    jclass loaded = (*env)->FindClass(env, jnic_decrypt(..., 1104, 17));
    loaded = (*env)->NewGlobalRef(env, loaded);
    if (atomic_compare_exchange_strong_explicit(&clazz, &cached, loaded,
            memory_order_relaxed, memory_order_relaxed)) return loaded;  // CAS 发布
    (*env)->DeleteGlobalRef(env, loaded);
    return cached;                                             // 竞争失败者用别人结果
}
__attribute__((const)) static jmethodID c_9id_0(JNIEnv *env) { /* 同模式，GetStaticMethodID */ }
```

- 类一个 `c_N` + 每个成员一个 `c_Nid_M`，各自独立 atomic。
- 方法/字段 ID 的缓存函数**不加密**自身名称（调用点加密，如 GetStaticMethodID 参数用
  `jnic_decrypt`）。

### 3.5 方法体翻译（栈机模拟，与 3.5.1 同模型）

```c
void JNICALL jnic_0(JNIEnv *env, jobject thisObj) {
    jvalue local0; memset(&local0, 0, sizeof(jvalue));   // 局部变量槽
    jvalue stack0; ...;                                  // 操作数栈槽（按需）
    jvalue temp0; ...;                                   // 临时槽（复用）
    local0.l = thisObj;                                  // 参数 → 槽
    stack0.l = (*env)->AllocObject(env, c_0(env));       // new
    if (__builtin_expect((*env)->ExceptionCheck(env), 0)) return;
    ...
    stack1.i = 5;                                        // 常量明文
    (*env)->CallVoidMethod(env, stack0.l, c_0id_1(env), stack1.i);
    if (__builtin_expect(...ExceptionCheck, 0)) return;  // 逐调用点检查
    if (stack0.i != stack1.i) goto L0;                   // 控制流 = 直译 goto
    if (1) goto L1;                                      // 无条件跳转固定写法
    L0:; ... L1:; return;
}
```

指令映射表（与 3.5.1 一致）：

| Java 字节码 | 生成 C |
|---|---|
| ldc 常量（数字） | `stackN.i/f/d/l = <字面量>`（明文） |
| ldc 字符串 | `NewString(env, jnic_decrypt(...), len)` / code units |
| new + dup | `AllocObject(env, c_N(env))` + `tempN.l = stackN.l` 备份 |
| invokespecial `<init>` | `CallNonvirtualVoidMethod(env, obj, c_N(env), c_Nid_M(env), ...)` |
| invokevirtual / interface | `Call*Method(env, obj, c_Nid_M(env), ...)` |
| invokestatic | `CallStatic*Method(env, c_N(env), c_Nid_M(env), ...)` |
| getfield / putfield | `Get*Field(env, obj, c_Nid_M(env))` / `Set*Field` |
| getstatic / putstatic | `GetStatic*Field(env, c_N(env), c_Nid_M(env))` / `SetStatic*Field` |
| iadd 等 | `stackN.i = (unsigned int) stackN.i + stackN.i`（加宽防溢出语义） |
| iinc | `localN.i += k` |
| 比较/分支 | `if (cond) goto LN`；`goto` → `if (1) goto LN` |
| fcmpg/fcmpl | `a < b ? -1 : a > b ? 1 : a == b ? 0 : -1` |
| checkcast / instanceof | `if (!IsInstanceOf(env, obj, c_N(env))) ThrowNew(env, c_M(env), "")` |
| athrow | `if (obj == NULL) ThrowNew(...,"") else (*env)->Throw(env, obj)`（3.7.0 部分简化） |
| 异常检查（每个 JNI 调用后） | `if (__builtin_expect(ExceptionCheck, 0)) return;` |

### 3.6 invokedynamic 翻译（StringConcatFactory 等）

javac 生成的字符串拼接 `invokedynamic` 在 3.7.0 中被展开为**运行时解析 bootstrap**：

```c
static jobject indy;                       // 每个 invokedynamic 一个 static 缓存
if (indy == NULL) {
    jobject args = NewObjectArray(env, 4, Object.class, NULL);
    SetObjectArrayElement(args, 0, *c_1l());                 // lookup
    SetObjectArrayElement(args, 1, NewString(env, <name>));  // 名字
    SetObjectArrayElement(args, 2, <MethodType 构造>);
    SetObjectArrayElement(args, 3, NewString(env, <recipe>));
    jobject callSite = CallObjectMethod(env,
        CallObjectMethod(env, *c_1l(), c_6id_0(env),          // lookup.findStatic(...)
            c_5(env), NewString(<name>), <MethodType>),
        c_7id_0(env), ...);                                    // ...
    indy = NewGlobalRef(env, callSite);                        // CallSite 缓存为全局
    DeleteLocalRef(env, callSite);
}
temp0.l = CallObjectMethod(env, indy, c_8id_0(env));          // CallSite.getTarget()
temp1.l = NewObjectArray(env, N, Object.class, NULL);          // 参数打包
temp1.l[0..] = 参数;                                           // Integer.valueOf 装箱等
stackN.l = CallObjectMethod(env, temp0.l, c_7id_0(env), temp1.l); // MethodHandle.invokeWithArguments
```

- bootstrap（StringConcatFactory.makeConcatWithConstants）在 **native 侧运行时调用**，
  结果 CallSite 缓存为 `static jobject indy`（进程生命周期一次）。
- 需要 `MethodHandles.lookup()` 全局缓存 `*c_Nl()`（注册函数里初始化）。

### 3.7 注册机制与类改写（Java 侧）

- **每类一个注册函数**（不是集中注册）：
  ```c
  JNIEXPORT void JNICALL Java_<类>__00024jnicLoader(JNIEnv *env, jclass clazz) {
      *c_1l() = NewGlobalRef(env, MethodHandles.lookup());    // 需要反射时
      JNINativeMethod table[] = {
          {"main", "([Ljava/lang/String;)V", (void *) &jnic_0},   // stringObf=true 时名称也经 jnic_decrypt
          {"bump", "()I", (void *) &jnic_1},
      };
      (*env)->RegisterNatives(env, clazz, table, N);
  }
  ```
- **Java 类改写**（翻译后 class）：
  - 目标方法改为 `public static native`（main 等保持 static/native）。
  - 新增 `public static native void $jnicLoader();`（注册入口）。
  - `<clinit>` 重写为 `JNICLoader.init(); $jnicLoader();`。
  - **常量折叠**：`static final String GREETING = "Hello..."` 的赋值被删除
    （字符串直接内联进 native 翻译，clinit 不再初始化该字段）。
- **引擎 Java 层**（生成的随机包 `dev/jnic/<6字符>/`）：
  - `JNICLoader`：extends `java.io.InputStream`，内嵌 **LZMA 解压器**（`e`/`w`/`h` 等
    私有类 = LZMA 实现），`static {}` 中分配 `z = ByteBuffer.allocateDirect(64).order(LITTLE_ENDIAN)`
    并写入 16 个 int（如 2039518132, -1888464803, ... = **chacha20 nonce 区**），
    检查 os.name/os.arch（win + x86_64/amd64），解压 `.dat` 到临时文件后
    `System.load()`。
  - 其他类（`a/z/s/h/...`）为解密/加载/翻译辅助类。

### 3.8 编译打包管线

```
Java 方法 → jnic.c + jni.h + crypto.h（生成器输出）
         → zig 编译（"Found compiler: .../zig.exe"，LLVM 后端）
         → jnic.jnilib（PE DLL，导出 JNI_OnLoad + Java_<类>__00024jnicLoader）
         + jnic.lib（import lib）
         → LZMA 压缩（"Compressing native libraries" preset 6，87 KiB → 37 KiB）
         → .dat 资源 + 随机包类 + 改写后的用户类 → 输出 jar
```

## 4. 3.5.1 vs 3.7.0 生成差异（同输入量化）

TEST.jar（31 类）同 config（stringObf=false，include `pack/.*`）：

| 项 | 3.5.1 | 3.7.0 | 说明 |
|---|---|---|---|
| 方法/类 | 50 / 31 | 50 / 31 | 完全一致 |
| ExceptionCheck | 553 | 553 | 完全一致（100% 逐点） |
| 自旋锁（atomic_flag） | 71 | 0 | 3.5.1 双检锁+自旋 |
| CAS（atomic_compare_exchange_strong） | 0 | 70 | 3.7.0 无锁缓存 |
| `__builtin_expect` | 0 | 759 | 分支预测提示 |
| 缓存形态 | `cached_c_N` 合并结构（一次初始化全部成员） | `c_N` + `c_Nid_M` 每成员独立函数 | 更细粒度，无锁 |
| NewString / code units | 114 / 113 | 114 / 113 | 相同 |
| RegisterNatives | 31 | 31 | 每类一次 |
| invokedynamic 缓存 | 3 | 3 | 相同 |
| ThrowNew/Throw | 16 | 10 | 3.7.0 简化 athrow NULL 分支 |
| 字符串加密 | code units（无密钥） | code units；**stringObf=true 时 chacha20 XOR** | 见 3.2/3.3 |

**核心结论**：3.7.0 与 3.5.1 的**生成模型完全同源**（栈机模拟、控制流直译 goto、
逐点异常检查、每类注册、indy 缓存均相同）。3.7.0 的结构性更新仅三点：
① 无锁 CAS 类缓存（去掉自旋锁）；② 分支预测提示（`__builtin_expect`）；
③ stringObf=true 升级为 chacha20 密钥流加密（key 每次随机 + Java 端 nonce）。

## 5. 对 DLL 静态结论的更正（重要）

| 旧结论（仅静态） | 更正后 |
|---|---|
| RegisterNatives 全 DLL 仅 1 次（集中注册） | **每类一次**（31 类）；94 个 loader 导出 = 94 个类注册函数；静态统计到的 0x6B0 仅 1 处是 JNI_OnLoad 相关辅助调用 |
| ExceptionCheck 仅 37.3% 调用点检查 | 生成源码 **100% 逐点检查**；DLL 统计偏差源于引擎自身代码（licence/加载）混入 |
| 字符串常量池化（NewString 仅 32 次） | 字符串为 **code units / chacha20 加密常量**，NewString 数量反映编译后合并 |
| 类缓存 = 全局槽 + 惰性初始化（无锁） | 生成源码为**无锁 CAS + `__builtin_expect`**（比"全局槽"更完整：成员独立缓存） |
| 方法体控制流扁平化 + MBA（sub_180001021 等） | **生成源码无扁平化**（flowObf=false 时直译 goto）；DLL 中的扁平化/MBA 属于**引擎自身代码**（licence 验证、注册处理），不是生成方法体 |
| "反 agent"靠 Java 层 `getInputArguments()` | 底层还有 **`GetCommandLineW` 欺骗**（破解包手段，见第 2 节） |
| JNI 表偏移 0x720（ExceptionCheck 非标准 0x718） | 生成源码用标准 `(*env)->ExceptionCheck`；DLL 偏移差异待定（可能引擎自研 jni.h 或测量误差） |

## 6. 结论

3.7.0 相对 3.5.1 的核心演进（修正版）：**生成模型不变**（栈机直译 + 每类注册 +
逐点异常检查 + indy 缓存），性能/结构更新为无锁 CAS 缓存与分支预测提示；
**安全更新**为 stringObf=true 的 chacha20 密钥流字符串加密（key 每次生成随机、
nonce 由 Java 端 DirectByteBuffer 提供、密钥池恰为全部加密字符串所需长度）。
licence 验证全在引擎 native（工厂 `sub_1804818D8` 为验证出口），破解 = 替换工厂
返回值，jar 无需修改。

## 7. 附录：复现命令

```powershell
# 跑破解版（frida spawn，JNIC_JAVA 覆盖 JDK 路径；RUN_ARGS 传编译参数）
$env:JNIC_JAVA = "C:\Users\Canrad\.jdks\corretto-17.0.5\bin\java.exe"
$env:JNIC_JAR = "...\jnic370_crack\jnic370.jar"
$env:PYTHONPATH = "...\patcher\pydeps64"
$env:RUN_ARGS = "<in.jar> <out.jar> <config.xml>"
& python "...\patcher\autopatch.py"

# 抢抓 jnic.c（编译窗口 <1s）：catch_c.ps1 轮询 Temp/jnic*/**/jnic.c 复制走
# 或直接重跑后从 Temp/jnic*/WINDOWS_X86_64/ 找回（部分运行会保留）
```

分析脚本（静态线）位于 `C:\Users\Canrad\AppData\Local\Temp\opencode\ida_out\`。

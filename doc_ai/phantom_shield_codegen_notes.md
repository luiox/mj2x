# 幻影盾（PhantomShield）C 代码生成深度分析

> 本文档分析"幻影盾"混淆器（**native-obfuscator 魔改商业变体**）生成的 C 源码与
> 编译产物。样本为 SnakeGame 全量 native（含捆绑的 Skidonio 验证 SDK）。
> **基座机制见 `native_obfuscator_codegen_notes.md`（原版生成器逐模块分析）**，
> 本文只展开变体的差异与商业层。
>
> 素材：`D:\QQTent\幻影盾的c代码生成-原始样本SnakeGame.zip`
> （已解压到 `C:\Users\Canrad\AppData\Local\Temp\opencode\hys_snake\15899889444821520342\`）

## 1. 样本总览

```
15899889444821520342/
├── native_jvm_output.cpp     1.66 MB   主输出（38 个类的翻译）
├── native_jvm_inline.cpp    768 KB    inline 层（被提取方法 + 授权逻辑 + Ed25519）
├── native_jvm.cpp            11 KB     框架（utils）
├── native_jvm.hpp / output.hpp / inline.hpp
├── string_pool.cpp           88 KB     字符串池（19511 B 明文 ASCII 数值数组）
├── precomp_data.h            97 KB     Ed25519 ge_precomp Bi[8] 基表
├── ed25519.h / fe.h / ge.h / sc.h / sha512.h / fixedint.h   Ed25519 签名实现
├── jni.h / jni_md.h          （JNI 头）
├── ThemidaSDK.h + SecureEngineCustomVMs_*.h     Themida VM 保护 SDK（编译期可选加壳）
└── build/
    ├── native_jvm.lib
    └── PhantomShieldX64.dll  2.1 MB    编译产物
```

**输入类集合**（38 类全量翻译）：`org/example/Main`（入口）+ 37 个
`skidonion/nrgui/<单字母>` 类（a-z/A-Z 中取 37 个）——**类名已被重命名混淆**。
内容含 SnakeGame 逻辑（JFrame/KeyAdapter/MouseAdapter/Random）+ **Skidonio 验证 SDK**
（登录面板/云验证/QQ 协议采集）。

## 2. 架构：native-obfuscator 基座 + 幻影盾魔改

生成框架与开源 native-obfuscator 同源（`native_jvm::utils`、`__ngen_` 命名、
`register_for_class` 隐藏类回调、`reg_methods[]` 集中注册），魔改点：

| 层 | 开源 native-obfuscator | 幻影盾 |
|---|---|---|
| 命名 | `__ngen_native_<方法名>` | `__ngen_native_<字母><序号>`（方法名混淆）+ `__phantom_shield_x_N`（授权方法） |
| 授权 | 无 | **Ed25519 token 验签 + 云验证 + 心跳**（见 §5） |
| 字符串 | `string_pool` 明文池 | 同（明文 ASCII 数值数组，防 strings） |
| 加壳 | 无 | **ThemidaSDK.h / SecureEngineCustomVMs**（编译期 VM 化可选） |
| 类名 | 保留 | `skidonion/nrgui/<单字母>` 重命名 |
| 目标库 | native_jvm.lib | PhantomShieldX64.dll |

## 3. 翻译模型（每类一个 namespace）

```cpp
// native_jvm_output.cpp
namespace native_jvm::classes::__ngen_skidonion_nrgui_f_1 {

    char *string_pool;                     // 全局字符串池指针
    jstring cstrings[4];                   // classloader 相关 jstring 缓存
    std::mutex cclasses_mtx[2];            // 类缓存互斥锁（比 JNIC 的 CAS/自旋更重）
    jclass cclasses[2];
    // 两种类缓存：_CacheClass0（classloader 查找）/ _CacheClass1（FindClass）
    bool _CacheClass0(JNIEnv *env, jobject classloader, int class_index, int class_name_index){...}
    bool _CacheClass1(JNIEnv *env, int class_index, long long offset){...}
    jmethodID cmethods[2];  jfieldID cfields[1];
    bool _CacheMethod / _CacheStaticMethod / _CacheField / _CacheStaticField(env, class_id, member_index, name_offset, desc_offset)
    // 方法体（栈机模拟）
    void JNICALL __ngen_native_main1(JNIEnv *env, jclass clazz, jarray arg0) {
        static jobject classloader = utils::get_classloader_from_class(env, clazz);   // 缓存 classloader
        jvalue cstack0 = {}, cstack1 = {};
        jvalue clocal0 = {};
        std::unordered_set<jobject> refs;                                            // jobject 引用跟踪集
        clocal0.l = arg0; refs.insert(clocal0.l);
        L1: if (env->ExceptionCheck()) { return; }
        if (_CacheClass0(env, classloader, 0, 0)) if (env->ExceptionCheck()) return;
        if (_CacheStaticField(env, 0, 0, 48, 52)) if (env->ExceptionCheck()) return;
        cstack0.l = env->GetStaticObjectField(cclasses[0], cfields[0]); refs.insert(cstack0.l);
        ...
        L2: ... return;
        L3: ... return;                 // 异常路径/多出口
    }
    JNINativeMethod __ngen_methods[] = {
        { ((char *)(string_pool + 0LL)), ((char *)(string_pool + 5LL)), (void *)&__ngen_native_main1 },
        ...
    };
    void __ngen_register_methods(...) { env->RegisterNatives(clazz, __ngen_methods, N); }
}
```

要点：
- **局部变量/栈槽**：`jvalue cstackN / clocalN`（与 JNIC 的 `stackN/localN` 同思路，无 temp 槽——dup 用 `cstack1 = cstack0` 整槽拷贝）。
- **引用管理**：`std::unordered_set<jobject> refs` 跟踪所有 jobject（native-obfuscator 风格），方法退出 `refs.erase + utils::clear_refs` 清理；**这是 JNIC 没有的**（JNIC 用临时局部引用不跟踪）。
- **类缓存带 mutex**（`cclasses_mtx`，双检锁），JNIC 3.7.0 用无锁 CAS。
- **方法 ID 缓存**：`cmethods[]` 扁平数组 + `_CacheMethod(env, class_id, method_index, name_offset, desc_offset)`——名称/描述符来自字符串池偏移。
- **异常检查**：每个 JNI 调用后 `if (env->ExceptionCheck()) return;`（100% 逐点，与 JNIC 同）。
- **空指针保护**：调用前 `if (obj == nullptr) utils::throw_re(...) else env->Call*Method(...)`（native-obfuscator 的 `throw_re` + 行号）。
- **异常处理**：`goto L_CATCH_0`（catch 块——ExceptionOccurred/ExceptionClear + 异常对象入栈）。
- **byte 语义**：`(jint)(jbyte)x` 显式截断；`>> (0x1f & n)` 位移掩码。
- **数组**：`utils::bastore/baload`（boolean/byte 二义数组统一）；`NewObjectArray + SetObjectArrayElement`；多维数组走 `utils::create_multidim_array`。
- **invokedynamic**：`LambdaMetafactory.metafactory` + `findVirtual/findStatic/findSpecial` + `MethodHandle.invokeWithArguments`（Lambda 支持；JNIC 走 StringConcatFactory 运行时 bootstrap 同思路）。

## 4. 字符串池（string_pool）

```cpp
namespace native_jvm::string_pool {
    static char pool[19511LL] = { 109, 97, 105, 110, 0, 40, 91, 76, ... };  // "main\0([Ljava/lang/String;)V\0..."
    char *get_pool() { return pool; }
}
```

- **单一连续池**：全部字符串（类名/方法名/描述符/字面量/异常消息/URL）按序拼接成
  19511 B 数值数组（ASCII 明文，NUL 分隔）。
- 访问方式：`((char *)(string_pool + offset))`，offset 编译期常量（`0LL/5LL/48LL/52LL`…）。
- 混淆强度：**仅防 strings 扫描**（无加密/无 chacha20）。对比：JNIC stringObf=false
  为 UTF-16 code units 数组，JNIC stringObf=true 为 chacha20 XOR。

## 5. 授权体系（幻影盾商业层，全 native）

### 5.1 本地 token：Ed25519 验签

inline 层内联实现（`__phantom_shield_x_*` 方法中），流程：

```
DirectByteBuffer（token 文件）
├── buffer[0..128]   编码公钥（32×u32 LE）
│       └─ MBA 混淆解码：temp += 0x6178da53; temp -= 0x64b8de28; ~temp; ror/rol; ^ 0xf69ca60e; ...
│           → 32 字节 Ed25519 公钥
├── buffer[128..192]  64 字节 Ed25519 签名
├── buffer[192..196]  消息长度（LE u32）
└── buffer[196..]     消息

cstack0.i = ed25519_verify(signature, &buffer[128 + 64 + 4], message_len, public_key);
if (cstack0.i != 0) goto L8;   // 验签失败 → 错误路径（-3 等）
```

- Ed25519 完整实现内嵌（fe/ge/sc/sha512/precomp_data.h，ref10 系）。
- 公钥经 **MBA 混淆**（加法/取反/循环移位/XOR 链）隐藏，运行时还原。

### 5.2 授权状态与云验证

`native_jvm_inline.hpp` 全局授权状态（extern）：

```cpp
extern jobject __phantom_shield_x_PORT13;          // 云端端口（"https://localhost.ptlogin2.qq.com:430" 等）
extern jobject __phantom_shield_x_PT_LOCAL_TOKEN14; // 本地 token
extern jobject __phantom_shield_x_HEADERS15;        // HTTP 头（sec-ch-ua/User-Agent/Cookie…）
extern jobject __phantom_shield_x_RANDOM17;
extern jobject __phantom_shield_x_CLOUD_CONSTANT_MAP18;  // 云端常量
extern jobject __phantom_shield_x_EXPIRED_DATE19;        // 过期日期
extern jarray  __phantom_shield_x_NONCE20;
extern jobject __phantom_shield_x_CRYPTO21;
extern jobject __phantom_shield_x_VERIFY_TOKEN22;        // 验证 token
extern jarray  __phantom_shield_x_KEY23;
extern jobject __phantom_shield_x_USERNAME24;
extern jlong   __phantom_shield_x_USER_ID25;
extern jarray  __phantom_shield_x_MAGIC_KEY26;
extern jobject __phantom_shield_x_NICKNAME27;
extern bool* licenced;              // 授权标志
extern jbyte** __buffer;            // token 缓冲区
```

- **云验证**：`https://skidonion.tech/` + `api/v2/verify/login` / `api/v2/verify/heartbeat`
  （心跳定时器 `scheduleAtFixedRate`）+ QQ 协议采集（`ptlogin2.qq.com`、`pt_local_tk`、
  `UserDataInfo.ini`、`nt_qq`）。HTTP 走 `HttpURLConnection` + 加密签名（MD5/Base64/
  chacha20 `"expand 32-byte k"`）。
- **时间校验**：`std::chrono::system_clock::now()` 毫秒时间戳对比 `EXPIRED_DATE19`
  （1970-1-1T00:00:00 哨兵）。
- 授权判定 `licenced` 全局指针，`VERIFY_TOKEN` 等对象方法退出时写入。

### 5.3 inline 层分工

- `native_jvm::inlines` namespace：39 个 `__phantom_shield_x_N` 函数（签名带
  `jclass clazz` = 静态方法被提取）+ 授权全局状态。
- 调用点：output.cpp 方法体中 `inlines::__phantom_shield_x_16(env, (jclass) cstack0.l)`
  形式调用（inline 展开在编译期，源码层仍是函数）。
- 提取原因：授权相关静态方法被单独抽离（便于加 VM 保护标记/集中管控）。

## 6. 注册与装载

```cpp
// prepare_lib（JNI_OnLoad → prepare_lib）
utils::init_utils(env);                          // 框架全局缓存
string_pool = string_pool::get_pool();
reg_methods[0..37] = &(native_jvm::classes::__ngen_*::__ngen_register_methods);  // 38 类集中注册
env->DefineClass("skidonion/nrgui/___", nullptr,                                  // 注入隐藏类
    native_jvm::data::__ngen_data_skidonion_nrgui_________::get_class_data(), ...);  // 内嵌 class data
inlines::init(env);                              // 授权初始化
env->RegisterNatives(FindClass("skidonion/nrgui/___"), {{"___", "(ILjava/lang/Class;)V", &register_for_class}}, 1);
```

- **集中注册**：`reg_methods[]` 函数指针数组（38 类）；JNIC 为每类 `jnicLoader` 导出。
- **隐藏类注入**：`DefineClass` 内嵌 class data（`__ngen_data_` 字节数组）。
- JNI_OnLoad 返回 JNI_VERSION_1_8。

## 7. 与 JNIC 3.7.0 对比

| 维度 | JNIC 3.7.0 | 幻影盾（PhantomShield） |
|---|---|---|
| 基座 | 自研生成器 | native-obfuscator 魔改 |
| 栈机模拟 | `local/stack/temp` jvalue 槽 | `clocal/cstack` jvalue + `refs` 引用集 |
| 字符串 | chacha20 XOR（stringObf=true）/ UTF-16 code units | 明文 ASCII 池 + offset |
| 类缓存 | 无锁 CAS + `__builtin_expect` | mutex 双检锁 |
| 成员缓存 | 每成员独立函数 `c_Nid_M` | `cmethods[]/cfields[]` 扁平数组 + `_Cache*` |
| 异常检查 | `__builtin_expect(ExceptionCheck,0)` 100% 逐点 | `if(ExceptionCheck)` 100% 逐点 + null 保护 throw_re |
| 注册 | 每类一个 `Java_<类>__00024jnicLoader` 导出 | `reg_methods[]` 集中注册 + 隐藏类 |
| invokedynamic | StringConcatFactory（bootstrap 运行时解析+缓存） | LambdaMetafactory + invokeWithArguments |
| 授权 | licence 工厂 + 云端（离线验证失败） | **Ed25519 token 验签 + 云登录/心跳 + QQ 采集** |
| 加壳 | 无 | ThemidaSDK/VM 可选 |
| 命名 | `jnic_N` + 随机包 `dev/jnic/<6字符>` | `__ngen_native_<字母>N` + `skidonion/nrgui/<单字母>` |
| 引用管理 | 无（直接局部引用） | `std::unordered_set<jobject>` 跟踪清理 |
| 强度排序 | 字符串加密最强 | 授权体系最强（签名+云+VM） |

## 8. 结论

幻影盾 = **native-obfuscator 直译引擎 + 商业授权层**：
1. 翻译模型沿用开源（栈机模拟/逐点异常/集中注册/隐藏类/字符串池），加类名与方法名
   重命名混淆、`refs` 引用管理。
2. 授权层全 native：**Ed25519 签名 token**（公钥 MBA 混淆内嵌）+ **云验证**（登录/
   心跳/过期）+ ChaCha20 数据加密 + QQ 协议采集，授权状态为全局 `licenced` 指针。
3. 可选 Themida VM 加壳（SDK 头文件随生成代码一起编译）。

对比 JNIC：JNIC 胜在**字符串加密**（chacha20 + 随机 key + Java 端 nonce）与**无锁缓存**
（CAS）；幻影盾胜在**授权完整度**（签名验签+云心跳）与**生态**（Themida VM）。
两者生成模型同源（JVM 栈机 → C 直译），是理解商用 JNI 混淆器的两个代表样本。

# 幻影盾（PhantomShield）授权验证机制 + 代码生成逆向情报

> 样本：SnakeGame 全量 native 输出（生成器为 native-obfuscator 的魔改商业变体
> "幻影盾"，附带 Skidonio 验证 SDK）。生成器翻译机制见
> `native_obfuscator_codegen_notes.md`；本文记录对**破解/绕过**有直接价值的
> 精确情报（格式、算法、偏移、patch 点），以及生成代码的结构指纹。
> 依据文件：`native_jvm_output.cpp`（1.66MB，37 类全部翻译方法）、
> `native_jvm_inline.cpp`（768KB，授权 inline 层）、`string_pool.cpp`（19511B）。

## 1. 授权体系构成

```
本地 token 验证（Ed25519 验签，native 内联，两处独立验签点）
  + 云验证（HTTP 登录/心跳 + 过期日期）
  + 数据加密（ChaCha20 / MD5 / Base64）
  + 授权状态全局变量（licenced / VERIFY_TOKEN22 等，patch 目标）
```

**授权入口（关键）**：`Main.__ngen_register_methods`（output.cpp 开头，类索引 0 的
注册函数）内嵌硬 gate：

```c
VM_TIGER_WHITE_START
if (!*inlines::licenced) {
    jclass auth_class = FindClass("skidonion/nrgui/C");        // 授权类（重命名后）
    jmethodID show_verification_id = GetStaticMethodID(auth_class, "a", "()I");
    *inlines::licenced = CallStaticIntMethod(auth_class, show_verification_id);  // 写全局
    if (!*inlines::licenced) exit(0);                          // 硬退出！
}
```

即：**`licenced` 的写入点不是"验证方法内"，而是首个翻译类（Main）的
`__ngen_register_methods` 注册函数开头**（output.cpp L103-111，由 clinit →
Loader 注册链触发）。`licenced` = `C.a()` 的返回值（int→bool），为 0 直接
`exit(0)`。

## 2. 本地 token：Ed25519 验签（可复现）

### 2.1 token 缓冲区精确格式

token 由 Java 侧组装为 **DirectByteBuffer**（`ByteBuffer.allocateDirect(196+len)`），
native 经 `env->GetDirectBufferAddress` 直读（inline.cpp 共 4 处
GetDirectBufferAddress：x_29、x_28 等）。布局：

```
offset 0    128 B   编码公钥（32 × u32 LE，非原始公钥！）
offset 128   64 B   Ed25519 签名
offset 192    4 B   消息长度 len（LE u32）
offset 196   len B  消息（被签名内容）
```

### 2.2 公钥解码（MBA 混淆链，逐 32 位元素）

buffer[0..128] 每个 u32 依次经以下变换还原为 32 B 公钥（per-byte `& 0xff`）。
**已与生成代码逐行核对一致**（inline.cpp 两处验签点均有同一条链）：

```
temp += 0x6178da53
temp -= 0x64b8de28
temp = ~temp
temp = ror(temp, 1)      // >> 0x1f | << 0x1
temp = ror(temp, 2)
temp = ror(temp, 6)
temp ^= 0xf69ca60e
temp -= 0xb92b354a
temp = ror(temp, 2)
temp ^= 0xbde9e614
temp += 0xadfb4505
temp = rol(temp, 7)      // << 0x19 | >> 0x7
temp = ~temp; temp = ~temp
temp -= 0x8899a58b
temp = ror(temp, 2)
```

（ror/rol 均为 32 位循环移位；写入公钥时取低字节。）

### 2.3 验签调用（注意返回语义：1=验签通过）

**本生成器的 `ed25519_verify` 采用 ref10 风格：返回 `1` = 签名有效，
`0` = 无效**（非 OpenSSL 的 0=success！）。实现返回 `0` 的分支：
`signature[63] & 224`、`ge_frombytes_negate_vartime != 0`、`!consttime_equal(...)`；
其余情况 `return 1`。

调用处：

```c
// signature = &buffer[128]
// message_len = LE u32 @ buffer[192]
// ed25519_verify(signature, &buffer[196], message_len, public_key)
cstack0.i = (jint) ed25519_verify(...);
if (cstack0.i != 0) goto L8;      // == 1 → 验签通过 → L8（成功/续跑）
cstack0.i = (jint) -3;            // == 0 → 验签失败 → 抛错误
```

即 **`ed25519_verify(...) != 0 → goto L8（成功路径）`**；`== 0`（验签失败）才走
错误路径。**绕过方向：应强制返回 1，或把分支改写成无条件 `goto L8`。**

### 2.4 失败行为（两处独立验签点，错误码不同）

**验签点 A：`__phantom_shield_x_29`（verify/requestInformation 类，返回 jobject）**

```c
cstack0.i = (jint) -3;
Byte.valueOf(-3)      // _CacheStaticMethod(37,53,14279='valueOf', 14846='(B)Ljava/lang/Byte;')
Optional.of(...)      // _CacheStaticMethod(38,54,14866='of', 14869='(Ljava/lang/Object;)Ljava/util/Optional;')
return Optional<Byte>(-3)   // 实际是 Optional.of(Byte.valueOf((byte)-3))
```

即抛 **-3** 是 `Optional.of(Byte.valueOf(-3))`（`Optional<Byte>`），**不是
`Integer.valueOf(-3)`**。成功后进入 L8 初始化授权全局对象（MAGIC_KEY26 等，见 §3）。

**验签点 B：`__phantom_shield_x_28`（login 类，返回 jint）**

```c
if (cstack0.i != 0) goto L8;      // 通过 → L8
cstack0.i = (jint) -2;
// 打包：(clocal3.i & 0xFFFF00FF) | ((-2 & 0xff) << 8)
return 打包后的 int;               // 失败码 -2 装入 bits 8-15
```

即 login 路径验签失败返回 **-2**（打包进 bits 8-15，外层 `>>8 & 0xff` 提取）。
两处验签点共享同一条 MBA 公钥解码链，且都被 `VM_TIGER_WHITE_START/END` 包裹。

### 2.5 验签点所在函数（不是"唯一"）

| 函数 | 签名 | 错误码 | 语义 |
|---|---|---|---|
| `__phantom_shield_x_29` | `(JNIEnv*, jclass)` → jobject | **-3**（Optional\<Byte\>） | token/verify 流程 |
| `__phantom_shield_x_28` | `(JNIEnv*, jclass, jobject, jobject, jboolean)` → jint | **-2**（bits 8-15） | login 流程（= `C.a()I` show_verification_id 的底层） |

`C.a()I`（show_verification_id）注册为 `__ngen_native_a1`（C 类，类索引 24），
其函数体内直接 `inlines::__phantom_shield_x_28(...)`。

## 3. 授权全局状态（patch 目标）

inline 层暴露（extern 全局，定义在 inline.cpp 底部）：

| 符号 | 类型 | 意义 |
|---|---|---|
| `licenced` | bool* | **授权标志**（绕过首选目标；写入点 = Main.register_methods，见 §1） |
| `__buffer` | jbyte** | 哑标记：`new jbyte *[0]`（0 长度指针数组），非真实缓冲区 |
| `VERIFY_TOKEN22` | jobject | 验证 token 对象（每次调用比对，null 视为未授权） |
| `PT_LOCAL_TOKEN14` | jobject | 本地 token 对象 |
| `MAGIC_KEY26` / `KEY23` | jarray | 密钥材料 |
| `NONCE20` | jarray | nonce |
| `EXPIRED_DATE19` | jobject | 过期日期（`1970-1-1T00:00:00` 为哨兵） |
| `PORT13` / `HEADERS15` | jobject | 云端地址 / HTTP 头 |
| `CLOUD_CONSTANT_MAP18` | jobject | 云端常量表 |
| `CRYPTO21` | jobject | 加密器对象 |
| `RANDOM17` | jobject | SecureRandom |
| `USERNAME24` / `USER_ID25` / `NICKNAME27` | jobject/jlong | 用户信息 |

调用点特征：`cstackN.l = (jobject) inlines::__phantom_shield_x_VERIFY_TOKEN22;`，
对象在验证方法内 `NewGlobalRef` 赋值（验证成功才非 null）。`CRYPTO21`/`PORT13`/
`PT_LOCAL_TOKEN14`/`HEADERS15` 还被翻译类 t 的方法体直接引用（见 §6 注入点）。

## 4. 云验证（可拦截）

- 端点：`api/v2/verify/login`、`api/v2/verify/heartbeat`，主域 `skidonion.tech`；
  另有 QQ 协议采集（`ptlogin2.qq.com`、`pt_local_tk`、`UserDataInfo.ini`、
  `https://xui.ptlogin2.qq.com/cgi-bin/xlogin?...`）。
- HTTP 自实现：`HttpURLConnection` + 自定义头（`sec-ch-ua`/`User-Agent`/Cookie）+
  签名（MD5/Base64）。
- **心跳**：`ScheduledExecutorService.scheduleAtFixedRate` 定时上报；
  过期校验用 `std::chrono::system_clock::now()` 毫秒时间戳对比 `EXPIRED_DATE19`。
- 本地校验通过后仍会联网（心跳），断网/假服务器行为需实测（心跳失败是否吊销授权）。

## 5. 数据加密

- `"expand 32-byte k"` 常量 = **ChaCha20**（生成代码内联实现，与 token 数据加密相关）。
- `MessageDigest`（MD5）+ `SecureRandom` + 自实现 Base64（含 `1 input bytes left`
  警告字符串，池偏移 13976-14005）。

## 6. 绕过路径（按可行性，已按真实语义修正）

1. **patch `licenced`**：全局 bool*，**写入点在 `Main.__ngen_register_methods`**
   （output.cpp L103-111，首个类注册时），值为 `C.a()I` 返回值；置 1 后跳过整个
   授权调用。注意该块被 `VM_TIGER_WHITE` 包裹且失败会 `exit(0)`。
2. **patch 验签结果**：`ed25519_verify` 返回值**强制 1**（或把
   `if (cstack0.i != 0) goto L8` 改成无条件 goto），跳过 -2/-3 错误分支。
   **注意：强制 0 是反的**（本生成器 1=通过）。风险：成功路径后仍有云心跳、
   过期日期、VERIFY_TOKEN 比对等多处二次校验（x_28 成功路径还会继续做
   `getBytes→byte[]==48` 等附加校验，见 L8631）。
3. **patch `VERIFY_TOKEN22` 对象**：方法内比对处直接跳过 null 检查/篡改比对结果。
4. **本地假服务器**：`PORT13/HEADERS15` 指向自建端点，回放 login/heartbeat 协议
   （需先抓包还原协议；心跳响应决定是否持续授权）。
5. **伪造 token 不可行**：Ed25519 验签需要私钥，只能 patch 运行时。

## 7. 生成侧行为差异（vs 原版真实产物）

| 行为 | 原版（snake 7 类产物） | 幻影盾（snake 37 类产物） |
|---|---|---|
| 文件组织 | 每类独立 `output/<类>_<N>.cpp/.hpp` | **合并**：单文件 `native_jvm_output.cpp`（1.66MB）+ `native_jvm_inline.cpp`（768KB，新增） |
| 类命名 | 保留原类名（`Main`、`Window`…） | **类名重命名** `skidonion/nrgui/<单字母>`（36 类 + Main = 37），注册/缓存数组下标按类索引 |
| 方法命名 | `__ngen_native_<原方法名><N>`（可读） | `__ngen_native_<字母><N>`（`a1,b2,c3…` 按类序分配）+ `__ngen_native_main1`/`aaaaa2`（入口）；**源方法名只在字符串池残留** |
| 缓存代码 | 调用点内联双检锁展开（每处方法调用一段） | 封装为 `_CacheClass0`（classloader 加载，参数 class_name_index 查 cstrings）/ `_CacheClass1`（FindClass + 池偏移）/ `_CacheMethod/_CacheStaticMethod/_CacheField/_CacheStaticField`（参数传池偏移）——**缓存逻辑函数化**，产物更小 |
| 方法体结构 | 无 static 类缓存字段 | 增加 `static jobject classloader`（方法级 static 缓存 classloader）、`jobject lookup` 同原版 |
| 隐藏类 | `native<N>/hidden/Hidden0`（仅 clinit 代理等） | 同机制（`skidonion/nrgui/___` + `__ngen_data_` DefineClass，1502B，44 个 `___N_NNN` 方法），且授权方法被提取为 **inline 层函数** `__phantom_shield_x_N`（**25 个**，编号 0..38 带空洞，带 jclass 参数 = static 方法）+ 授权全局状态（`licenced`/`VERIFY_TOKEN22`/`__buffer` 等 extern） |
| 授权注入 | 无 | 方法体内穿插授权调用：`VERIFY_TOKEN22` 比对（null 跳转）、`PT_LOCAL_TOKEN14` 赋值、`std::chrono` 时间戳、Ed25519 验签内联段（§2，**两处**）、云端 API 调用（§4）；翻译类 t 的方法体直接读 `CRYPTO21`/`PORT13`/`HEADERS15`/`VERIFY_TOKEN22` |
| 注册入口 | `native<N>/Loader` + `registerNativesForClass` | `skidonion/nrgui/___` + `___`（类名/方法名双混淆） |
| 字符串池 | 明文 ASCII，可还原全部符号 | 同格式（19511B），但**方法名已混淆**（`a,b,c…`），描述符/异常消息/URL 保留可读（`api/v2/verify/login`、`https://skidonion.tech/` 直接可见） |
| 额外 SDK | 无 | 内嵌 **Ed25519 全套**（fe/ge/sc/sha512/precomp_data + ed25519_key_exchange/create_keypair）+ **ThemidaSDK.h** + **SecureEngineCustomVMs**（VM_TIGER 标记） |
| 类改写 | 方法 ACC_NATIVE + clinit 钩子 | 同，另把授权相关静态字段（`KEY23`/`NONCE20`/`EXPIRED_DATE19`…）在验证方法内 NewGlobalRef 缓存为全局 |

## 8. 代码生成结构指纹（新增）

1. **VM_TIGER_WHITE 标记**：生成代码被 `VM_TIGER_WHITE_START/END` 包裹
   （output.cpp 78 处、inline.cpp 14 处）——配合 ThemidaSDK/SecureEngineCustomVMs
   的编译期 VM 化。所有 register_methods、授权 inline 函数、授权注入点都在
   VM 标记内。
2. **不透明谓词**：inline 函数体统一套 `volatile bool __dummy = true;
   if(__dummy){ ... }`（inline.cpp 6 处），保持 VM 标记代码不被编译器消除。
3. **inline 层是超大全局缓存表**：`cstrings[191]`、`cclasses[82]`、`cmethods[174]`、
   `cfields[8]`、`ccallsites[3]`、`cinits[4]`；`init(JNIEnv*)` 在 prepare_lib
   阶段一次性 NewStringUTF→intern→NewGlobalRef 预热 191 个字符串。
4. **`__buffer` 是哑指针**：`jbyte** __buffer = new jbyte *[0]`（0 长度），仅作
   标记，不是真实 token 缓冲区；真实 token 走 Java DirectByteBuffer +
   `GetDirectBufferAddress`。
5. **错误码打包**：login 失败码 -2 写入返回 int 的 bits 8-15
   （`(val & 0xFFFF00FF) | ((-2 & 0xff) << 8)`），外层 `>>8 & 0xff` 提取——这是
   native-obfuscator 原版没有的授权专用返回协议。

**逆向启示**：
1. 幻影盾的字符串池仍是明文池——**转储后描述符+异常消息可还原 90% 调用语义**；
2. inline 层是授权逻辑集中地（`__phantom_shield_x_*`）——patch 点优先扫 inline.cpp 符号；
3. 缓存函数 `_CacheClass*` 是幻影盾的**结构指纹**（原版无此封装）；
4. 授权验证有**两处** `ed25519_verify` 调用点（x_28 login / x_29 verify），
   返回 1=通过；`licenced` 写入点在 Main.register_methods（首类注册时）。

# 幻影盾（PhantomShield）授权验证机制逆向情报

> 样本：SnakeGame 全量 native 输出（生成器为 native-obfuscator 的魔改商业变体
> "幻影盾"，附带 Skidonio 验证 SDK）。生成器翻译机制见
> `native_obfuscator_codegen_notes.md`；本文只记录对**破解/绕过**有直接价值的
> 精确情报（格式、算法、偏移、patch 点），不做泛泛描述。

## 1. 授权体系构成

```
本地 token 验证（Ed25519 验签，native 内联）
  + 云验证（HTTP 登录/心跳 + 过期日期）
  + 数据加密（ChaCha20 / MD5 / Base64）
  + 授权状态全局变量（patch 目标）
```

## 2. 本地 token：Ed25519 验签（可复现）

### 2.1 token 文件精确格式

```
offset 0    128 B   编码公钥（32 × u32 LE，非原始公钥！）
offset 128   64 B   Ed25519 签名
offset 192    4 B   消息长度 len（LE u32）
offset 196   len B  消息（被签名内容）
```

### 2.2 公钥解码（MBA 混淆链，逐 32 位元素）

buffer[0..128] 每个 u32 依次经以下变换还原为 32 B 公钥（per-byte `& 0xff`）：

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

### 2.3 验签调用

```c
// signature = &buffer[128]
// message_len = LE u32 @ buffer[192]
// ed25519_verify(signature, &buffer[196], message_len, public_key)
if (ed25519_verify(...) != 0) → 失败路径
```

- Ed25519 实现为 ref10 系（fe/ge/sc/sha512/precomp_data.h），内嵌于生成代码。
- **验签返回值是唯一裁决点**：`ed25519_verify(...) != 0 → goto L8（失败）`。

### 2.4 失败行为

- 验签失败走错误路径并抛 **-3** 错误（`CallStaticObjectMethod(Integer, valueOf(-3))` 包装）。
- 成功后初始化授权全局对象（见 §3）。

## 3. 授权全局状态（patch 目标）

生成代码的 inline 层暴露（extern 全局）：

| 符号 | 类型 | 意义 |
|---|---|---|
| `licenced` | bool* | **授权标志**（绕过首选目标） |
| `__buffer` | jbyte** | token 原始缓冲区 |
| `VERIFY_TOKEN22` | jobject | 验证 token 对象（每次调用比对，null 视为未授权） |
| `PT_LOCAL_TOKEN14` | jobject | 本地 token 对象 |
| `MAGIC_KEY26` / `KEY23` | jarray | 密钥材料 |
| `NONCE20` | jarray | nonce |
| `EXPIRED_DATE19` | jobject | 过期日期（`1970-1-1T00:00:00` 为哨兵） |
| `PORT13` / `HEADERS15` | jobject | 云端地址 / HTTP 头 |
| `CLOUD_CONSTANT_MAP18` | jobject | 云端常量表 |
| `CRYPTO21` | jobject | 加密器对象 |
| `USERNAME24` / `USER_ID25` / `NICKNAME27` | jobject/jlong | 用户信息 |

调用点特征：`cstackN.l = (jobject) inlines::__phantom_shield_x_VERIFY_TOKEN22;`，
对象在验证方法内 `NewGlobalRef` 赋值（验证成功才非 null）。

## 4. 云验证（可拦截）

- 端点：`api/v2/verify/login`、`api/v2/verify/heartbeat`，主域 `skidonion.tech`；
  另有 QQ 协议采集（`ptlogin2.qq.com`、`pt_local_tk`、`UserDataInfo.ini`）。
- HTTP 自实现：`HttpURLConnection` + 自定义头（`sec-ch-ua`/`User-Agent`/Cookie）+ 
  签名（MD5/Base64）。
- **心跳**：`ScheduledExecutorService.scheduleAtFixedRate` 定时上报；
  过期校验用 `std::chrono::system_clock::now()` 毫秒时间戳对比 `EXPIRED_DATE19`。
- 本地校验通过后仍会联网（心跳），断网/假服务器行为需实测（心跳失败是否吊销授权）。

## 5. 数据加密

- `"expand 32-byte k"` 常量 = **ChaCha20**（生成代码内联实现，与 token 数据加密相关）。
- `MessageDigest`（MD5）+ `SecureRandom` + 自实现 Base64（含 `1 input bytes left`
  警告字符串）。

## 6. 绕过路径（按可行性）

1. **patch `licenced`**：验证成功后置 1 的全局标志，启动后 hook/置值即绕过后续检查
   （需定位写入点——验证方法内）。
2. **patch 验签结果**：`ed25519_verify` 返回值强制 0（拦截该调用，跳过错分支）。
   风险：验证成功后仍有云心跳、过期日期、VERIFY_TOKEN 比对等多处二次校验。
3. **patch `VERIFY_TOKEN22` 对象**：方法内比对处直接跳过 null 检查/篡改比对结果。
4. **本地假服务器**：`PORT13/HEADERS15` 指向自建端点，回放 login/heartbeat 协议
   （需先抓包还原协议；心跳响应决定是否持续授权）。
5. **伪造 token 不可行**：Ed25519 验签需要私钥，只能 patch 运行时。

## 7. 生成侧行为差异（vs 原版真实产物）

| 行为 | 原版（snake 7 类产物） | 幻影盾（snake 38 类产物） |
|---|---|---|
| 文件组织 | 每类独立 `output/<类>_<N>.cpp/.hpp` | **合并**：单文件 `native_jvm_output.cpp`（1.66MB）+ `native_jvm_inline.cpp`（768KB，新增） |
| 类命名 | 保留原类名（`Main`、`Window`…） | **类名重命名** `skidonion/nrgui/<单字母>`（37 类），注册/缓存数组下标按类索引 |
| 方法命名 | `__ngen_native_<原方法名><N>`（可读） | `__ngen_native_<字母><N>`（`a1,b2,c3…` 按类序分配）+ `__ngen_native_main1`/`aaaaa2`（入口）；**源方法名只在字符串池残留** |
| 缓存代码 | 调用点内联双检锁展开（每处方法调用一段） | 封装为 `_CacheClass0`（classloader 加载，参数 class_name_index 查 cstrings）/ `_CacheClass1`（FindClass + 池偏移）/ `_CacheMethod/_CacheStaticMethod/_CacheField/_CacheStaticField`（参数传池偏移）——**缓存逻辑函数化**，产物更小 |
| 方法体结构 | 无 static 类缓存字段 | 增加 `static jobject classloader`（方法级 static 缓存 classloader）、`jobject lookup` 同原版 |
| 隐藏类 | `native<N>/hidden/Hidden0`（仅 clinit 代理等） | 同机制（`skidonion/nrgui/___` + `__ngen_data_` DefineClass），且授权方法被提取为 **inline 层函数** `__phantom_shield_x_N`（39 个，带 jclass 参数 = static 方法）+ 授权全局状态（`licenced`/`VERIFY_TOKEN22`/`__buffer` 等 extern） |
| 授权注入 | 无 | 方法体内穿插授权调用：`VERIFY_TOKEN22` 比对（null 跳转）、`PT_LOCAL_TOKEN14` 赋值、`std::chrono` 时间戳、Ed25519 验签内联段（§2）、云端 API 调用（§4） |
| 注册入口 | `native<N>/Loader` + `registerNativesForClass` | `skidonion/nrgui/___` + `___`（类名/方法名双混淆） |
| 字符串池 | 明文 ASCII，可还原全部符号 | 同格式，但**方法名已混淆**（`a,b,c…`），描述符/异常消息/URL 保留可读（`api/v2/verify/login`、`https://skidonion.tech/` 直接可见） |
| 额外 SDK | 无 | 内嵌 **Ed25519 全套**（fe/ge/sc/sha512/precomp_data）+ **ThemidaSDK.h**（编译期 VM 化可选） |
| 类改写 | 方法 ACC_NATIVE + clinit 钩子 | 同，另把授权相关静态字段（`KEY23`/`NONCE20`/`EXPIRED_DATE19`…）在验证方法内 NewGlobalRef 缓存为全局 |

**逆向启示**：
1. 幻影盾的字符串池仍是明文池——**转储后描述符+异常消息可还原 90% 调用语义**；
2. inline 层是授权逻辑集中地（`__phantom_shield_x_*`）——patch 点优先扫 inline.cpp 符号；
3. 缓存函数 `_CacheClass*` 是幻影盾的**结构指纹**（原版无此封装）；
4. 授权验证方法 = `ed25519_verify` 调用点所在函数（唯一裁决，见 §2.3）。

# 原生运行时保护

在编译后的原生代码中嵌入反调试、反篡改和加密机制，保护运行时安全。

## 问题

JVM 字节码编译为原生代码后，攻击者从字节码层面难以分析，但可以从进程层面入手：

- 调试器附着（`CreateRemoteThread` + `DebugActiveProcess`）
- JVMTI agent 注入（通过 `jvm.dll` 的导出函数）
- 内存 dump（读取进程内存中的代码和数据）
- API 监控（挂钩 JNI 函数或系统调用）

运行时保护的目的是提高这层分析的门槛。

## 反调试

### Windows 调试器检测

| 方法 | 实现 | 优缺点 |
|---|---|---|
| `IsDebuggerPresent` | 调用 `kernel32!IsDebuggerPresent` | 最简单，但容易被绕过 |
| `NtQueryInformationProcess` | 查询 `ProcessDebugPort` / `ProcessDebugFlags` / `ProcessDebugObjectHandle` | 比 `IsDebuggerPresent` 更底层 |
| `PEB.BeingDebugged` | 直接读取 PEB 结构体偏移（`PEB + 0x2`） | 避免 API 调用，但 PEB 偏移因 Windows 版本变异 |
| `NtGlobalFlag` | 读取 `PEB + 0x68`（`NtGlobalFlag` 字段） | 调试器启动的进程会有特定标志位 |

```c
bool checkDebugger() {
    // PEB 直接读取（避免 API 被 hook）
#ifdef _WIN64
    Peb* peb = (Peb*)__readgsqword(0x60);
#else
    Peb* peb = (Peb*)__readfsdword(0x30);
#endif
    return peb->BeingDebugged != 0;
}
```

### 硬件断点检测

调试器可以通过 DR0-DR3 寄存器设置执行访问断点。检查这些寄存器的值：

```c
bool checkHardwareBreakpoints() {
    CONTEXT ctx = { .ContextFlags = CONTEXT_DEBUG_REGISTERS };
    GetThreadContext(GetCurrentThread(), &ctx);
    return ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0;
}
```

### 时间差检测

调试器单步执行会导致代码段执行时间异常增长：

```c
bool checkTiming() {
    uint64_t start = __rdtsc();  // RDTSC 指令
    // 插入一段固定计算
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) sum += i;
    uint64_t end = __rdtsc();

    return (end - start) > THRESHOLD;  // 远超过阈值 → 可能正在被调试
}
```

RDTSC 在多核 CPU 上的值可能不一致，更好的方案是用 `QueryPerformanceCounter`。

## 反 JVMTI

JVMTI agent 依赖 `jvm.dll` 的导出函数来注册回调。可以通过检查 `jvm.dll` 中关键导出函数的首字节是否为原始值来检测 hook：

```c
bool checkJvmtiHook() {
    HMODULE jvm = GetModuleHandleA("jvm.dll");
    if (!jvm) return false;

    // 检查 JVMTI 相关导出函数的入口字节
    FARPROC func = GetProcAddress(jvm, "JVM_DefineClass");

    // 0xE9 = JMP（MinHook/Detours 常用）
    // 0xCC = INT3（VEH hook）
    if (*(uint8_t*)func == 0xE9 || *(uint8_t*)func == 0xCC) {
        return true;  // 被 hook
    }
    return false;
}
```

也可以枚举已加载的模块，检查是否有已知的 JVMTI agent DLL：

```c
bool checkAgentModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SMMODULE, 0);
    // 遍历模块列表，检查是否有已知 JVMTI agent 名字
    // 或检查是否有除 jvm.dll 以外的 DLL 也导出了 Agent_OnLoad
}
```

## 加密原语

### 常量时间比较

避免基于时序的侧信道攻击：

```c
// 非常量时间：只要有一个字节不匹配就返回
int insecureCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;  // 提前返回 → 时序泄露
    }
    return 1;
}

// 常量时间：始终比较所有字节
int constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    int result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result == 0;
}
```

### AES-128 CTR

计数器模式（CTR）的 AES：

```c
void aesCtrEncrypt(const uint8_t* key, const uint8_t* nonce,
                   const uint8_t* input, uint8_t* output, size_t len) {
    uint8_t counter[16];
    memcpy(counter, nonce, 12);
    uint32_t blockCount = 0;

    for (size_t i = 0; i < len; i += 16) {
        // 计数器的后 4 字节递增
        counter[12] = (blockCount >> 24) & 0xFF;
        counter[13] = (blockCount >> 16) & 0xFF;
        counter[14] = (blockCount >> 8) & 0xFF;
        counter[15] = blockCount & 0xFF;

        uint8_t keystream[16];
        aesEncryptBlock(key, counter, keystream);

        // XOR 密钥流
        for (int j = 0; j < 16 && (i + j) < len; j++) {
            output[i + j] = input[i + j] ^ keystream[j];
        }
        blockCount++;
    }
}
```

CTR 模式不需要补丁，输入和输出长度一致。适合加密运行时字符串池或配置文件。

### HMAC-SHA256

消息认证码确保数据的完整性：

```c
void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* data, size_t dataLen,
                uint8_t* outMac /* 32 字节 */) {
    uint8_t k_ipad[64];  // inner padding
    uint8_t k_opad[64];  // outer padding

    // 如果密钥长度 > 64，先哈希
    uint8_t keyHash[32];
    uint8_t actualKey[64] = {0};
    if (keyLen > 64) {
        sha256(key, keyLen, keyHash);
        memcpy(actualKey, keyHash, 32);
    } else {
        memcpy(actualKey, key, keyLen);
    }

    // ipad = key ^ 0x36, opad = key ^ 0x5c
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = actualKey[i] ^ 0x36;
        k_opad[i] = actualKey[i] ^ 0x5c;
    }

    // HMAC = SHA256(opad || SHA256(ipad || data))
    uint8_t inner[128];
    memcpy(inner, k_ipad, 64);
    memcpy(inner + 64, data, dataLen);
    sha256(inner, 64 + dataLen, outMac);

    uint8_t outer[96];
    memcpy(outer, k_opad, 64);
    memcpy(outer + 64, outMac, 32);
    sha256(outer, 96, outMac);
}
```

## 反篡改

### 校验和

在编译时计算关键代码段的哈希值，运行时验证：

```c
// 编译时预计算：SHA256(code_section) = expected_hash
// 运行时：
void integrityCheck() {
    uint8_t actual[32];
    sha256((uint8_t*)&__code_start, &__code_end - &__code_start, actual);

    if (!constantTimeCompare(actual, expected_hash, 32)) {
        // 代码被篡改
        abort();
    }
}
```

代码段的起止地址需要在链接脚本中定义符号。

## 检测响应策略

| 检测结果 | 响应 |
|---|---|
| 检测到调试器 | 正常运行（打草惊蛇），或触发错误路径 |
| 检测到 hook | 记录日志后退出，或跳过敏感操作 |
| 时间差异常 | 正常行为，继续执行（避免时序验证成为 oracle） |
| 校验和不匹配 | 立即退出或崩溃 |

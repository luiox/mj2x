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

### Linux 调试器检测

Linux 下没有与 Windows `IsDebuggerPresent` 等价的单 API；常见信号来自 procfs 与 ptrace：

| 方法 | 实现 | 优缺点 |
|---|---|---|
| `TracerPid` | 读 `/proc/self/status`，解析 `TracerPid:` 字段非零 | 最直接；调试器 attach 后必非零（gdb/strace 都算） |
| `ptrace(PTRACE_TRACEME)` | 自检：成功 = 未被跟踪；`EPERM` = 已被跟踪或 seccomp 限制 | 需要区分"被跟踪"与"沙箱禁用 ptrace"（后者 TracerPid 为零 → 探测不可用而非被跟踪） |
| 可写可执行段 | 解析 `/proc/self/maps`，perms 含 `w`+`x`（如 `rwxp`） | 注入器/shellcode 特征；正常共享库为 r-xp/rw-p 分离 |
| 时间差 | `clock_gettime(CLOCK_MONOTONIC)` 两段测量 | 弱信号：调度/节能易误报，只报告不进聚合 |

```c
// TracerPid 解析（纯文本谓词，可 mock 测试；注意按行解析避免误匹配相邻字段）
bool hasTracerPid(const char* statusText) {
    while (*statusText) {
        if (strncmp(statusText, "TracerPid:", 10) == 0) {
            const char* v = statusText + 10;
            while (*v == ' ' || *v == '\t' || *v == '\r') v++;
            return atol(v) != 0;
        }
        while (*statusText && *statusText != '\n') statusText++;
        if (*statusText == '\n') statusText++;
    }
    return false;
}
```

## 反 JVMTI / 反 Java agent

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

### Java agent 检测（javaagent / JVMTI agent / JDWP）

Java 侧 agent 按加载方式分两类，检测信号也分两类：

| 加载方式 | 信号 | 检测手段 |
|---|---|---|
| 启动型（`-javaagent:` / `-agentpath:` / `-agentlib:` / `-Xrunjdwp:`） | 命令行参数 | 扫描进程命令行：Windows `GetCommandLineW`（引号感知切分），Linux `/proc/self/cmdline`（`\0` 分隔） |
| 动态 attach（jcmd/jattach 注入） | 已加载 native 模块 + JVMTI capability | 模块枚举（Windows Toolhelp / Linux `/proc/self/maps`）匹配已知 agent 工具名；`JavaVM::GetEnv(JVMTI)` + `GetCapabilities` 检查 dump 相关位（can_get_bytecodes / can_retransform_classes / can_generate_all_class_hook_events） |
| IDE 调试 | JDWP 参数 | `jdwp` / `transport=dt_socket` 子串（含 `-agentlib:jdwp=` 与旧式 `-Xrunjdwp:`） |

```c
// 命令行扫描（启动型 agent 的强信号；引号内空格不切分）
bool containsAgentOption(const char** args, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!strncmp(args[i], "-javaagent:", 11) ||
            !strncmp(args[i], "-agentpath:", 11) ||
            !strncmp(args[i], "-agentlib:", 10))
            return true;
    }
    return false;
}
```

JNI 函数表本身也是 hook 锚点：启动期快照 `env->functions` 地址，后续调用前比对
是否仍指向原始表（agent 原地改写表项或整体换表都会暴露）。注意 capability 查询
只能看到自己的 JVMTI 环境，跨 agent 检测要靠模块枚举互补。

### 检测响应策略：多阶段检查点

检测命中后的动作按策略分层，避免"检测即退出"成为可预测 oracle：

| 动作 | 语义 | 适用 |
|---|---|---|
| 只记录 | 计数/日志，行为不变 | 观察期、误报容忍 |
| 延迟 | 命中后睡眠固定时长 | 拖慢人工分析，不破坏正常路径 |
| 假数据 | 返回类型默认值/降级结果 | 高价值方法，让攻击者拿到"看似正常"的假结果 |
| 终止 | 退出进程 | 最激进；命中点可预测 |

检查点本身也应多阶段：每个检查点独立计数，命中概率随调用次数上升（确定性下限
+ 伪随机提前），攻击者无法靠"精确等到第 N 次调用"定位或规避。

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

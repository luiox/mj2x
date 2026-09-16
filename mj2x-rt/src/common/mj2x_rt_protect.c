/// @file mj2x_rt_protect.c
/// @brief mj2x_rt_protect 实现（C99，Windows/Linux）。
///
/// 探测目录含 mj2x-runtime C++ antidebug 层的移植版（R3 整合，见
/// doc_ai/r3_protect_integration.md）：PEB BeingDebugged/NtGlobalFlag、
/// NtQueryInformationProcess 调试三元组、硬件断点调试寄存器、Linux agent
/// 库映射扫描。所有探测 fail-open：探测失败按无信号处理。
// nanosleep 与 struct timespec 需要 _GNU_SOURCE（严格 c99 下 glibc 默认隐藏），
// 必须在任何系统头（含 mj2x_rt_protect.h 引入的 <stdint.h>）之前定义。
#ifdef __linux__
#    define _GNU_SOURCE
#endif

#include "mj2x_rt_protect.h"

#include <string.h>
#include <time.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <wchar.h>
#    if defined(_MSC_VER)
#        include <intrin.h>
#    endif
#else
#    include <stdio.h>
#    include <stdlib.h>
// POSIX 下 _exit 声明在 <unistd.h>（Windows 侧走 <stdlib.h> 的 _exit）；
// 缺声明时 clang 以 -Wimplicit-function-declaration 直接报错。
#    include <unistd.h>
#endif

/// @brief xorshift32 伪随机（自包含、确定性；seed 由检查点状态持有）。
static uint32_t mj2x_rt_xorshift32(uint32_t* state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// ---- 纯解码器（导出供单测；与 mj2x-runtime/antidebug 移植源逐位一致） ----

/// NtGlobalFlag 调试堆标志位（FLG_HEAP_ENABLE_TAIL_CHECK|FREE_CHECK|PARAM_CHECK）。
#define MJ2X_RT_NT_GLOBAL_FLAG_DEBUG_HEAP_MASK 0x70u

int mj2x_rt_nt_global_flag_suspicious(uint32_t nt_global_flag)
{
    return (nt_global_flag & MJ2X_RT_NT_GLOBAL_FLAG_DEBUG_HEAP_MASK) != 0;
}

int mj2x_rt_hardware_breakpoint_enabled(uintptr_t dr0, uintptr_t dr1, uintptr_t dr2, uintptr_t dr3,
                                        uintptr_t dr7)
{
    const int any_address = (dr0 | dr1 | dr2 | dr3) != 0;
    // Dr7 低 8 位 = L0/G0..L3/G3 局部/全局使能位；任一使能且任一地址非零即命中。
    const int any_enabled_slot = (dr7 & 0xFFu) != 0;
    return any_address && any_enabled_slot;
}

#ifdef _WIN32

static int mj2x_rt_scan_command_line(void)
{
    const wchar_t* cmd = GetCommandLineW();
    if (cmd == NULL) return 0;
    static const wchar_t* const kTokens[] = {
        L"-javaagent:", L"-agentpath:", L"-agentlib:", L"jdwp",
    };
    for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        if (wcsstr(cmd, kTokens[i]) != NULL) return 1;
    }
    return 0;
}

/// PEB BeingDebugged + NtGlobalFlag（x64: gs:[0x60]/+0xBC，x86: fs:[0x30]/+0x68）。
static int mj2x_rt_peb_debugged(void)
{
#    if defined(_MSC_VER) && defined(_M_X64)
    const unsigned char* peb = (const unsigned char*)__readgsqword(0x60);
    if (peb == NULL) return 0;
    if (peb[2] != 0) return 1;   // PEB.BeingDebugged
    uint32_t nt_global_flag = 0;
    memcpy(&nt_global_flag, peb + 0xBC, sizeof(nt_global_flag));
    return mj2x_rt_nt_global_flag_suspicious(nt_global_flag);
#    elif defined(_MSC_VER) && defined(_M_IX86)
    const unsigned char* peb = (const unsigned char*)__readfsdword(0x30);
    if (peb == NULL) return 0;
    if (peb[2] != 0) return 1;
    uint32_t nt_global_flag = 0;
    memcpy(&nt_global_flag, peb + 0x68, sizeof(nt_global_flag));
    return mj2x_rt_nt_global_flag_suspicious(nt_global_flag);
#    else
    return 0;   // 非 MSVC x86/x64：探测不可用（fail-open）。
#    endif
}

/// NtQueryInformationProcess 调试三元组：DebugPort(7)/DebugObjectHandle(30)/
/// DebugFlags(31)。动态加载 ntdll 导出，失败按无信号处理。
static int mj2x_rt_ntquery_debugged(void)
{
    typedef LONG(__stdcall * NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) return 0;
    NtQueryInformationProcessFn query = (NtQueryInformationProcessFn)(void*)GetProcAddress(
        ntdll, "NtQueryInformationProcess");
    if (query == NULL) return 0;

    ULONG_PTR debug_port          = 0;
    const LONG port_status        = query(GetCurrentProcess(), 7, &debug_port,
                                            sizeof(debug_port), NULL);
    if (port_status >= 0 && debug_port != 0) return 1;

    HANDLE     debug_object       = NULL;
    const LONG object_status       = query(GetCurrentProcess(), 30, &debug_object,
                                            sizeof(debug_object), NULL);
    // STATUS_PORT_NOT_SET(0xC0000353) 是"未被调试"的正常路径。
    if (object_status >= 0 && debug_object != NULL) {
        CloseHandle(debug_object);
        return 1;
    }

    ULONG      no_debug_inherit   = 1;
    const LONG flags_status        = query(GetCurrentProcess(), 31, &no_debug_inherit,
                                            sizeof(no_debug_inherit), NULL);
    if (flags_status >= 0 && no_debug_inherit == 0) return 1;

    return 0;
}

/// 硬件断点：当前线程 Dr0-3 地址 + Dr7 使能位（CONTEXT_DEBUG_REGISTERS）。
static int mj2x_rt_hardware_breakpoint_active(void)
{
#    if defined(_M_X64) || defined(_M_IX86)
    CONTEXT context;
    ZeroMemory(&context, sizeof(context));
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &context) == FALSE) return 0;
    return mj2x_rt_hardware_breakpoint_enabled((uintptr_t)context.Dr0, (uintptr_t)context.Dr1,
                                               (uintptr_t)context.Dr2, (uintptr_t)context.Dr3,
                                               (uintptr_t)context.Dr7);
#    else
    return 0;
#    endif
}

#else   // !_WIN32

static int mj2x_rt_proc_text_has(const char* path, const char* needle)
{
    FILE* file = fopen(path, "r");
    if (file == NULL) return 0;
    char buffer[4096];
    const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (read == 0) return 0;
    buffer[read] = '\0';
    return strstr(buffer, needle) != NULL;
}

static int mj2x_rt_tracer_pid_active(void)
{
    // TracerPid 非零 = 被 ptrace 跟踪（字段后跟 \t 与十进制数）。
    FILE* file = fopen("/proc/self/status", "r");
    if (file == NULL) return 0;
    char buffer[4096];
    const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (read == 0) return 0;
    buffer[read] = '\0';
    const char* hit = strstr(buffer, "TracerPid:");
    if (hit == NULL) return 0;
    hit += 10;
    while (*hit == ' ' || *hit == '\t' || *hit == '\r') ++hit;
    if (*hit < '0' || *hit > '9') return 0;
    unsigned long pid = 0;
    while (*hit >= '0' && *hit <= '9') {
        pid = pid * 10 + (unsigned long)(*hit - '0');
        ++hit;
    }
    return pid != 0;
}

static int mj2x_rt_maps_has_rwx(void)
{
    // /proc/self/maps 行 `start-end perms ...`：perms 位置 1='w'、2='x' 即 rwx。
    FILE* file = fopen("/proc/self/maps", "r");
    if (file == NULL) return 0;
    char  line[512];
    int   found = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        char* space = strchr(line, ' ');
        if (space != NULL && space[1] == 'w' && space[2] == 'x') {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

/// agent 库映射扫描：-agentpath/-agentlib 加载的 .so 出现在 maps 且路径含
/// "agent"。启动型 agent 已由 cmdline 覆盖；此探测补运行期 attach 注入。
static int mj2x_rt_maps_has_agent_library(void)
{
    FILE* file = fopen("/proc/self/maps", "r");
    if (file == NULL) return 0;
    char  line[1024];
    int   found = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        const char* dot_so = strstr(line, ".so");
        if (dot_so == NULL) continue;
        if (strstr(line, "agent") != NULL) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

#endif   // !_WIN32

static int mj2x_rt_detect_debugger_or_agent(void)
{
#ifdef _WIN32
    if (IsDebuggerPresent()) return 1;
    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) != FALSE && remote) return 1;
    if (mj2x_rt_peb_debugged()) return 1;
    if (mj2x_rt_ntquery_debugged()) return 1;
    if (mj2x_rt_hardware_breakpoint_active()) return 1;
    return mj2x_rt_scan_command_line();
#else
    // 注意：/proc/self/status 的 TracerPid 在子进程被跟踪时非零；maps rwx
    // 段是注入器/自修改特征；cmdline 覆盖启动型 agent 选项，maps 补运行期
    // attach 的 agent .so。
    if (mj2x_rt_tracer_pid_active()) return 1;
    if (mj2x_rt_maps_has_rwx()) return 1;
    if (mj2x_rt_maps_has_agent_library()) return 1;
    static const char* const kTokens[] = {
        "-javaagent:", "-agentpath:", "-agentlib:", "jdwp",
    };
    for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        if (mj2x_rt_proc_text_has("/proc/self/cmdline", kTokens[i])) return 1;
    }
    return 0;
#endif
}

int mj2x_rt_checkpoint_run(mj2x_rt_checkpoint* cp, mj2x_rt_protect_action action,
                           uint32_t step, uint32_t delay_ms, int force)
{
    if (cp == NULL) return 0;
    if (cp->seed == 0) {
        // 零初始化状态的懒初始化：地址（ASLR 下进程间不同）+ 时间（进程内
        // 不同检查点不同），保证每次运行的触发序列不同。
        cp->seed = (uint32_t)(uintptr_t)(void*)cp ^ (uint32_t)time(NULL);
        if (cp->seed == 0) cp->seed = 0x9E3779B9u;
    }

    const uint32_t detected = mj2x_rt_detect_debugger_or_agent() != 0;
    int            trigger  = 0;
    if (detected && force) {
        // 检测命中 + force：必然触发（推进计数，保持调用/命中统计一致）。
        ++cp->invocations;
        ++cp->triggers;
        trigger = 1;
    }
    else {
        // 无信号（或未 force）：多阶段概率推进，防检测窗口外 attach。
        ++cp->invocations;
        const uint32_t effectiveStep = step != 0 ? step : 16;
        trigger = (cp->invocations % effectiveStep) == 0 ||
                  (mj2x_rt_xorshift32(&cp->seed) % effectiveStep) == 0;
        if (trigger) ++cp->triggers;
    }
    if (!trigger) return 0;

    switch (action) {
    case MJ2X_RT_PROTECT_FAKE: return 1;
    case MJ2X_RT_PROTECT_TERMINATE:
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0xC0000409u);
#else
        _exit(1);
#endif
        return 1;
    case MJ2X_RT_PROTECT_DELAY:
#ifdef _WIN32
        Sleep(delay_ms);
#else
        {
            struct timespec ts;
            ts.tv_sec  = (time_t)(delay_ms / 1000u);
            ts.tv_nsec = (long)(delay_ms % 1000u) * 1000000L;
            nanosleep(&ts, NULL);
        }
#endif
        return 0;
    case MJ2X_RT_PROTECT_LOG:
    default: return 0;
    }
}

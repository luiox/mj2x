/// @file mj2x_rt_protect.c
/// @brief mj2x_rt_protect 实现（C99，Windows/Linux）。
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
#else
#    include <stdio.h>
#    include <stdlib.h>
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

#else

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

#endif   // !_WIN32

static int mj2x_rt_detect_debugger_or_agent(void)
{
#ifdef _WIN32
    if (IsDebuggerPresent()) return 1;
    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) != FALSE && remote) return 1;
    return mj2x_rt_scan_command_line();
#else
    // 注意：/proc/self/status 的 TracerPid 在子进程被跟踪时非零；maps rwx
    // 段是注入器/自修改特征；cmdline 覆盖启动型 agent 选项。
    if (mj2x_rt_tracer_pid_active()) return 1;
    if (mj2x_rt_maps_has_rwx()) return 1;
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

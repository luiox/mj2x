/// @file mj2x_regc_sys.c
/// @brief System/Objects 类 RegC intrinsic 实现（声明见 mj2x_regc_sys.h）。
///
/// @details
/// currentTimeMillis/nanoTime 是跨平台墙钟/单调时钟读取（CI 是 linux+windows
/// 双门，两分支都必须可编译）；requireNonNull 族经 context->throwException
/// 回调路由 NPE（与 runtime.c parse 族的异常路由同契约）。
#ifndef _WIN32
#    ifndef _POSIX_C_SOURCE
#        define _POSIX_C_SOURCE 200809L
#    endif
#endif

#include "regc/mj2x_regc_sys.h"

#include <stdint.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <sys/time.h>
#    include <time.h>
#endif

/* System.currentTimeMillis()：纯读取，不抛异常。
 * Windows FILETIME 是 1601-01-01 起 100ns 单位；1601→1970 的偏移
 * 11644473600s = 116444736000000000 × 100ns，除 10^4 落到毫秒。
 * POSIX gettimeofday：tv_sec*1000 + tv_usec/1000。 */
int64_t mj2x_intrinsic_system_current_time_millis(void)
{
#ifdef _WIN32
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    const uint64_t units = ((uint64_t)file_time.dwHighDateTime << 32) | file_time.dwLowDateTime;
    return (int64_t)((units - 116444736000000000ULL) / 10000ULL);
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
#endif
}

/* System.nanoTime()：单调时钟，仅保证相对间隔语义。
 * Windows QueryPerformanceCounter：秒/余数拆分换算，rem*1e9 不溢出 u64
 * （rem < freq，QPF 上界 ~3.3e9 → rem*1e9 < 3.3e18 < 1.8e19）；频率恒定，
 * 静态缓存首写竞态双方写入同值，覆写无害。
 * POSIX clock_gettime(CLOCK_MONOTONIC)：tv_sec*1e9 + tv_nsec（int64 在
 * ~292 年单调秒内不溢出）。 */
int64_t mj2x_intrinsic_system_nano_time(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency = {0};
    if (frequency.QuadPart == 0) {
        LARGE_INTEGER queried;
        if (QueryPerformanceFrequency(&queried) == 0 || queried.QuadPart <= 0) return 0;
        frequency.QuadPart = queried.QuadPart;
    }
    LARGE_INTEGER counter;
    if (QueryPerformanceCounter(&counter) == 0) return 0;
    const uint64_t ticks    = (uint64_t)counter.QuadPart;
    const uint64_t freq     = (uint64_t)frequency.QuadPart;
    const uint64_t seconds  = ticks / freq;
    const uint64_t remainder = ticks % freq;
    return (int64_t)(seconds * 1000000000ULL + remainder * 1000000000ULL / freq);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

/* NPE 路由：与 runtime.c parse 族同契约（context->throwException 回调，
 * ThrowNew 侧 message 为 NULL 时归一空串）。 */
static void mj2x_regc_sys_throw_npe(mj2x_regc_context* context, const char* message)
{
    if (context == NULL || context->throwException == NULL) return;
    context->throwException(context->env, "java/lang/NullPointerException", message);
}

/* Objects.requireNonNull(T)：JDK 17 对 null 抛无消息 NPE；非 null 透传。 */
mj2x_ref _mj2x_intrinsic_require_non_null(mj2x_regc_context* context, mj2x_ref value)
{
    if (value != NULL) return value;
    mj2x_regc_sys_throw_npe(context, NULL);
    return NULL;
}

/* Objects.requireNonNull(T, String)：JDK 17 对 null 抛 NPE(message)；
 * message 经 GetStringUTFChars 取 modified UTF-8（ThrowNew 输入口径），
 * 提取失败（OOM）时仍抛 NPE，消息退化为空串。 */
mj2x_ref _mj2x_intrinsic_require_non_null_message(mj2x_regc_context* context, mj2x_ref value,
                                                  mj2x_ref message)
{
    if (value != NULL) return value;
    JNIEnv*      env  = (JNIEnv*)mj2x_regc_env(context);
    const char*  text = NULL;
    if (message != NULL && env != NULL) {
        text = (*env)->GetStringUTFChars(env, (jstring)message, NULL);
    }
    mj2x_regc_sys_throw_npe(context, text != NULL ? text : "");
    if (text != NULL && env != NULL) (*env)->ReleaseStringUTFChars(env, (jstring)message, text);
    return NULL;
}

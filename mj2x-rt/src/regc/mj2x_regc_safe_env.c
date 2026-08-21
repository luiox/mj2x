/// @file mj2x_regc_safe_env.c
/// @brief safe-env JNI 函数表快照/校验/恢复实现（C99，随 mj2x-rt 分发）。
#include "regc/mj2x_regc_safe_env.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__linux__)
#    include <stdio.h>
#else
#    include <stdio.h>
#endif

/// 表按 8 字节（指针宽度）逐项原子恢复，避免并发 JNI 调用读到撕裂指针。
#if defined(_MSC_VER) && defined(_WIN64)
#    define MJ2X_SAFEENV_ATOMIC_STORE(dst, val) InterlockedExchange64((volatile LONG64*)(dst), (LONG64)(val))
#elif defined(__GNUC__) || defined(__clang__)
#    define MJ2X_SAFEENV_ATOMIC_STORE(dst, val) __atomic_store_n((void**)(dst), (void*)(val), __ATOMIC_RELEASE)
#else
#    define MJ2X_SAFEENV_ATOMIC_STORE(dst, val) (*(void**)(dst) = (void*)(val))
#endif

/// C 模式下 `JNIEnv` 的类型是 `const struct JNINativeInterface_*`（表指针本身），
/// 不含 `functions` 成员；真实运行对象是 `struct JNIEnv_`（首个成员即函数表
/// 指针）。通过该转换取出当前表指针。
#define MJ2X_SAFEENV_TABLE(env) \
    (((const struct JNIEnv_*)(const void*)(env))->functions)

static struct JNINativeInterface_ g_snapshot;
static int                 g_captured = 0;
static int                 g_snapshot_valid = 0;

#if defined(_WIN32)

static HMODULE g_jvm_module = NULL;

/// @brief Windows 默认归属校验：地址落在 jvm.dll 映像内（MEM_IMAGE 且
/// AllocationBase == jvm.dll 基址）。GetModuleHandleW 返回的模块一定是
/// 已加载的 jvm.dll；无法定位时保守放行（避免误杀，校验不可用视为可接受）。
static int mj2x_safeenv_default_module_check(const void* addr)
{
    if (addr == NULL) return 1;
    if (g_jvm_module == NULL) g_jvm_module = GetModuleHandleW(L"jvm.dll");
    if (g_jvm_module == NULL) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return 0;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
           mbi.AllocationBase == (void*)g_jvm_module;
}

#elif defined(__linux__)

/// @brief Linux 默认归属校验：地址落在 libjvm.so 的可执行映射区间内。
/// /proc/self/maps 解析失败（权限/容器）时保守放行。
static int mj2x_safeenv_default_module_check(const void* addr)
{
    if (addr == NULL) return 1;
    static int          checked = 0;
    static unsigned long long lo[16];
    static unsigned long long hi[16];
    static int            count = 0;
    if (!checked) {
        checked = 1;
        FILE* fp = fopen("/proc/self/maps", "r");
        if (fp != NULL) {
            char line[512];
            while (fgets(line, sizeof(line), fp) != NULL && count < 16) {
                unsigned long long a = 0, b = 0;
                char  perms[8] = {0};
                if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) == 3 &&
                    strchr(perms, 'x') != NULL && strstr(line, "libjvm.so") != NULL) {
                    lo[count] = a;
                    hi[count] = b;
                    count++;
                }
            }
            fclose(fp);
        }
    }
    const unsigned long long p = (unsigned long long)(uintptr_t)addr;
    for (int i = 0; i < count; i++) {
        if (p >= lo[i] && p < hi[i]) return 1;
    }
    return count > 0 ? 0 : 1;
}

#else

/// 非 Windows/Linux：无模块归属校验来源，保守放行（仅依赖恢复机制）。
static int mj2x_safeenv_default_module_check(const void* addr)
{
    (void)addr;
    return 1;
}

#endif

static mj2x_safeenv_module_check_fn g_module_check = mj2x_safeenv_default_module_check;

void mj2x_regc_safeenv_set_module_check(mj2x_safeenv_module_check_fn fn)
{
    g_module_check = fn != NULL ? fn : mj2x_safeenv_default_module_check;
}

/// 基本类型字段 getter 的 JNI 表索引（GetBooleanField..GetDoubleField，规范冻结）。
///
/// HotSpot 默认 `UseFastJNIAccessors`（x64 开）：这 8 项在 VM 启动时被替换为
/// JIT 生成的快速访问器 stub，位于 JVM 代码堆（MEM_PRIVATE + PAGE_EXECUTE），
/// 不在 jvm.dll 映像内。校验豁免它们，其余表项仍必须归属 jvm.dll。
#define MJ2X_SAFEENV_FAST_FIELD_GETTER_START 96
#define MJ2X_SAFEENV_FAST_FIELD_GETTER_END   104   // 不含 104

static int mj2x_safeenv_table_polluted(void)
{
    const void* const* entries = (const void* const*)&g_snapshot;
    const size_t       n       = sizeof(struct JNINativeInterface_) / sizeof(void*);
    for (size_t i = 0; i < n; i++) {
        if (entries[i] == NULL) continue;
        if (i >= MJ2X_SAFEENV_FAST_FIELD_GETTER_START && i < MJ2X_SAFEENV_FAST_FIELD_GETTER_END) {
            continue;
        }
        if (!g_module_check(entries[i])) return 1;
    }
    return 0;
}

mj2x_regc_status mj2x_regc_safeenv_capture(JNIEnv* env, int mode)
{
    const struct JNINativeInterface_* current;
    if (env == NULL) return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    current = MJ2X_SAFEENV_TABLE(env);
    if (current == NULL) return MJ2X_REGC_STATUS_INVALID_STATE;
    g_snapshot       = *current;
    g_snapshot_valid = !mj2x_safeenv_table_polluted();
    g_captured       = 1;
    if (!g_snapshot_valid) {
        if ((mode & MJ2X_SAFEENV_MODE_DEBUG) != 0) {
            fprintf(stderr,
                    "[mj2x-safe-env] WARNING: JNI function table appears hooked at capture; "
                    "snapshot polluted (debug mode, continuing)\n");
            return MJ2X_REGC_STATUS_OK;
        }
        return MJ2X_REGC_STATUS_INVALID_STATE;
    }
    return MJ2X_REGC_STATUS_OK;
}

void mj2x_regc_safeenv_restore(JNIEnv* env, int mode)
{
    const struct JNINativeInterface_* current;
    const void* const*         src;
    void* const*               dst;
    size_t                     n;
    size_t                     i;
    (void)mode;
    if (env == NULL) return;
    if ((mode & MJ2X_SAFEENV_MODE_RESTORE) == 0) return;
    if (!g_captured || !g_snapshot_valid) return;
    current = MJ2X_SAFEENV_TABLE(env);
    if (current == NULL) return;
    if (memcmp(current, &g_snapshot, sizeof(g_snapshot)) == 0) return;

    // 表内容被改写（指针替换或原地改写）：逐项 8 字节原子恢复。
    src = (const void* const*)&g_snapshot;
    dst = (void* const*)(struct JNINativeInterface_*)current;
    n   = sizeof(struct JNINativeInterface_) / sizeof(void*);
    for (i = 0; i < n; i++) {
        MJ2X_SAFEENV_ATOMIC_STORE(&dst[i], src[i]);
    }
    if ((mode & MJ2X_SAFEENV_MODE_DEBUG) != 0) {
        fprintf(stderr, "[mj2x-safe-env] restored JNI function table at entry\n");
    }
}

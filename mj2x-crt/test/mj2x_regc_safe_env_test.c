#include "regc/mj2x_regc_safe_env.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

// 纯逻辑层单测：不依赖真实 JVM。通过注入的模块归属校验模拟
// "表项在 jvm.dll 内 / 在 agent DLL 内" 两种归属，验证：
//   1. 干净快照捕获成功，且可在入口恢复被原地改写的表；
//   2. 捕获时表已污染（启动期 hook）→ 硬模式 fail / debug 模式继续；
//   3. 未捕获时 restore 为空操作。

static int g_test_mode = 0;   // 0=干净归属；1=污染归属（模拟 agent DLL 表项）

static int fake_module_check(const void* addr)
{
    if (addr == NULL) return 1;
    return g_test_mode == 0 ? 1 : 0;
}

static void test_capture_and_restore_in_place_patch(void)
{
    // 构造一个 fake "JNIEnv"：functions 指向一个可写表。
    struct JNINativeInterface_ clean;
    memset(&clean, 0, sizeof(clean));
    clean.FindClass        = (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1000;
    clean.GetMethodID      = (jmethodID(JNICALL*)(JNIEnv*, jclass, const char*, const char*))(uintptr_t)0x2000;
    clean.CallIntMethodA   = (jint(JNICALL*)(JNIEnv*, jobject, jmethodID, const jvalue*))(uintptr_t)0x3000;
    clean.CallStaticIntMethodA =
        (jint(JNICALL*)(JNIEnv*, jclass, jmethodID, const jvalue*))(uintptr_t)0x4000;
    clean.ExceptionCheck = (jboolean(JNICALL*)(JNIEnv*))(uintptr_t)0x5000;

    struct JNIEnv_ env;
    memset(&env, 0, sizeof(env));
    env.functions = &clean;

    // 干净快照。
    mj2x_regc_safeenv_set_module_check(fake_module_check);
    assert(mj2x_regc_safeenv_capture((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE) == MJ2X_REGC_STATUS_OK);

    // 原地改写表内容（指针不变，c2j 工具姿势）：hook FindClass + CallIntMethodA。
    clean.FindClass      = (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0xDEAD;
    clean.CallIntMethodA = (jint(JNICALL*)(JNIEnv*, jobject, jmethodID, const jvalue*))(uintptr_t)0xBEEF;

    // 入口恢复。
    mj2x_regc_safeenv_restore((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE);
    assert(clean.FindClass == (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1000);
    assert(clean.CallIntMethodA == (jint(JNICALL*)(JNIEnv*, jobject, jmethodID, const jvalue*))(uintptr_t)0x3000);
    assert(clean.CallStaticIntMethodA ==
           (jint(JNICALL*)(JNIEnv*, jclass, jmethodID, const jvalue*))(uintptr_t)0x4000);

    // 表干净时 restore 为空操作（不改变内容）。
    mj2x_regc_safeenv_restore((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE);
    assert(clean.FindClass == (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1000);
}

static void test_capture_polluted_hard_fails_debug_continues(void)
{
    struct JNINativeInterface_ hooked;
    memset(&hooked, 0, sizeof(hooked));
    hooked.FindClass = (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1000;
    struct JNIEnv_ env;
    memset(&env, 0, sizeof(env));
    env.functions = &hooked;

    // 启动期 agent 已在库加载前 hook：表项归属在 agent DLL → 污染。
    g_test_mode = 1;
    assert(mj2x_regc_safeenv_capture((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE) ==
           MJ2X_REGC_STATUS_INVALID_STATE);

    // 污染快照下 restore 是空操作（无干净来源可恢复）。
    mj2x_regc_safeenv_restore((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE);
    assert(hooked.FindClass == (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1000);

    // debug 模式：污染不 fail，返回 OK。
    assert(mj2x_regc_safeenv_capture((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE | MJ2X_SAFEENV_MODE_DEBUG) ==
           MJ2X_REGC_STATUS_OK);

    g_test_mode = 0;
}

static void test_restore_without_capture_is_noop(void)
{
    struct JNINativeInterface_ table;
    memset(&table, 0, sizeof(table));
    table.FindClass = (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1234;
    struct JNIEnv_ env;
    memset(&env, 0, sizeof(env));
    env.functions = &table;

    // 未捕获快照（新进程入口防御性调用）：空操作。
    mj2x_regc_safeenv_restore((JNIEnv*)&env, MJ2X_SAFEENV_MODE_RESTORE);
    assert(table.FindClass == (jclass(JNICALL*)(JNIEnv*, const char*))(uintptr_t)0x1234);
}

static void test_invalid_arguments(void)
{
    assert(mj2x_regc_safeenv_capture(NULL, MJ2X_SAFEENV_MODE_RESTORE) ==
           MJ2X_REGC_STATUS_INVALID_ARGUMENT);
    mj2x_regc_safeenv_restore(NULL, MJ2X_SAFEENV_MODE_RESTORE);   // 不崩溃即可
}

void test_safe_env(void)
{
    mj2x_regc_safeenv_set_module_check(fake_module_check);
    test_capture_and_restore_in_place_patch();
    test_capture_polluted_hard_fails_debug_continues();
    test_restore_without_capture_is_noop();
    test_invalid_arguments();
}

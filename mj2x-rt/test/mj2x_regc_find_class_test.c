#include "regc/mj2x_regc_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 由 mj2x_regc_crt_test.c 的 main 统一调用（单 runner 惯例）。
//
// _mj2x_regc_find_class 描述符守卫的 C 级直钉（#1182 评审收口 P1）：只剥完整
// `L...;` 形态描述符，裸内部名直通不改写——裸名可能以 'L' 开头（default 包
// 合成类 Lambda$N），按首字符无条件剥壳会截成 "ambda$N" → NoClassDefFoundError。
// 此前该守卫只有发射侧 E2E 间接覆盖，抓不住"传给 FindClass 的名字被剥错"的
// 中间态；此处注入记录型 fake FindClass 观察实际传入名，不依赖真实 JVM。

static struct JNINativeInterface_ g_find_class_table;
static JNIEnv                     g_find_class_env = &g_find_class_table;

static char g_find_class_last_name[1024];
static int  g_find_class_calls = 0;

static jclass JNICALL fake_find_class(JNIEnv* env, const char* name)
{
    (void)env;
    g_find_class_calls += 1;
    if (name != NULL) {
        size_t i = 0;
        for (; i + 1 < sizeof(g_find_class_last_name) && name[i] != '\0'; ++i)
            g_find_class_last_name[i] = name[i];
        g_find_class_last_name[i] = '\0';
    }
    else {
        g_find_class_last_name[0] = '\0';
    }
    return (jclass)(uintptr_t)0x1;   /* 非 NULL 哨兵：守卫不检查返回值 */
}

static void reset_find_class_fake(void)
{
    memset(&g_find_class_table, 0, sizeof(g_find_class_table));
    g_find_class_table.FindClass = fake_find_class;
    g_find_class_calls           = 0;
    g_find_class_last_name[0]    = '\0';
    _mj2x_regc_env               = &g_find_class_env;
}

void test_regc_find_class_desc_shell(void)
{
    // 完整 `L...;` 描述符：剥壳转内部名。
    reset_find_class_fake();
    assert(_mj2x_regc_find_class("Ljava/lang/String;") != NULL);
    assert(g_find_class_calls == 1);
    assert(strcmp(g_find_class_last_name, "java/lang/String") == 0);

    // 裸内部名以 'L' 开头（default 包合成类 Lambda$N，#1182 P1）：直通不改写。
    reset_find_class_fake();
    assert(_mj2x_regc_find_class("Lambda$0") != NULL);
    assert(g_find_class_calls == 1);
    assert(strcmp(g_find_class_last_name, "Lambda$0") == 0);

    // 守卫边界：len<3（"L;"）与缺尾分号（"Ljava/lang/String"）都不剥。
    reset_find_class_fake();
    assert(_mj2x_regc_find_class("L;") != NULL);
    assert(strcmp(g_find_class_last_name, "L;") == 0);

    reset_find_class_fake();
    assert(_mj2x_regc_find_class("Ljava/lang/String") != NULL);
    assert(strcmp(g_find_class_last_name, "Ljava/lang/String") == 0);

    // NULL 入参：早退，FindClass 不被调用。
    reset_find_class_fake();
    assert(_mj2x_regc_find_class(NULL) == NULL);
    assert(g_find_class_calls == 0);

    printf("mj2x_regc_find_class_test: all passed\n");
}

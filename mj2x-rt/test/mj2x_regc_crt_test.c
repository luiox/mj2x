#include "regc/mj2x_regc_crt.h"
#include "common/mj2x_rt_protect.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct thrown_exception
{
    int         count;
    const char* class_name;
    const char* message;
} thrown_exception;

void test_string_buffer_concurrency(void);
void test_regc_intrinsic_c99_abi(void);
void test_regc_intrinsic_bit_ops(void);
void test_regc_intrinsic_DISABLED_round_f32_full_sweep(void);
void test_safe_env(void);
void test_regc_find_class_desc_shell(void);
void test_const_key_derivation(void);
void mj2x_rt_protect_probe_test_run(void);
void mj2x_vmc4_blob_crypto_test_run(void);

static void record_throw(void* env, const char* class_name, const char* message)
{
    thrown_exception* thrown = (thrown_exception*)env;
    if (thrown == NULL) return;
    thrown->count += 1;
    thrown->class_name = class_name;
    thrown->message    = message;
}

static void test_div_rem_and_status_bridge(void)
{
    thrown_exception  thrown = {0};
    mj2x_regc_context context;
    mj2x_regc_init_context(&context, &thrown, record_throw);

    assert(mj2x_regc_div_i32(&context, 9, 3) == 3);
    assert(mj2x_regc_rem_i64(&context, 13, 5) == 3);
    assert(mj2x_regc_div_i32(&context, INT32_MIN, -1) == INT32_MIN);
    assert(mj2x_regc_rem_i64(&context, INT64_MIN, -1) == 0);
    assert(mj2x_regc_div_i32(&context, 1, 0) == 0);
    assert(thrown.count == 1);
    assert(strcmp(thrown.class_name, "java/lang/ArithmeticException") == 0);
    assert(strcmp(thrown.message, "/ by zero") == 0);

    mj2x_regc_report_status(&context, MJ2X_REGC_STATUS_UNSUPPORTED, "test operation");
    assert(thrown.count == 2);
    assert(strcmp(thrown.class_name, "java/lang/UnsupportedOperationException") == 0);
    assert(strcmp(thrown.message, "test operation") == 0);
}

static void test_compare_intrinsics(void)
{
    assert(_mj2x_intrinsic_int_compare(1, 2) == -1);
    assert(_mj2x_intrinsic_long_compare(INT64_MAX, INT64_MIN) == 1);
    assert(_mj2x_intrinsic_byte_compare(INT8_MIN, INT8_MAX) == -255);
    assert(_mj2x_intrinsic_short_compare(300, -20) == 320);
    assert(_mj2x_intrinsic_float_compare(-0.0f, 0.0f) == -1);
    assert(_mj2x_intrinsic_double_compare(0.0, -0.0) == 1);
    assert(_mj2x_intrinsic_bool_compare(42, 1) == 0);
    assert(_mj2x_intrinsic_char_compare((uint16_t)'B', (uint16_t)'A') == 1);
}

static void test_string_builder_utf16_and_errors(void)
{
    const uint16_t       prefix[] = {(uint16_t)'A', 0x4E2D, 0xD83D, 0xDE00};
    thrown_exception     thrown   = {0};
    mj2x_regc_context    context;
    mj2x_string_builder* builder = NULL;

    mj2x_regc_init_context(&context, &thrown, record_throw);
    assert(mj2x_string_builder_init(&context, &builder) == MJ2X_REGC_STATUS_OK);
    assert(builder != NULL);
    assert(mj2x_string_builder_append_utf16(&context, builder, prefix, 4) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_append_i32(&context, builder, -42) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_append_bool(&context, builder, 1) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_utf16_length(builder) == 11);
    int32_t  capacity = 0;
    int32_t  length   = 0;
    uint16_t value    = 0;
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 16);
    assert(mj2x_string_builder_ensure_capacity(&context, builder, 17) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 34);
    assert(mj2x_string_builder_set_length(&context, builder, 20) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_length(&context, builder, &length) == MJ2X_REGC_STATUS_OK);
    assert(length == 20);
    assert(mj2x_string_builder_char_at(&context, builder, 11, &value) == MJ2X_REGC_STATUS_OK);
    assert(value == 0);
    assert(mj2x_string_builder_set_char_at(&context, builder, 11, (uint16_t)'X') ==
           MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_trim_to_size(&context, builder) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 20);
    assert(mj2x_string_builder_append_code_point(&context, builder, 0x1F600) ==
           MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_utf16_length(builder) == 22);

    assert(mj2x_string_builder_append_utf16(&context, NULL, prefix, 1) ==
           MJ2X_REGC_STATUS_INVALID_STATE);
    assert(thrown.count == 1);
    assert(strcmp(thrown.class_name, "java/lang/IllegalStateException") == 0);

    assert(mj2x_string_builder_append_utf16(&context, builder, prefix, SIZE_MAX) ==
           MJ2X_REGC_STATUS_OUT_OF_MEMORY);
    assert(thrown.count == 2);
    assert(strcmp(thrown.class_name, "java/lang/OutOfMemoryError") == 0);

    mj2x_string_builder_destroy(builder);
}

// ---- mj2x_rt_protect 检查点（纯逻辑部分；平台探测走真实进程）----

static void test_rt_protect_deterministic_lower_bound(void)
{
    mj2x_rt_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    // LOG 动作按契约恒返回 0（命中与否只进计数，见 mj2x_rt_protect.h
    // "@return"与 test_rt_protect_fake_and_delay），不能作为命中信号。
    for (int i = 0; i < 3; ++i) {
        assert(mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_LOG, 3, 0, 0) == 0);
    }
    // step=3：3 次调用内必然至少一次触发（确定性下限）；触发面经 triggers
    // 计数观察，计数推进。
    assert(cp.invocations == 3);
    assert(cp.triggers >= 1);
    assert(cp.seed != 0);   // 懒初始化生效。
}

static void test_rt_protect_step_one_triggers_every_call(void)
{
    mj2x_rt_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    for (int i = 0; i < 5; ++i) {
        assert(mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_LOG, 1, 0, 0) == 0);
    }
    assert(cp.invocations == 5);
    assert(cp.triggers == 5);
}

static void test_rt_protect_fake_and_delay(void)
{
    mj2x_rt_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    // step=1 + FAKE：每次命中都返回 1（调用方应伪造数据）。
    assert(mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_FAKE, 1, 0, 0) == 1);
    // LOG 命中返回 0。
    assert(mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_LOG, 1, 0, 0) == 0);
    // DELAY：delay_ms=0 立即返回。
    assert(mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_DELAY, 1, 0, 0) == 0);
    assert(cp.triggers == 3);
}

static void test_rt_protect_large_step_stays_quiet(void)
{
    mj2x_rt_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    cp.seed = 0x12345678u;   // 固定 seed，序列可复现。
    for (int i = 0; i < 100; ++i) {
        mj2x_rt_checkpoint_run(&cp, MJ2X_RT_PROTECT_LOG, 1000000u, 0, 0);
    }
    assert(cp.invocations == 100);
    // 100 次调用、step=1e6：确定性下限不触发；随机命中概率 100/1e6，
    // 固定 seed 下要么 0 要么 1（不脆断）。
    assert(cp.triggers <= 1);
}

// ---- 浮点→整型转换（#1027 D2 回归）----
// JVM f2i/f2l/d2i/d2l 语义（JLS 5.1.3）：NaN→0、向零截断、超界饱和
// [INT_MIN, INT_MAX]/[INT64_MIN, INT64_MAX]。这些向量对修复前的裸 C cast
// 实现是红的：裸 cast 对 NaN/越界是 UB（x64 上 (int)NaN 常得 INT_MIN 而非 0）。

static void test_float_to_int_jvm_semantics(void)
{
    /* mj2x_regc_f2i32：JVM f2i/d2i 口径 */
    assert(mj2x_regc_f2i32((double)NAN) == 0);                            /* NaN→0 */
    assert(mj2x_regc_f2i32((double)INFINITY) == INT32_MAX);               /* +inf 饱和 */
    assert(mj2x_regc_f2i32((double)-INFINITY) == INT32_MIN);              /* -inf 饱和 */
    assert(mj2x_regc_f2i32(1.0e30) == INT32_MAX);                         /* 超大正 */
    assert(mj2x_regc_f2i32(-1.0e30) == INT32_MIN);                        /* 超大负 */
    assert(mj2x_regc_f2i32(0.5) == 0);                                    /* 向零截断 */
    assert(mj2x_regc_f2i32(-0.5) == 0);
    assert(mj2x_regc_f2i32(1.9) == 1);
    assert(mj2x_regc_f2i32(-1.9) == -1);
    assert(mj2x_regc_f2i32(0.0) == 0);
    assert(mj2x_regc_f2i32(-0.0) == 0);
    assert(mj2x_regc_f2i32(2147483647.0) == INT32_MAX);                   /* 2^31-1 界内恰可表示 */
    assert(mj2x_regc_f2i32(2147483648.0) == INT32_MAX);                   /* 2^31 → 饱和 */
    assert(mj2x_regc_f2i32(-2147483648.0) == INT32_MIN);                  /* -2^31 界内恰可表示 */
    assert(mj2x_regc_f2i32(-2147483649.0) == INT32_MIN);                  /* < -2^31 → 饱和 */

    /* mj2x_regc_f2i64：JVM f2l/d2l 口径，饱和点 2^63（(double)INT64_MAX
     * 因 53 位尾数本就舍入为 2^63，与实现注释一致）。 */
    assert(mj2x_regc_f2i64((double)NAN) == 0);
    assert(mj2x_regc_f2i64((double)INFINITY) == INT64_MAX);
    assert(mj2x_regc_f2i64((double)-INFINITY) == INT64_MIN);
    assert(mj2x_regc_f2i64(1.0e30) == INT64_MAX);
    assert(mj2x_regc_f2i64(-1.0e30) == INT64_MIN);
    assert(mj2x_regc_f2i64(0.5) == 0);
    assert(mj2x_regc_f2i64(-0.5) == 0);
    assert(mj2x_regc_f2i64(0.0) == 0);
    assert(mj2x_regc_f2i64(-0.0) == 0);
    assert(mj2x_regc_f2i64(9223372036854775807.0) == INT64_MAX);          /* 字面量舍入为 2^63 → 饱和 */
    assert(mj2x_regc_f2i64(9223372036854774784.0) == 9223372036854774784LL); /* 2^63-2^10 界内 */
    assert(mj2x_regc_f2i64(-9223372036854775808.0) == INT64_MIN);         /* -2^63 界内恰可表示 */
    assert(mj2x_regc_f2i64(-9223372036854777856.0) == INT64_MIN);         /* < -2^63 → 饱和 */
}

int main(int argc, char** argv)
{
    // DISABLED_ 语义门（#1186）：纯 C 驱动器仿 gtest 约定——argv 白名单命中
    // --gtest_also_run_disabled_tests（裸旗标或 =值 形式，=0 关、其余值开，
    // 评审 P3-2）才追加执行 DISABLED_ 前缀测试；缺省时函数集合与输出与既有
    // 套件完全一致（DISABLED_ 不计入日常跑套）。
    // --gtest_filter=DISABLED_* 仅门面兼容不单独路由（gtest 同语义：单独
    // 过滤 DISABLED_ 也是 0 例）。
    int run_disabled = 0;
    int argi;
    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--gtest_also_run_disabled_tests") == 0) {
            run_disabled = 1;
        } else if (strncmp(argv[argi], "--gtest_also_run_disabled_tests=", 32) == 0) {
            run_disabled = (strcmp(argv[argi] + 32, "0") != 0);
        }
    }

    test_div_rem_and_status_bridge();
    test_compare_intrinsics();
    test_float_to_int_jvm_semantics();
    test_regc_intrinsic_c99_abi();
    test_regc_intrinsic_bit_ops();
    test_regc_intrinsic_bit_ops();
    test_string_builder_utf16_and_errors();
    test_string_buffer_concurrency();
    test_safe_env();
    test_regc_find_class_desc_shell();
    test_const_key_derivation();
    test_rt_protect_deterministic_lower_bound();
    test_rt_protect_step_one_triggers_every_call();
    test_rt_protect_fake_and_delay();
    test_rt_protect_large_step_stays_quiet();
    mj2x_rt_protect_probe_test_run();
    mj2x_vmc4_blob_crypto_test_run();
    if (run_disabled) {
        test_regc_intrinsic_DISABLED_round_f32_full_sweep();
    }
    printf("mj2x_regc_crt_test: all passed\n");
    return 0;
}

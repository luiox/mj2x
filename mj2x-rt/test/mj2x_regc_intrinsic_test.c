#include "regc/mj2x_regc_intrinsic.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_at(int line, int condition)
{
    if (!condition)
    {
        fprintf(stderr, "expect failed at line %d\n", line);
        abort();
    }
}
#define expect(c) expect_at(__LINE__, (c))

void test_regc_intrinsic_c99_abi(void)
{
    mj2x_int integer = mj2x_int_value_of(-42);
    mj2x_long wide   = mj2x_long_value_of(INT64_C(1234567890123));
    mj2x_bool truthy = mj2x_bool_value_of(7);
    expect(mj2x_int_int_value(integer) == -42);
    expect(mj2x_long_long_value(wide) == INT64_C(1234567890123));
    expect(mj2x_bool_bool_value(truthy) == 1);

    expect(mj2x_math_abs_i32(INT32_MIN) == INT32_MIN);
    expect(mj2x_math_max_i32(-7, 5) == 5);
    expect(isnan(mj2x_math_min_f64(NAN, 3.0)));
    expect(mj2x_math_round(-2.5) == -2);
    expect(fabs(mj2x_math_sqrt(9.0) - 3.0) < 1e-12);

    // ---- #1117 补位/修复面（对照 jvmtp 1.7.1 修复③：Math.max/min NaN 与
    // ±0.0 位型；g17.math_nan_zero 按 JVM 语义 exec 对拍锁定）----
    {
        const float  fpz = 0.0f, fnz = -0.0f;
        const double dpz = 0.0, dnn = -0.0;
        // ±0.0 结果位型：IEEE 比较视为相等，必须按位断言符号。
        const float rf1 = mj2x_math_max_f32(fpz, fnz);
        const float rf2 = mj2x_math_max_f32(fnz, fpz);
        const float rf3 = mj2x_math_min_f32(fpz, fnz);
        const float rf4 = mj2x_math_min_f32(fnz, fpz);
        expect(memcmp(&rf1, &fpz, sizeof fpz) == 0);
        expect(memcmp(&rf2, &fpz, sizeof fpz) == 0);
        expect(memcmp(&rf3, &fnz, sizeof fnz) == 0);
        expect(memcmp(&rf4, &fnz, sizeof fnz) == 0);
        const double rd1 = mj2x_math_max_f64(dpz, dnn);
        const double rd2 = mj2x_math_max_f64(dnn, dpz);
        const double rd3 = mj2x_math_min_f64(dpz, dnn);
        const double rd4 = mj2x_math_min_f64(dnn, dpz);
        expect(memcmp(&rd1, &dpz, sizeof dpz) == 0);
        expect(memcmp(&rd2, &dpz, sizeof dpz) == 0);
        expect(memcmp(&rd3, &dnn, sizeof dnn) == 0);
        expect(memcmp(&rd4, &dnn, sizeof dnn) == 0);
        // NaN 操作数：返回操作数本身（位型保留）——用带 payload 的非规范 NaN。
        {
            const uint32_t nan_bits = 0x7FC01234u;
            float          nan_operand;
            memcpy(&nan_operand, &nan_bits, sizeof nan_operand);
            const float rn = mj2x_math_max_f32(nan_operand, 1.0f);
            expect(memcmp(&rn, &nan_operand, sizeof nan_operand) == 0);
            expect(isnan(mj2x_math_max_f32(1.0f, nan_operand)));
            expect(isnan(mj2x_math_min_f64(3.0, (double)nan_operand)));
        }
        // round(float)：JVM 公式 (int)floor(a + 0.5f)，NaN→0，饱和夹紧。
        expect(mj2x_math_round_f32(2.5f) == 3);
        expect(mj2x_math_round_f32(3.499f) == 3);
        expect(mj2x_math_round_f32(-2.5f) == -2);
        expect(mj2x_math_round_f32(-2.4f) == -2);
        expect(mj2x_math_round_f32(NAN) == 0);
        expect(mj2x_math_round_f32(2.4e9f) == INT32_MAX);
        expect(mj2x_math_round_f32(-2.4e9f) == INT32_MIN);
        // signum：±0.0 原样返回（位型断言），NaN 透传，其余 ±1。
        expect(mj2x_math_signum_f32(5.0f) == 1.0f);
        expect(mj2x_math_signum_f32(-5.0f) == -1.0f);
        const float sfz = mj2x_math_signum_f32(fnz);
        const float sf_p = mj2x_math_signum_f32(fpz);
        expect(memcmp(&sfz, &fnz, sizeof fnz) == 0);
        expect(memcmp(&sf_p, &fpz, sizeof fpz) == 0);
        expect(isnan(mj2x_math_signum_f32(NAN)));
        expect(mj2x_math_signum_f64(9.0) == 1.0);
        expect(mj2x_math_signum_f64(-9.0) == -1.0);
        const double sdz = mj2x_math_signum_f64(dnn);
        expect(memcmp(&sdz, &dnn, sizeof dnn) == 0);
        expect(isnan(mj2x_math_signum_f64(NAN)));
        // exp/log/log10：特殊值按 IEEE/JVM 语义，普通值 libm 精度档。
        expect(mj2x_math_exp(0.0) == 1.0);
        expect(fabs(mj2x_math_exp(1.0) - 2.718281828459045) < 1e-12);
        expect(isinf(mj2x_math_exp(1e308)));
        expect(mj2x_math_log(1.0) == 0.0);
        expect(isinf(mj2x_math_log(0.0)));
        expect(isnan(mj2x_math_log(-1.0)));
        expect(fabs(mj2x_math_log10(1000.0) - 3.0) < 1e-12);
    }

    mj2x_random random;
    mj2x_random_init(&random, 42);
    expect(mj2x_random_next_int(&random) == -1170105035);
    mj2x_random_init(&random, 42);
    expect(mj2x_random_next_int_bound(&random, 100) == 30);
    expect(mj2x_random_next_int_bound(&random, 100) == 63);
    expect(mj2x_random_next_int_bound(&random, 0) == INT32_MIN);

    static const int64_t expected_next_long[] = {
        INT64_C(-5025562857975149833),
        INT64_C(-5843495416241995736),
        INT64_C(5694868678511409995),
    };
    mj2x_random_init(&random, 42);
    for (unsigned int index = 0; index < sizeof(expected_next_long) / sizeof(expected_next_long[0]);
         ++index) {
        expect(mj2x_random_next_long(&random) == expected_next_long[index]);
    }
}

void test_regc_intrinsic_bit_ops(void)
{
    // 期望值取自 JDK 实际输出（BitCheck 交叉验证）。
    // ---- Integer 位操作 ----
    expect(mj2x_intrinsic_int_rotate_left(0x12345678, 8) == (int32_t)0x34567812);
    expect(mj2x_intrinsic_int_rotate_left(1, 32) == 1);
    expect(mj2x_intrinsic_int_rotate_left(-1, 1) == -1);
    expect(mj2x_intrinsic_int_rotate_right(0x12345678, 8) == (int32_t)0x78123456);
    expect(mj2x_intrinsic_int_rotate_right(1, -1) == 2);
    expect(mj2x_intrinsic_int_reverse(0x00000001) == (int32_t)0x80000000);
    expect(mj2x_intrinsic_int_reverse((int32_t)0x80000000U) == 1);
    expect(mj2x_intrinsic_int_reverse_bytes(0x12345678) == (int32_t)0x78563412);
    expect(mj2x_intrinsic_int_reverse_bytes(0x00000001) == (int32_t)0x01000000);
    expect(mj2x_intrinsic_int_reverse_bytes(-1) == -1);
    expect(mj2x_intrinsic_int_bit_count(0) == 0);
    expect(mj2x_intrinsic_int_bit_count(-1) == 32);
    expect(mj2x_intrinsic_int_bit_count(0x0F0F0F0F) == 16);
    expect(mj2x_intrinsic_int_leading_zeros(0) == 32);
    expect(mj2x_intrinsic_int_leading_zeros(1) == 31);
    expect(mj2x_intrinsic_int_leading_zeros(-1) == 0);
    expect(mj2x_intrinsic_int_trailing_zeros(0) == 32);
    expect(mj2x_intrinsic_int_trailing_zeros(8) == 3);
    expect(mj2x_intrinsic_int_trailing_zeros(-1) == 0);
    expect(mj2x_intrinsic_int_highest_one_bit(0) == 0);
    expect(mj2x_intrinsic_int_highest_one_bit(3) == 2);
    expect(mj2x_intrinsic_int_highest_one_bit(-1) == (int32_t)0x80000000);
    expect(mj2x_intrinsic_int_lowest_one_bit(12) == 4);
    expect(mj2x_intrinsic_int_lowest_one_bit(0) == 0);
    expect(mj2x_intrinsic_int_lowest_one_bit(INT32_MIN) == INT32_MIN);
    expect(mj2x_intrinsic_int_signum(0) == 0);
    expect(mj2x_intrinsic_int_signum(5) == 1);
    expect(mj2x_intrinsic_int_signum(-5) == -1);

    // ---- Long 位操作 ----
    expect(mj2x_intrinsic_long_rotate_left(INT64_C(0x123456789ABCDEF0), 16) ==
           INT64_C(0x56789ABCDEF01234));
    expect(mj2x_intrinsic_long_rotate_left(1, 64) == 1);
    expect(mj2x_intrinsic_long_rotate_right(INT64_C(0x123456789ABCDEF0), 16) ==
           INT64_C(0xDEF0123456789ABC));
    expect(mj2x_intrinsic_long_rotate_right(1, -1) == 2);
    expect(mj2x_intrinsic_long_reverse(1) == INT64_MIN);
    expect(mj2x_intrinsic_long_reverse_bytes(INT64_C(0x1122334455667788)) ==
           INT64_C(0x8877665544332211));
    expect(mj2x_intrinsic_long_reverse_bytes(1) == INT64_C(0x0100000000000000));
    expect(mj2x_intrinsic_long_reverse(INT64_MIN) == 1);
    expect(mj2x_intrinsic_long_bit_count(INT64_C(0)) == 0);
    expect(mj2x_intrinsic_long_bit_count(INT64_C(-1)) == 64);
    expect(mj2x_intrinsic_long_bit_count(INT64_C(0x0F0F0F0F0F0F0F0F)) == 32);
    expect(mj2x_intrinsic_long_leading_zeros(0) == 64);
    expect(mj2x_intrinsic_long_leading_zeros(1) == 63);
    expect(mj2x_intrinsic_long_leading_zeros(-1) == 0);
    expect(mj2x_intrinsic_long_trailing_zeros(0) == 64);
    expect(mj2x_intrinsic_long_trailing_zeros(INT64_C(1) << 40) == 40);
    expect(mj2x_intrinsic_long_highest_one_bit(INT64_C(5)) == 4);
    expect(mj2x_intrinsic_long_highest_one_bit(0) == 0);
    expect(mj2x_intrinsic_long_lowest_one_bit(INT64_C(-8)) == 8);
    expect(mj2x_intrinsic_long_lowest_one_bit(0) == 0);
    expect(mj2x_intrinsic_long_signum(INT64_C(-9)) == -1);
    expect(mj2x_intrinsic_long_signum(INT64_C(0)) == 0);
    expect(mj2x_intrinsic_long_signum(INT64_C(9)) == 1);
}

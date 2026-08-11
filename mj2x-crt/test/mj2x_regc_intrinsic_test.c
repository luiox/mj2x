#include "regc/mj2x_regc_intrinsic.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

static void expect(int condition)
{
    if (!condition) abort();
}

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

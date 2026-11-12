#include "regc/mj2x_regc_intrinsic.h"
#include "regc/mj2x_regc_runtime.h"
#include "regc/mj2x_regc_strictmath.h"

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

// ---- 除零 pending 断言用的 throw 回调（crt 测试同款形状）----
typedef struct scalar_thrown_exception
{
    int         count;
    const char* class_name;
    const char* message;
} scalar_thrown_exception;

static void scalar_record_throw(void* env, const char* class_name, const char* message)
{
    scalar_thrown_exception* thrown = (scalar_thrown_exception*)env;
    if (thrown == NULL) return;
    thrown->count += 1;
    thrown->class_name = class_name;
    thrown->message    = message;
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

    // ---- #1117 补位/修复面（对照 jvmtp 1.7.1 修复③：Math.max/min NaN 与
    // ±0.0 位型；g17.math_nan_zero 按 JVM 语义 exec 比对锁定）----
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
        // NaN 操作数：返回操作数本身（位型保留），用带 payload 的非规范 NaN。
        {
            const uint32_t nan_bits = 0x7FC01234u;
            float          nan_operand;
            memcpy(&nan_operand, &nan_bits, sizeof nan_operand);
            const float rn = mj2x_math_max_f32(nan_operand, 1.0f);
            expect(memcmp(&rn, &nan_operand, sizeof nan_operand) == 0);
            expect(isnan(mj2x_math_max_f32(1.0f, nan_operand)));
            expect(isnan(mj2x_math_min_f64(3.0, (double)nan_operand)));
        }
        // ---- round(F)I / round(D)J：OpenJDK 17 位运算实现（#1144）----
        // 期望值全部取自 HotSpot 17.0.20.1 (Temurin) Math.round 实测
        //（临时 Java 探针，不入库）；输入按位型构造，杜绝字面量解析歧义。
        // 两个经典发散形状（floor(x+0.5) 公式的 JDK-8010430 类缺陷）：
        // 0.49999997f 公式给 1 实际 0；8388609.0f 公式给 8388610 实际 8388609。
        {
            struct RoundFCase
            {
                uint32_t bits;
                int32_t  expected;
            };
            static const struct RoundFCase cases[] = {
                {0x3EFFFFFFu, 0},           // 0.49999997f（最大的小于 0.5）
                {0x3EFFFFFDu, 0},           // 0.4999999f（次边界）
                {0x3F000000u, 1},           // 0.5f
                {0x3FBFFFFFu, 1},           // 1.4999999f
                {0x3FC00000u, 2},           // 1.5f（tie→+∞）
                {0xBFC00000u, -1},          // -1.5f（tie→+∞）
                {0x40200000u, 3},           // 2.5f
                {0xC0200000u, -2},          // -2.5f
                {0xBF000000u, 0},           // -0.5f（tie→+∞）
                {0xBEFFFFFFu, 0},           // -0.49999997f
                {0x4B000001u, 8388609},     // [2^23,2^24) 奇整数（公式差 +1）
                {0xCB000001u, -8388609},    // 同上负侧（公式差 -1）
                {0x4B000000u, 8388608},     // 2^23 整数
                {0x4B7FFFFFu, 16777215},    // 2^24-1 整数
                {0xCB7FFFFFu, -16777215},
                {0x00000000u, 0},           // +0.0
                {0x80000000u, 0},           // -0.0
                {0x7FC00000u, 0},           // NaN
                {0x7F800000u, INT32_MAX},   // +Inf
                {0xFF800000u, INT32_MIN},   // -Inf
                {0x00000001u, 0},           // Float.MIN_VALUE
                {0x80000001u, 0},           // -MIN_VALUE
                {0x4EFFFFFFu, 2147483520},  // ≤ INT_MAX 的最大 float
                {0x4F000000u, INT32_MAX},   // 2^31（> INT_MAX，饱和）
                {0xCF000000u, INT32_MIN},   // -2^31
                {0x4F0F0D18u, INT32_MAX},   // 2.4e9f 饱和
                {0xCF0F0D18u, INT32_MIN},
            };
            for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                float   input;
                int32_t got;
                memcpy(&input, &cases[i].bits, sizeof input);
                got = mj2x_math_round_f32(input);
                if (got != cases[i].expected)
                {
                    fprintf(stderr,
                            "round_f32(0x%08X) = %d, expected %d\n",
                            (unsigned int)cases[i].bits, (int)got, (int)cases[i].expected);
                }
                expect(got == cases[i].expected);
            }
        }
        {
            struct RoundDCase
            {
                uint64_t bits;
                int64_t  expected;
            };
            static const struct RoundDCase cases[] = {
                {UINT64_C(0x3FDFFFFFFFFFFFFF), 0},                   // 最大的小于 0.5
                {UINT64_C(0x3FDFFFFFFFFFFFFE), 0},                   // 次边界
                {UINT64_C(0x3FE0000000000000), 1},                   // 0.5
                {UINT64_C(0xBFE0000000000000), 0},                   // -0.5（tie→+∞）
                {UINT64_C(0xBFDFFFFFFFFFFFFF), 0},                   // -0.49999999999999994
                {UINT64_C(0x4330000000000001), INT64_C(4503599627370497)},  // 2^52+1 奇整数
                {UINT64_C(0xC330000000000001), INT64_C(-4503599627370497)},
                {UINT64_C(0x4330000000000000), INT64_C(4503599627370496)},  // 2^52
                {UINT64_C(0x433FFFFFFFFFFFFF), INT64_C(9007199254740991)},  // 2^53-1
                {UINT64_C(0xC33FFFFFFFFFFFFF), INT64_C(-9007199254740991)},
                {UINT64_C(0x4004000000000000), 3},                   // 2.5
                {UINT64_C(0xC004000000000000), -2},                  // -2.5
                {UINT64_C(0x0000000000000000), 0},                   // +0.0
                {UINT64_C(0x8000000000000000), 0},                   // -0.0
                {UINT64_C(0x7FF8000000000000), 0},                   // NaN
                {UINT64_C(0x7FF0000000000000), INT64_MAX},           // +Inf
                {UINT64_C(0xFFF0000000000000), INT64_MIN},           // -Inf
                {UINT64_C(0x0000000000000001), 0},                   // Double.MIN_VALUE
                {UINT64_C(0x43E0000000000000), INT64_MAX},           // 2^63（饱和）
                {UINT64_C(0xC3E0000000000000), INT64_MIN},           // -2^63
                {UINT64_C(0x7E37E43C8800759C), INT64_MAX},           // 1e300 饱和
                {UINT64_C(0xFE37E43C8800759C), INT64_MIN},           // -1e300
            };
            for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                double  input;
                int64_t got;
                memcpy(&input, &cases[i].bits, sizeof input);
                got = mj2x_math_round(input);
                if (got != cases[i].expected)
                {
                    fprintf(stderr,
                            "round(0x%016llX) = %lld, expected %lld\n",
                            (unsigned long long)cases[i].bits,
                            (long long)got, (long long)cases[i].expected);
                }
                expect(got == cases[i].expected);
            }
        }
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

// ---- Float/Double bits 视图 ----
// floatToIntBits 把任意 NaN 位型规范化为 0x7FC00000，floatToRawIntBits 逐位
// 保留（含 NaN payload）；±0.0/Infinity 两版都原样。

void test_regc_intrinsic_bits_views(void)
{
    // NaN 规范化：带 payload 的非规范 NaN。
    {
        const uint32_t payload_bits = 0x7FC01234U;
        float          payload_nan;
        memcpy(&payload_nan, &payload_bits, sizeof payload_nan);
        expect(mj2x_intrinsic_float_to_int_bits(payload_nan) == (int32_t)0x7FC00000);
        expect(mj2x_intrinsic_float_to_raw_int_bits(payload_nan) == (int32_t)0x7FC01234);
        // canonical NaN 幂等。
        expect(mj2x_intrinsic_float_to_int_bits((float)NAN) == (int32_t)0x7FC00000);
        // 负侧 payload NaN 同样规范化：规范化到 +NaN 0x7FC00000，符号位丢弃。
        const uint32_t neg_payload_bits = 0xFFC01234U;
        float          neg_payload_nan;
        memcpy(&neg_payload_nan, &neg_payload_bits, sizeof neg_payload_nan);
        expect(mj2x_intrinsic_float_to_int_bits(neg_payload_nan) == (int32_t)0x7FC00000);
        expect(mj2x_intrinsic_float_to_raw_int_bits(neg_payload_nan) == (int32_t)0xFFC01234);
        // Infinity / ±0.0 位型原样（两版一致）。
        expect(mj2x_intrinsic_float_to_int_bits((float)INFINITY) == (int32_t)0x7F800000);
        expect(mj2x_intrinsic_float_to_raw_int_bits((float)INFINITY) == (int32_t)0x7F800000);
        expect(mj2x_intrinsic_float_to_int_bits(-0.0f) == (int32_t)0x80000000);
        expect(mj2x_intrinsic_float_to_raw_int_bits(-0.0f) == (int32_t)0x80000000);
        expect(mj2x_intrinsic_float_to_int_bits(0.0f) == 0);
        expect(mj2x_intrinsic_float_to_int_bits(1.0f) == (int32_t)0x3F800000);
        // intBitsToFloat 往返（含 NaN 位型直通）。
        expect(mj2x_intrinsic_int_bits_to_float((int32_t)0x3F800000) == 1.0f);
        expect(mj2x_intrinsic_int_bits_to_float((int32_t)0x80000000) == -0.0f);
        {
            const float round_trip =
                mj2x_intrinsic_int_bits_to_float((int32_t)0x7FC01234U);
            expect(mj2x_intrinsic_float_to_raw_int_bits(round_trip) == (int32_t)0x7FC01234);
        }
        // double：规范化 0x7FF8000000000000，raw 逐位保留。
        const uint64_t d_payload_bits = UINT64_C(0x7FF8000000123456);
        double         d_payload_nan;
        memcpy(&d_payload_nan, &d_payload_bits, sizeof d_payload_nan);
        expect(mj2x_intrinsic_double_to_long_bits(d_payload_nan) ==
               (int64_t)UINT64_C(0x7FF8000000000000));
        expect(mj2x_intrinsic_double_to_raw_long_bits(d_payload_nan) ==
               (int64_t)UINT64_C(0x7FF8000000123456));
        const uint64_t d_neg_payload_bits = UINT64_C(0xFFF8000000123456);
        double         d_neg_payload_nan;
        memcpy(&d_neg_payload_nan, &d_neg_payload_bits, sizeof d_neg_payload_nan);
        expect(mj2x_intrinsic_double_to_long_bits(d_neg_payload_nan) ==
               (int64_t)UINT64_C(0x7FF8000000000000));
        expect(mj2x_intrinsic_double_to_raw_long_bits(d_neg_payload_nan) ==
               (int64_t)UINT64_C(0xFFF8000000123456));
        expect(mj2x_intrinsic_double_to_long_bits((double)INFINITY) ==
               (int64_t)UINT64_C(0x7FF0000000000000));
        expect(mj2x_intrinsic_double_to_long_bits(-0.0) ==
               (int64_t)UINT64_C(0x8000000000000000));
        expect(mj2x_intrinsic_double_to_long_bits(1.0) == (int64_t)UINT64_C(0x3FF0000000000000));
        expect(mj2x_intrinsic_long_bits_to_double(INT64_C(0x3FF0000000000000)) == 1.0);
        expect(mj2x_intrinsic_long_bits_to_double((int64_t)UINT64_C(0x8000000000000000)) == -0.0);
        {
            const double d_round_trip =
                mj2x_intrinsic_long_bits_to_double((int64_t)UINT64_C(0x7FF8000000123456));
            expect(mj2x_intrinsic_double_to_raw_long_bits(d_round_trip) ==
                   (int64_t)UINT64_C(0x7FF8000000123456));
        }
    }
}

// ---- Integer/Long 无符号族 + Character 静态代理 ----
// divideUnsigned/remainderUnsigned 除零断言 pending
// ArithmeticException("/ by zero")，RegCLocalHelper 形态的语义锚点。

void test_regc_intrinsic_unsigned_and_char(void)
{
    // compareUnsigned：无符号序（-1 = 0xFFFFFFFF 是最大值）。
    expect(mj2x_intrinsic_int_compare_unsigned(-1, 1) == 1);
    expect(mj2x_intrinsic_int_compare_unsigned(1, -1) == -1);
    expect(mj2x_intrinsic_int_compare_unsigned(0, INT32_MIN) == -1);
    expect(mj2x_intrinsic_int_compare_unsigned(INT32_MIN, INT32_MIN) == 0);
    /* 0x7FFFFFFF < 0x80000000（无符号序）。 */
    expect(mj2x_intrinsic_int_compare_unsigned(INT32_MAX, INT32_MIN) == -1);
    expect(mj2x_intrinsic_int_compare_unsigned(INT32_MIN, INT32_MAX) == 1);
    expect(mj2x_intrinsic_long_compare_unsigned(-1, 1) == 1);
    expect(mj2x_intrinsic_long_compare_unsigned(INT64_MIN, INT64_MAX) == 1);
    expect(mj2x_intrinsic_long_compare_unsigned(INT64_C(5), INT64_C(5)) == 0);

    // divideUnsigned/remainderUnsigned：位型与 JDK 一致。
    {
        scalar_thrown_exception thrown = {0, NULL, NULL};
        mj2x_regc_context       context;
        mj2x_regc_init_context(&context, &thrown, scalar_record_throw);

        expect(mj2x_intrinsic_int_divide_unsigned(&context, 7, 2) == 3);
        expect(mj2x_intrinsic_int_remainder_unsigned(&context, 7, 2) == 1);
        /* 无符号 0x80000000 / 0xFFFFFFFF = 0，余 0x80000000。 */
        expect(mj2x_intrinsic_int_divide_unsigned(&context, INT32_MIN, -1) == 0);
        expect(mj2x_intrinsic_int_remainder_unsigned(&context, INT32_MIN, -1) == INT32_MIN);
        /* 0xFFFFFFFF / 2 = 0x7FFFFFFF，余 1。 */
        expect(mj2x_intrinsic_int_divide_unsigned(&context, -1, 2) == INT32_MAX);
        expect(mj2x_intrinsic_int_remainder_unsigned(&context, -1, 2) == 1);
        expect(mj2x_intrinsic_long_divide_unsigned(&context, -1, 3) ==
               INT64_C(6148914691236517205));
        expect(mj2x_intrinsic_long_remainder_unsigned(&context, -1, 3) == 0);
        expect(mj2x_intrinsic_long_divide_unsigned(&context, INT64_MIN, -1) == 0);
        expect(mj2x_intrinsic_long_remainder_unsigned(&context, INT64_MIN, -1) == INT64_MIN);
        expect(thrown.count == 0);
        /* 除零：挂 pending ArithmeticException("/ by zero") 并返回 0。 */
        expect(mj2x_intrinsic_int_divide_unsigned(&context, 1, 0) == 0);
        expect(thrown.count == 1);
        expect(strcmp(thrown.class_name, "java/lang/ArithmeticException") == 0);
        expect(strcmp(thrown.message, "/ by zero") == 0);
        expect(mj2x_intrinsic_int_remainder_unsigned(&context, 1, 0) == 0);
        expect(thrown.count == 2);
        expect(mj2x_intrinsic_long_divide_unsigned(&context, 1, 0) == 0);
        expect(thrown.count == 3);
        expect(mj2x_intrinsic_long_remainder_unsigned(&context, 1, 0) == 0);
        expect(thrown.count == 4);
    }

    // Character 静态代理：0xD7FF/0xD800/0xDBFF/0xDC00/0xDFFF/0xE000 边界。
    expect(mj2x_intrinsic_char_is_high_surrogate((uint16_t)0xD800) == 1);
    expect(mj2x_intrinsic_char_is_high_surrogate((uint16_t)0xDBFF) == 1);
    expect(mj2x_intrinsic_char_is_high_surrogate((uint16_t)0xDC00) == 0);
    expect(mj2x_intrinsic_char_is_high_surrogate((uint16_t)0xD7FF) == 0);
    expect(mj2x_intrinsic_char_is_low_surrogate((uint16_t)0xDC00) == 1);
    expect(mj2x_intrinsic_char_is_low_surrogate((uint16_t)0xDFFF) == 1);
    expect(mj2x_intrinsic_char_is_low_surrogate((uint16_t)0xD7FF) == 0);
    expect(mj2x_intrinsic_char_is_low_surrogate((uint16_t)0xE000) == 0);
    expect(mj2x_intrinsic_char_is_surrogate((uint16_t)0xD800) == 1);
    expect(mj2x_intrinsic_char_is_surrogate((uint16_t)0xDFFF) == 1);
    expect(mj2x_intrinsic_char_is_surrogate((uint16_t)0xD7FF) == 0);
    expect(mj2x_intrinsic_char_is_surrogate((uint16_t)0xE000) == 0);
    expect(mj2x_intrinsic_char_is_surrogate_pair((uint16_t)0xD83D, (uint16_t)0xDE00) == 1);
    expect(mj2x_intrinsic_char_is_surrogate_pair((uint16_t)0xD83D, (uint16_t)0x0041) == 0);
    expect(mj2x_intrinsic_char_is_surrogate_pair((uint16_t)0x0041, (uint16_t)0xD800) == 0);
    /* high/lowSurrogate：OpenJDK 公式（0x1F600 -> D83D/DE00）。 */
    expect(mj2x_intrinsic_char_high_surrogate(0x1F600) == (uint16_t)0xD83D);
    expect(mj2x_intrinsic_char_low_surrogate(0x1F600) == (uint16_t)0xDE00);
    expect(mj2x_intrinsic_char_high_surrogate(0) == (uint16_t)0xD7C0);
    expect(mj2x_intrinsic_char_low_surrogate(0) == (uint16_t)0xDC00);
    /* toCodePoint：代理对往返。 */
    expect(mj2x_intrinsic_char_to_code_point((uint16_t)0xD83D, (uint16_t)0xDE00) == 0x1F600);
    expect(mj2x_intrinsic_char_to_code_point((uint16_t)0xD800, (uint16_t)0xDC00) == 0x10000);
    /* isBmpCodePoint：负值（>0xFFFF 无符号序）非 BMP。 */
    expect(mj2x_intrinsic_char_is_bmp_code_point(0xFFFF) == 1);
    expect(mj2x_intrinsic_char_is_bmp_code_point(0x10000) == 0);
    expect(mj2x_intrinsic_char_is_bmp_code_point(-1) == 0);
    /* isValidCodePoint：[0, 0x10FFFF]。 */
    expect(mj2x_intrinsic_char_is_valid_code_point(0) == 1);
    expect(mj2x_intrinsic_char_is_valid_code_point(0x10FFFF) == 1);
    expect(mj2x_intrinsic_char_is_valid_code_point(0x110000) == 0);
    expect(mj2x_intrinsic_char_is_valid_code_point(-1) == 0);
    /* charCount：cp >= 0x10000 ? 2 : 1（负值/越界不校验）。 */
    expect(mj2x_intrinsic_char_char_count(0xFFFF) == 1);
    expect(mj2x_intrinsic_char_char_count(0x10000) == 2);
    expect(mj2x_intrinsic_char_char_count(-1) == 1);
    expect(mj2x_intrinsic_char_char_count(0x110000) == 2);
}

// ---- parseUnsigned 族：符号/radix/边界 ----
//
// 解析路径经 _mj2x_regc_env 取字符串：fake JNIEnv 把 jstring 当 C 串直通
// （GetStringUTFChars 原样返回、ReleaseStringUTFChars 空操作），NFE 走
// context->throwException 记录断言，不依赖真实 JVM。

static const char* JNICALL parse_fake_get_utf_chars(JNIEnv* env, jstring str, jboolean* is_copy)
{
    (void)env;
    (void)is_copy;
    return (const char*)str;
}

static void JNICALL parse_fake_release_utf_chars(JNIEnv* env, jstring str, const char* chars)
{
    (void)env;
    (void)str;
    (void)chars;
}

void test_regc_intrinsic_parse_unsigned(void)
{
    static struct JNINativeInterface_ parse_table;
    static JNIEnv                     parse_env = &parse_table;
    parse_table.GetStringUTFChars     = parse_fake_get_utf_chars;
    parse_table.ReleaseStringUTFChars = parse_fake_release_utf_chars;
    JNIEnv* saved_env                 = _mj2x_regc_env;
    _mj2x_regc_env                    = &parse_env;

    scalar_thrown_exception thrown = {0, NULL, NULL};
    mj2x_regc_context       context;
    mj2x_regc_init_context(&context, &thrown, scalar_record_throw);

    // '+' 前缀接受。
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"+42") == 42);
    expect(_mj2x_intrinsic_parse_unsigned_long(&context, (mj2x_ref)"+42") == 42);
    expect(thrown.count == 0);
    // '-' 拒绝（NFE）。
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"-1") == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.class_name, "java/lang/NumberFormatException") == 0);
    // 空串 → NFE。
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"") == 0);
    expect(thrown.count == 1);
    // 上界："4294967295" -> -1（位型）、"18446744073709551615" -> -1。
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"4294967295") == -1);
    expect(_mj2x_intrinsic_parse_unsigned_long(&context, (mj2x_ref)"18446744073709551615") == -1);
    expect(thrown.count == 0);
    // 超上界 → NFE。
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"4294967296") == 0);
    expect(thrown.count == 1);
    // radix 边界：1/37 → NFE；2/36 合法。
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int_radix(&context, (mj2x_ref)"101", 1) == 0);
    expect(thrown.count == 1);
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int_radix(&context, (mj2x_ref)"101", 37) == 0);
    expect(thrown.count == 1);
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int_radix(&context, (mj2x_ref)"1010", 2) == 10);
    expect(_mj2x_intrinsic_parse_unsigned_int_radix(&context, (mj2x_ref)"zz", 36) == 1295);
    expect(_mj2x_intrinsic_parse_unsigned_long_radix(&context, (mj2x_ref)"z", 36) == 35);
    expect(thrown.count == 0);
    // null 输入 → NFE（非 NPE）。
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, NULL) == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.class_name, "java/lang/NumberFormatException") == 0);

    _mj2x_regc_env = saved_env;
}

// ---- StrictMath 位精确 12 条 + Math rint/cbrt：锚点矩阵 ----
//
// 期望位型取自 JDK 17.0.20.1 (Temurin) StrictMath 实测探针（仓外 harness，
// 不入库）；输入按位型构造，杜绝字面量解析歧义。全量位精确比对（每函数
// 1e5+ 随机位模式 + 特殊值）由仓外 harness 完成，见 PR 说明；此处锁定
// 特殊值行为与代表点，防实现漂移。
void test_regc_intrinsic_strictmath_anchors(void)
{
    static double dnan = 0.0;
    {
        const uint64_t nan_bits = UINT64_C(0x7FF8000000000000);
        memcpy(&dnan, &nan_bits, sizeof dnan);
    }
    struct Anchor1
    {
        const char* label;
        uint64_t    input;
        uint64_t    expected;
        double (*fn)(double);
    };
    static const struct Anchor1 anchors1[] = {
        {"sin(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FEAED548F090CEE), mj2x_strictmath_sin},
        {"sin(1e300)", UINT64_C(0x7E37E43C8800759C), UINT64_C(0xBFEA2C16B010E385), mj2x_strictmath_sin},
        {"sin(-0.0)", UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000), mj2x_strictmath_sin},
        {"cos(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FE14A280FB5068C), mj2x_strictmath_cos},
        {"cos(1e300)", UINT64_C(0x7E37E43C8800759C), UINT64_C(0xBFE2699022ADC4C1), mj2x_strictmath_cos},
        {"tan(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FF8EB245CBEE3A6), mj2x_strictmath_tan},
        {"tan(1e300)", UINT64_C(0x7E37E43C8800759C), UINT64_C(0x3FF6BE411F37AC77), mj2x_strictmath_tan},
        {"asin(0.5)", UINT64_C(0x3FE0000000000000), UINT64_C(0x3FE0C152382D7366), mj2x_strictmath_asin},
        {"asin(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FF921FB54442D18), mj2x_strictmath_asin},
        {"asin(-1.0)", UINT64_C(0xBFF0000000000000), UINT64_C(0xBFF921FB54442D18), mj2x_strictmath_asin},
        {"acos(0.5)", UINT64_C(0x3FE0000000000000), UINT64_C(0x3FF0C152382D7366), mj2x_strictmath_acos},
        {"acos(-1.0)", UINT64_C(0xBFF0000000000000), UINT64_C(0x400921FB54442D18), mj2x_strictmath_acos},
        {"acos(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x0000000000000000), mj2x_strictmath_acos},
        {"atan(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FE921FB54442D18), mj2x_strictmath_atan},
        {"atan(inf)", UINT64_C(0x7FF0000000000000), UINT64_C(0x3FF921FB54442D18), mj2x_strictmath_atan},
        {"exp(1.0)", UINT64_C(0x3FF0000000000000), UINT64_C(0x4005BF0A8B14576A), mj2x_strictmath_exp},
        {"exp(709.782712893384)", UINT64_C(0x40862E42FEFA39EF), UINT64_C(0x7FEFFFFFFFFFFF2A), mj2x_strictmath_exp},
        {"exp(710.0)", UINT64_C(0x4086300000000000), UINT64_C(0x7FF0000000000000), mj2x_strictmath_exp},
        {"exp(-745.0)", UINT64_C(0xC087480000000000), UINT64_C(0x0000000000000001), mj2x_strictmath_exp},
        {"log(2.0)", UINT64_C(0x4000000000000000), UINT64_C(0x3FE62E42FEFA39EF), mj2x_strictmath_log},
        {"log(10.0)", UINT64_C(0x4024000000000000), UINT64_C(0x40026BB1BBB55516), mj2x_strictmath_log},
        {"log(0.5)", UINT64_C(0x3FE0000000000000), UINT64_C(0xBFE62E42FEFA39EF), mj2x_strictmath_log},
        {"log(0.0)", UINT64_C(0x0000000000000000), UINT64_C(0xFFF0000000000000), mj2x_strictmath_log},
        {"log10(100.0)", UINT64_C(0x4059000000000000), UINT64_C(0x4000000000000000), mj2x_strictmath_log10},
        {"log10(0.3)", UINT64_C(0x3FD3333333333333), UINT64_C(0xBFE0BB6C34D81502), mj2x_strictmath_log10},
        {"log10(0.0)", UINT64_C(0x0000000000000000), UINT64_C(0xFFF0000000000000), mj2x_strictmath_log10},
        {"cbrt(27.0)", UINT64_C(0x403B000000000000), UINT64_C(0x4008000000000000), mj2x_strictmath_cbrt},
        {"cbrt(-8.0)", UINT64_C(0xC020000000000000), UINT64_C(0xC000000000000000), mj2x_strictmath_cbrt},
        {"cbrt(1e-7)", UINT64_C(0x3E7AD7F29ABCAF48), UINT64_C(0x3F73030F03DE9991), mj2x_strictmath_cbrt},
        {"rint(2.5)", UINT64_C(0x4004000000000000), UINT64_C(0x4000000000000000), mj2x_strictmath_rint},
        {"rint(3.5)", UINT64_C(0x400C000000000000), UINT64_C(0x4010000000000000), mj2x_strictmath_rint},
        {"rint(-2.5)", UINT64_C(0xC004000000000000), UINT64_C(0xC000000000000000), mj2x_strictmath_rint},
        {"rint(4.5)", UINT64_C(0x4012000000000000), UINT64_C(0x4010000000000000), mj2x_strictmath_rint},
        {"rint(0.5)", UINT64_C(0x3FE0000000000000), UINT64_C(0x0000000000000000), mj2x_strictmath_rint},
        {"rint(-0.5)", UINT64_C(0xBFE0000000000000), UINT64_C(0x8000000000000000), mj2x_strictmath_rint},
        {"rint(2^52+1)", UINT64_C(0x4330000000000001), UINT64_C(0x4330000000000001), mj2x_strictmath_rint},
    };
    struct Anchor2
    {
        const char* label;
        uint64_t    input_a;
        uint64_t    input_b;
        uint64_t    expected;
        double (*fn)(double, double);
    };
    static const struct Anchor2 anchors2[] = {
        {"atan2(1,1)", UINT64_C(0x3FF0000000000000), UINT64_C(0x3FF0000000000000), UINT64_C(0x3FE921FB54442D18), mj2x_strictmath_atan2},
        {"atan2(0,-0)", UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x400921FB54442D18), mj2x_strictmath_atan2},
        {"atan2(0,-1)", UINT64_C(0x0000000000000000), UINT64_C(0xBFF0000000000000), UINT64_C(0x400921FB54442D18), mj2x_strictmath_atan2},
        {"atan2(-1,-1)", UINT64_C(0xBFF0000000000000), UINT64_C(0xBFF0000000000000), UINT64_C(0xC002D97C7F3321D2), mj2x_strictmath_atan2},
        {"atan2(inf,inf)", UINT64_C(0x7FF0000000000000), UINT64_C(0x7FF0000000000000), UINT64_C(0x3FE921FB54442D18), mj2x_strictmath_atan2},
        {"pow(2,10)", UINT64_C(0x4000000000000000), UINT64_C(0x4024000000000000), UINT64_C(0x4090000000000000), mj2x_strictmath_pow},
        {"pow(2,0.5)", UINT64_C(0x4000000000000000), UINT64_C(0x3FE0000000000000), UINT64_C(0x3FF6A09E667F3BCD), mj2x_strictmath_pow},
        {"pow(0.5,-1)", UINT64_C(0x3FE0000000000000), UINT64_C(0xBFF0000000000000), UINT64_C(0x4000000000000000), mj2x_strictmath_pow},
        {"pow(-2,3)", UINT64_C(0xC000000000000000), UINT64_C(0x4008000000000000), UINT64_C(0xC020000000000000), mj2x_strictmath_pow},
        {"pow(7,0)", UINT64_C(0x401C000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x3FF0000000000000), mj2x_strictmath_pow},
        {"pow(inf,2)", UINT64_C(0x7FF0000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x7FF0000000000000), mj2x_strictmath_pow},
        {"pow(-inf,3)", UINT64_C(0xFFF0000000000000), UINT64_C(0x4008000000000000), UINT64_C(0xFFF0000000000000), mj2x_strictmath_pow},
        {"pow(-inf,2)", UINT64_C(0xFFF0000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x7FF0000000000000), mj2x_strictmath_pow},
        {"hypot(3,4)", UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000), UINT64_C(0x4014000000000000), mj2x_strictmath_hypot},
        {"hypot(1e300,1e300)", UINT64_C(0x7E37E43C8800759C), UINT64_C(0x7E37E43C8800759C), UINT64_C(0x7E40E4D50F99B211), mj2x_strictmath_hypot},
        {"hypot(3e-320,4e-320)", UINT64_C(0x00000000000017B8), UINT64_C(0x0000000000001FA0), UINT64_C(0x0000000000002788), mj2x_strictmath_hypot},
        {"hypot(inf,1)", UINT64_C(0x7FF0000000000000), UINT64_C(0x3FF0000000000000), UINT64_C(0x7FF0000000000000), mj2x_strictmath_hypot},
    };
    for (size_t i = 0; i < sizeof anchors1 / sizeof anchors1[0]; ++i) {
        double   input;
        double   got;
        uint64_t got_bits;
        memcpy(&input, &anchors1[i].input, sizeof input);
        got      = anchors1[i].fn(input);
        got_bits = 0;
        memcpy(&got_bits, &got, sizeof got_bits);
        if (got_bits != anchors1[i].expected) {
            fprintf(stderr, "strictmath anchor %s: got 0x%016llX want 0x%016llX\n",
                    anchors1[i].label, (unsigned long long)got_bits,
                    (unsigned long long)anchors1[i].expected);
        }
        expect(got_bits == anchors1[i].expected);
    }
    for (size_t i = 0; i < sizeof anchors2 / sizeof anchors2[0]; ++i) {
        double   input_a;
        double   input_b;
        double   got;
        uint64_t got_bits;
        memcpy(&input_a, &anchors2[i].input_a, sizeof input_a);
        memcpy(&input_b, &anchors2[i].input_b, sizeof input_b);
        got      = anchors2[i].fn(input_a, input_b);
        got_bits = 0;
        memcpy(&got_bits, &got, sizeof got_bits);
        if (got_bits != anchors2[i].expected) {
            fprintf(stderr, "strictmath anchor %s: got 0x%016llX want 0x%016llX\n",
                    anchors2[i].label, (unsigned long long)got_bits,
                    (unsigned long long)anchors2[i].expected);
        }
        expect(got_bits == anchors2[i].expected);
    }
    /* NaN/Inf 传播：NaN -> 规范 NaN，Inf -> NaN（trig 三条）与恒等（hypot）。 */
    {
        uint64_t bits;
        double   got;
        got = mj2x_strictmath_sin(dnan);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0x7FF8000000000000));
        got = mj2x_strictmath_sin(HUGE_VAL);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0xFFF8000000000000) || bits == UINT64_C(0x7FF8000000000000));
        got = mj2x_strictmath_pow(1.0, dnan);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0x7FF8000000000000));
        got = mj2x_strictmath_pow(-1.0, HUGE_VAL);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0xFFF8000000000000) || bits == UINT64_C(0x7FF8000000000000));
        got = mj2x_strictmath_hypot(dnan, 1.0);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0x7FF8000000000000));
        got = mj2x_strictmath_acos(1.5);
        memcpy(&bits, &got, sizeof bits);
        expect(bits == UINT64_C(0xFFF8000000000000) || bits == UINT64_C(0x7FF8000000000000));
    }
}

// ---- Math.floorDiv/floorMod：值语义 + 除零 pending + MIN/-1 回绕 ----
void test_regc_intrinsic_floor_div_mod(void)
{
    scalar_thrown_exception thrown = {0, NULL, NULL};
    mj2x_regc_context       context;
    mj2x_regc_init_context(&context, &thrown, scalar_record_throw);

    /* 值语义（期望值取自 JDK 17 Math.floorDiv/floorMod 实测）。 */
    expect(mj2x_intrinsic_math_floor_div_i32(&context, 7, 2) == 3);
    expect(mj2x_intrinsic_math_floor_mod_i32(&context, 7, 2) == 1);
    expect(mj2x_intrinsic_math_floor_div_i32(&context, -7, 2) == -4);
    expect(mj2x_intrinsic_math_floor_mod_i32(&context, -7, 2) == 1);
    expect(mj2x_intrinsic_math_floor_mod_i32(&context, 7, -2) == -1);
    expect(mj2x_intrinsic_math_floor_div_i64(&context, 7, 2) == 3);
    expect(mj2x_intrinsic_math_floor_mod_i64(&context, -7, 2) == 1);
    /* MIN/-1：JVM 位型回绕（C 有符号除法未定义行为，先分流）。 */
    expect(mj2x_intrinsic_math_floor_div_i32(&context, INT32_MIN, -1) == INT32_MIN);
    expect(mj2x_intrinsic_math_floor_mod_i32(&context, INT32_MIN, -1) == 0);
    expect(mj2x_intrinsic_math_floor_div_i64(&context, INT64_MIN, -1) == INT64_MIN);
    expect(mj2x_intrinsic_math_floor_mod_i64(&context, INT64_MIN, -1) == 0);
    expect(thrown.count == 0);
    /* 除零：挂 pending ArithmeticException("/ by zero") 并返回 0。 */
    expect(mj2x_intrinsic_math_floor_div_i32(&context, 1, 0) == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.class_name, "java/lang/ArithmeticException") == 0);
    expect(strcmp(thrown.message, "/ by zero") == 0);
    expect(mj2x_intrinsic_math_floor_mod_i32(&context, 1, 0) == 0);
    expect(thrown.count == 2);
    expect(mj2x_intrinsic_math_floor_div_i64(&context, 1, 0) == 0);
    expect(thrown.count == 3);
    expect(mj2x_intrinsic_math_floor_mod_i64(&context, 1, 0) == 0);
    expect(thrown.count == 4);
}

// ---- Character.digit 镜像助手：ASCII/Nd 区段/全角别名/radix 边界 ----
void test_regc_intrinsic_digit_char(void)
{
    /* ASCII 快路径。 */
    expect(_mj2x_intrinsic_digit_char((uint16_t)'0', 10) == 0);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'9', 10) == 9);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'a', 16) == 10);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'z', 36) == 35);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'Z', 36) == 35);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'g', 16) == -1);
    /* 全角数字（Nd 区段 U+FF10-FF19）：Character.digit('１',10)==1。 */
    expect(_mj2x_intrinsic_digit_char(0xFF11u, 10) == 1);
    expect(_mj2x_intrinsic_digit_char(0xFF19u, 10) == 9);
    expect(_mj2x_intrinsic_digit_char(0xFF10u, 10) == 0);
    /* 阿拉伯印度数字（Nd U+0660-0669）。 */
    expect(_mj2x_intrinsic_digit_char(0x0663u, 10) == 3);
    /* 全角字母别名（radix>10）：Character.digit('Ａ',16)==10（JDK javadoc 明载）。 */
    expect(_mj2x_intrinsic_digit_char(0xFF21u, 16) == 10);
    expect(_mj2x_intrinsic_digit_char(0xFF3Au, 36) == 35);
    expect(_mj2x_intrinsic_digit_char(0xFF41u, 16) == 10);
    expect(_mj2x_intrinsic_digit_char(0xFF21u, 10) == -1);
    /* surrogate 与未收录字符一律 -1。 */
    expect(_mj2x_intrinsic_digit_char(0xD800u, 10) == -1);
    expect(_mj2x_intrinsic_digit_char(0x00BDu, 10) == -1); /* '½' 非 Nd */
    /* radix 边界。 */
    expect(_mj2x_intrinsic_digit_char((uint16_t)'1', 1) == -1);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'1', 37) == -1);
    expect(_mj2x_intrinsic_digit_char((uint16_t)'1', 36) == 1);
}

// ---- parse 族 null/radix/消息文本/全角数字（JDK 17 逐字镜像） ----
void test_regc_intrinsic_parse_semantics(void)
{
    static struct JNINativeInterface_ parse_table;
    static JNIEnv                     parse_env = &parse_table;
    parse_table.GetStringUTFChars     = parse_fake_get_utf_chars;
    parse_table.ReleaseStringUTFChars = parse_fake_release_utf_chars;
    JNIEnv* saved_env                 = _mj2x_regc_env;
    _mj2x_regc_env                    = &parse_env;

    scalar_thrown_exception thrown = {0, NULL, NULL};
    mj2x_regc_context       context;
    mj2x_regc_init_context(&context, &thrown, scalar_record_throw);

    /* null：整型 parse 族按 JDK 17 一律 NFE("Cannot parse null string")，
     * 不再是各方法名 NPE；Float/Double 经 JNI 委派 JDK 不在此列。 */
    expect(_mj2x_intrinsic_parse_int(&context, NULL) == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.class_name, "java/lang/NumberFormatException") == 0);
    expect(strcmp(thrown.message, "Cannot parse null string") == 0);
    expect(_mj2x_intrinsic_parse_long(&context, NULL) == 0);
    expect(thrown.count == 2);
    expect(strcmp(thrown.message, "Cannot parse null string") == 0);
    expect(_mj2x_intrinsic_parse_short(&context, NULL) == 0);
    expect(thrown.count == 3);
    expect(_mj2x_intrinsic_parse_byte(&context, NULL) == 0);
    expect(thrown.count == 4);
    expect(_mj2x_intrinsic_parse_int_radix(&context, NULL, 10) == 0);
    expect(thrown.count == 5);

    /* 基本值回归 + radix 消息文本。 */
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"-2147483648") == INT32_MIN);
    expect(_mj2x_intrinsic_parse_long(&context, (mj2x_ref)"-9223372036854775808") == INT64_MIN);
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"+2147483647") == INT32_MAX);
    expect(thrown.count == 0);
    expect(_mj2x_intrinsic_parse_int_radix(&context, (mj2x_ref)"1", 1) == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.message, "radix 1 less than Character.MIN_RADIX") == 0);
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_long_radix(&context, (mj2x_ref)"1", 37) == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.message, "radix 37 greater than Character.MAX_RADIX") == 0);
    /* 空串/坏字符消息：forInputString 带 radix 后缀（JDK 17 格式）。 */
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"") == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.message, "For input string: \"\"") == 0);
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"abc") == 0);
    expect(thrown.count == 2);
    expect(strcmp(thrown.message, "For input string: \"abc\"") == 0);
    expect(_mj2x_intrinsic_parse_int_radix(&context, (mj2x_ref)"z", 35) == 0);
    expect(thrown.count == 3);
    expect(strcmp(thrown.message, "For input string: \"z\" under radix 35") == 0);
    /* 孤立符号。 */
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"-") == 0);
    expect(thrown.count == 4);
    /* Short/Byte 范围消息。 */
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_short(&context, (mj2x_ref)"99999") == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.message, "Value out of range. Value:\"99999\" Radix:10") == 0);
    expect(_mj2x_intrinsic_parse_byte_radix(&context, (mj2x_ref)"300", 16) == 0);
    expect(thrown.count == 2);
    expect(strcmp(thrown.message, "Value out of range. Value:\"300\" Radix:16") == 0);
    /* parseUnsigned：minus/超界/末位坏 digit 消息。 */
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"-1") == 0);
    expect(thrown.count == 1);
    expect(strcmp(thrown.message, "Illegal leading minus sign on unsigned string -1.") == 0);
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"4294967296") == 0);
    expect(thrown.count == 2);
    expect(strcmp(thrown.message, "String value 4294967296 exceeds range of unsigned int.") == 0);
    expect(_mj2x_intrinsic_parse_unsigned_long(&context, (mj2x_ref)"18446744073709551616") == 0);
    expect(thrown.count == 3);
    expect(strcmp(thrown.message, "String value 18446744073709551616 exceeds range of unsigned long.") == 0);
    /* 拆分路径（串长 > 12，radix 36）：末位坏 digit 单独消息。 */
    expect(_mj2x_intrinsic_parse_unsigned_long_radix(&context, (mj2x_ref)"100000000000z!", 36) == 0);
    expect(thrown.count == 4);
    expect(strcmp(thrown.message, "Bad digit at end of 100000000000z!") == 0);
    /* null -> NFE（非 NPE）。 */
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, NULL) == 0);
    expect(thrown.count == 5);
    expect(strcmp(thrown.message, "Cannot parse null string") == 0);
    expect(_mj2x_intrinsic_parse_unsigned_long(&context, NULL) == 0);
    expect(thrown.count == 6);

    /* 全角数字解析：modified UTF-8 字节直通 fake JNIEnv。 */
    thrown.count = 0;
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93") == 123);
    expect(_mj2x_intrinsic_parse_long(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93") == 123);
    expect(_mj2x_intrinsic_parse_short(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93") == 123);
    expect(thrown.count == 0);
    /* 全角字母别名（radix 16）："１Ａ" -> 0x1A。 */
    expect(_mj2x_intrinsic_parse_int_radix(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\xA1", 16) == 26);
    expect(thrown.count == 0);
    /* 阿拉伯印度数字："١٢٣" -> 123。 */
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"\xD9\xA1\xD9\xA2\xD9\xA3") == 123);
    /* 全角 '+'/'-' 不收：NFE。 */
    expect(_mj2x_intrinsic_parse_int(&context, (mj2x_ref)"\xEF\xBC\x8B\xEF\xBC\x91") == 0);
    expect(thrown.count == 1);
    /* parseUnsigned 全角同路径。 */
    expect(_mj2x_intrinsic_parse_unsigned_int(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93") == 123);
    expect(thrown.count == 1);
    expect(_mj2x_intrinsic_parse_unsigned_long(&context, (mj2x_ref)"\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93") == 123);
    expect(thrown.count == 1);

    _mj2x_regc_env = saved_env;
}

// ---- #1186：DISABLED_ 全量 float 穷举常驻回归 ----
// 命名仿 gtest DISABLED_ 前缀约定：本套驱动器是纯 C main()（非 gtest），以
// main() 的 argv 白名单复刻同语义，日常跑套不调用（套件函数集合与输出与
// 改动前一致），显式传 --gtest_also_run_disabled_tests 时追加执行；
// --gtest_filter=DISABLED_* 仅门面兼容不单独路由（gtest 同语义：单独过滤
// DISABLED_ 也是 0 例）。SkipCSV/统计面属 mj2x_unittest gtest 套，本套无
// 计数面，不存在 DISABLED_ 计数漂移。

// 参考实现：语义定义路线。OpenJDK 17 `Math.round(float)` 实源即生产码移植的
// 位路径版（Math.java 无 floatToIndex 辅助函数，该名出自 JDK-8010430 评审
// 过程稿），照搬同路线会循环论证；此处按 JLS/Javadoc 契约独立推导：
// closest int、tie→+∞、NaN→0、±Inf/出界按 Java cast 语义饱和。float→double
// 恒等加宽后 floor(x+0.5) 的加法在绝大多数域精确（float 有效位 ≤ 24，加半位
// 后 ≤ 32 位 ≤ double 53 位）；仅 |x| 足够小时加法跨 53 位产生失真，但该区域
// floor(x+0.5) 恒为 0、与精确结果一致，全域等价由 DISABLED_ 穷举兜底。
// 与生产码位路径互为独立推导，比对结论才非平凡。
static int32_t round_f32_reference(float value)
{
    if (isnan(value)) return 0;
    if (value >= 2147483648.0f) return INT32_MAX;   /* ≥ 2^31（含 +Inf）饱和 */
    if (value <= -2147483648.0f) return INT32_MIN;  /* ≤ -2^31（含 -Inf）饱和 */
    return (int32_t)floor((double)value + 0.5);
}

void test_regc_intrinsic_DISABLED_round_f32_full_sweep(void)
{
    // 全 2^32 float 位型逐点比对 mj2x_math_round_f32 与 round_f32_reference；
    // 64 块 × 2^26 点，逐块 sum/xor 校验和双侧全等断言 + 失配计数（首例位型
    // 打印后 abort）。评审基线：4,294,967,296 点 0 失配、分块校验和全等
    //（#1144 独立评审复现）；合计校验和随行打印，供跨机复跑比对。
    const uint32_t chunk_shift = 26; /* 64 × 2^26 == 2^32 */
    uint64_t total_impl_sum = 0;
    uint32_t total_impl_xor = 0;
    for (uint32_t chunk = 0; chunk < 64; ++chunk) {
        const uint64_t begin = (uint64_t)chunk << chunk_shift;
        const uint64_t end   = begin + (UINT64_C(1) << chunk_shift);
        uint64_t impl_sum = 0, ref_sum = 0;
        uint32_t impl_xor = 0, ref_xor = 0;
        uint32_t first_bad_bits = 0;
        int32_t  first_bad_impl = 0, first_bad_ref = 0;
        uint32_t bad_count = 0;
        for (uint64_t b = begin; b < end; ++b) {
            const uint32_t bits = (uint32_t)b;
            float   value;
            int32_t impl, ref;
            memcpy(&value, &bits, sizeof value);
            impl = mj2x_math_round_f32(value);
            ref  = round_f32_reference(value);
            impl_sum += (uint32_t)impl;
            ref_sum  += (uint32_t)ref;
            impl_xor ^= (uint32_t)impl;
            ref_xor  ^= (uint32_t)ref;
            if (impl != ref) {
                if (bad_count == 0) {
                    first_bad_bits = bits;
                    first_bad_impl = impl;
                    first_bad_ref  = ref;
                }
                ++bad_count;
            }
        }
        if (bad_count != 0) {
            fprintf(stderr,
                    "round_f32 sweep: chunk %u/64 first mismatch bits=0x%08X "
                    "impl=%d ref=%d (%u mismatches in chunk)\n",
                    (unsigned)chunk, (unsigned)first_bad_bits,
                    (int)first_bad_impl, (int)first_bad_ref,
                    (unsigned)bad_count);
        }
        expect(bad_count == 0);
        expect(impl_sum == ref_sum);
        expect(impl_xor == ref_xor);
        total_impl_sum += impl_sum;
        total_impl_xor ^= impl_xor;
        printf("round_f32 sweep: chunk %2u/64 ok (sum=0x%016llX xor=0x%08X)\n",
               (unsigned)chunk, (unsigned long long)impl_sum,
               (unsigned)impl_xor);
    }
    printf("round_f32 sweep: 4294967296 points, 64 chunks, 0 mismatch, "
           "total sum=0x%016llX xor=0x%08X\n",
           (unsigned long long)total_impl_sum, (unsigned)total_impl_xor);
}

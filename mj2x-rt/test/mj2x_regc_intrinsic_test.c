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

// ---- #1186：DISABLED_ 全量 float 穷举常驻回归 ----
// 命名仿 gtest DISABLED_ 前缀约定：本套驱动器是纯 C main()（非 gtest），以
// main() 的 argv 白名单复刻同语义——日常跑套不调用（套件函数集合与输出与
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
// floor(x+0.5) 恒为 0、与精确结果一致——全域等价由 DISABLED_ 穷举兜底。
// 与生产码位路径互为独立推导，对拍结论才非平凡。
static int32_t round_f32_reference(float value)
{
    if (isnan(value)) return 0;
    if (value >= 2147483648.0f) return INT32_MAX;   /* ≥ 2^31（含 +Inf）饱和 */
    if (value <= -2147483648.0f) return INT32_MIN;  /* ≤ -2^31（含 -Inf）饱和 */
    return (int32_t)floor((double)value + 0.5);
}

void test_regc_intrinsic_DISABLED_round_f32_full_sweep(void)
{
    // 全 2^32 float 位型逐点对拍 mj2x_math_round_f32 vs round_f32_reference；
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

#include "regc/mj2x_regc_intrinsic.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define MJ2X_RANDOM_MULT UINT64_C(0x5DEECE66D)
#define MJ2X_RANDOM_ADD UINT64_C(0xB)
#define MJ2X_RANDOM_MASK ((UINT64_C(1) << 48) - UINT64_C(1))

static uint64_t mj2x_math_random_state =
    (UINT64_C(0x123456789ABCDEF0) ^ MJ2X_RANDOM_MULT) & MJ2X_RANDOM_MASK;

static uint32_t mj2x_math_next_random_bits(int bits)
{
    mj2x_math_random_state =
        (mj2x_math_random_state * MJ2X_RANDOM_MULT + MJ2X_RANDOM_ADD) & MJ2X_RANDOM_MASK;
    return (uint32_t)(mj2x_math_random_state >> (48 - bits));
}

mj2x_int mj2x_int_value_of(int32_t value)
{
    return (mj2x_int){value};
}

int32_t mj2x_int_int_value(mj2x_int box)
{
    return box.value;
}

mj2x_long mj2x_long_value_of(int64_t value)
{
    return (mj2x_long){value};
}

int64_t mj2x_long_long_value(mj2x_long box)
{
    return box.value;
}

mj2x_short mj2x_short_value_of(int16_t value)
{
    return (mj2x_short){value};
}

int16_t mj2x_short_short_value(mj2x_short box)
{
    return box.value;
}

mj2x_byte mj2x_byte_value_of(int8_t value)
{
    return (mj2x_byte){value};
}

int8_t mj2x_byte_byte_value(mj2x_byte box)
{
    return box.value;
}

mj2x_float mj2x_float_value_of(float value)
{
    return (mj2x_float){value};
}

float mj2x_float_float_value(mj2x_float box)
{
    return box.value;
}

mj2x_double mj2x_double_value_of(double value)
{
    return (mj2x_double){value};
}

double mj2x_double_double_value(mj2x_double box)
{
    return box.value;
}

mj2x_bool mj2x_bool_value_of(int8_t value)
{
    return (mj2x_bool){value != 0 ? 1 : 0};
}

int8_t mj2x_bool_bool_value(mj2x_bool box)
{
    return box.value;
}

mj2x_char mj2x_char_value_of(uint16_t value)
{
    return (mj2x_char){value};
}

uint16_t mj2x_char_char_value(mj2x_char box)
{
    return box.value;
}

void mj2x_random_init(mj2x_random* state, int64_t seed)
{
    if (state == NULL) return;
    state->seed = ((uint64_t)seed ^ MJ2X_RANDOM_MULT) & MJ2X_RANDOM_MASK;
}

int32_t mj2x_random_next(mj2x_random* state, int bits)
{
    if (state == NULL || bits <= 0 || bits > 32) return 0;
    state->seed = (state->seed * MJ2X_RANDOM_MULT + MJ2X_RANDOM_ADD) & MJ2X_RANDOM_MASK;
    return (int32_t)(state->seed >> (48 - bits));
}

int32_t mj2x_random_next_int(mj2x_random* state)
{
    return mj2x_random_next(state, 32);
}

int32_t mj2x_random_next_int_bound(mj2x_random* state, int32_t bound)
{
    if (bound <= 0) return INT32_MIN;
    if ((bound & -bound) == bound) {
        const int32_t bits = mj2x_random_next(state, 31);
        return (int32_t)(((int64_t)bound * bits) >> 31);
    }

    for (;;) {
        const int32_t bits  = mj2x_random_next(state, 31);
        const int32_t value = bits % bound;
        if ((int64_t)bits - value + (bound - 1) <= INT32_MAX) return value;
    }
}

int64_t mj2x_random_next_long(mj2x_random* state)
{
    const int32_t high = mj2x_random_next(state, 32);
    const int32_t low  = mj2x_random_next(state, 32);
    return (int64_t)(((uint64_t)(uint32_t)high << 32) + (uint64_t)(int64_t)low);
}

int8_t mj2x_random_next_boolean(mj2x_random* state)
{
    return mj2x_random_next(state, 1) != 0 ? 1 : 0;
}

float mj2x_random_next_float(mj2x_random* state)
{
    return (float)mj2x_random_next(state, 24) * (1.0f / (float)(UINT32_C(1) << 24));
}

double mj2x_random_next_double(mj2x_random* state)
{
    const uint64_t high = (uint32_t)mj2x_random_next(state, 26);
    const uint32_t low  = (uint32_t)mj2x_random_next(state, 27);
    return (double)((high << 27) + low) * (1.0 / (double)(UINT64_C(1) << 53));
}

int32_t mj2x_math_abs_i32(int32_t value)
{
    return value == INT32_MIN ? INT32_MIN : (value < 0 ? -value : value);
}

int64_t mj2x_math_abs_i64(int64_t value)
{
    return value == INT64_MIN ? INT64_MIN : (value < 0 ? -value : value);
}

float mj2x_math_abs_f32(float value)
{
    return fabsf(value);
}

double mj2x_math_abs_f64(double value)
{
    return fabs(value);
}

int32_t mj2x_math_min_i32(int32_t lhs, int32_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

int32_t mj2x_math_max_i32(int32_t lhs, int32_t rhs)
{
    return lhs > rhs ? lhs : rhs;
}

int64_t mj2x_math_min_i64(int64_t lhs, int64_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

int64_t mj2x_math_max_i64(int64_t lhs, int64_t rhs)
{
    return lhs > rhs ? lhs : rhs;
}

// java.lang.Math.max/min float/double 语义（OpenJDK Math.max/min）：
//   1) 任一操作数为 NaN → 返回该 NaN 操作数（保留位型，不用规范 NAN 常量）；
//   2) +0.0/-0.0 相互比较必须返回正确符号：max(+0.0,-0.0)=+0.0、
//      max(-0.0,+0.0)=+0.0、min(+0.0,-0.0)=-0.0、min(-0.0,+0.0)=-0.0——
//      IEEE 754 比较把 ±0.0 视为相等，裸三目会返回错误符号的零
//      （jvmtp 1.7.1 修复③同款缺陷；mtest_set g17.math_nan_zero 按 JVM
//      语义对拍锁定）。
//   实现注意：±0.0 与 NaN 都必须在三元比较前显式分流——MSVC 会把
//   `a >= b ? a : b` 折叠成 maxss/minss 指令，而 SSE 相等（含 ±0.0/NaN
//   tie）时返回第二操作数，恰好复现缺陷。分流后剩余路径上折叠无影响。
float mj2x_math_min_f32(float lhs, float rhs)
{
    if (isnan(lhs)) return lhs;
    if (isnan(rhs)) return rhs;
    if (lhs == 0.0f && rhs == 0.0f) return signbit(rhs) ? rhs : lhs;
    return lhs <= rhs ? lhs : rhs;
}

float mj2x_math_max_f32(float lhs, float rhs)
{
    if (isnan(lhs)) return lhs;
    if (isnan(rhs)) return rhs;
    if (lhs == 0.0f && rhs == 0.0f) return signbit(lhs) ? rhs : lhs;
    return lhs >= rhs ? lhs : rhs;
}

double mj2x_math_min_f64(double lhs, double rhs)
{
    if (isnan(lhs)) return lhs;
    if (isnan(rhs)) return rhs;
    if (lhs == 0.0 && rhs == 0.0) return signbit(rhs) ? rhs : lhs;
    return lhs <= rhs ? lhs : rhs;
}

double mj2x_math_max_f64(double lhs, double rhs)
{
    if (isnan(lhs)) return lhs;
    if (isnan(rhs)) return rhs;
    if (lhs == 0.0 && rhs == 0.0) return signbit(lhs) ? rhs : lhs;
    return lhs >= rhs ? lhs : rhs;
}

double mj2x_math_sin(double value)
{
    return sin(value);
}

double mj2x_math_cos(double value)
{
    return cos(value);
}

double mj2x_math_tan(double value)
{
    return tan(value);
}

double mj2x_math_sqrt(double value)
{
    return sqrt(value);
}

double mj2x_math_pow(double base, double exponent)
{
    return pow(base, exponent);
}

double mj2x_math_floor(double value)
{
    return floor(value);
}

double mj2x_math_ceil(double value)
{
    return ceil(value);
}

int64_t mj2x_math_round(double value)
{
    if (isnan(value)) return 0;
    if (value >= (double)INT64_MAX) return INT64_MAX;
    if (value <= (double)INT64_MIN) return INT64_MIN;
    return (int64_t)floor(value + 0.5);
}

// java.lang.Math.round(float)：JVM 规范公式 (int)floor(a + 0.5f)——加法按
// float 精度执行，NaN→0；溢出侧饱和到 int 边界（C 裸 float→int 转换越界
// 是 UB，必须显式夹紧，与上方 round(double) 的 long 夹紧同口径）。
int32_t mj2x_math_round_f32(float value)
{
    if (isnan(value)) return 0;
    const float shifted = value + 0.5f;
    if (shifted >= 2147483648.0f) return INT32_MAX;
    if (shifted <= -2147483648.0f) return INT32_MIN;
    return (int32_t)floorf(shifted);
}

// java.lang.Math.signum(float/double)：NaN → 同位型 NaN；±0.0 → 同符号零
// （IEEE 比较无法区分 ±0.0，必须按符号位返回原值）；其余按符号返回 ±1.0。
float mj2x_math_signum_f32(float value)
{
    if (isnan(value)) return value;
    if (value == 0.0f) return value;
    return value > 0.0f ? 1.0f : -1.0f;
}

double mj2x_math_signum_f64(double value)
{
    if (isnan(value)) return value;
    if (value == 0.0) return value;
    return value > 0.0 ? 1.0 : -1.0;
}

// java.lang.Math.exp/log/log10：与 sin/cos/tan/pow 同类——JVM 允许 1 ulp 内
// 的半纯（semi-pure）实现，CRT libm 满足该精度档；特殊值（exp(±0/±Inf)、
// log(±0/1/负数/NaN)）由 libm 按 IEEE 语义给出，与 JVM 规范一致。
double mj2x_math_exp(double value)
{
    return exp(value);
}

double mj2x_math_log(double value)
{
    return log(value);
}

double mj2x_math_log10(double value)
{
    return log10(value);
}

double mj2x_math_random(void)
{
    const uint64_t high = mj2x_math_next_random_bits(26);
    const uint64_t low  = mj2x_math_next_random_bits(27);
    return (double)((high << 27) + low) / (double)(UINT64_C(1) << 53);
}

// ---- java.lang.Integer / java.lang.Long 位操作 intrinsic ----
//
// 所有实现与 OpenJDK 语义一致：位移量按 32/64 取模，旋转/反转/radix 输出
// 基于无符号位型，杜绝有符号溢出与 UB 位移。

static uint32_t mj2x_intrinsic_clz32(uint32_t value)
{
    uint32_t n = 0;
    if (value == 0) return 32;
    if ((value & 0xFFFF0000U) == 0) {
        n += 16;
        value <<= 16;
    }
    if ((value & 0xFF000000U) == 0) {
        n += 8;
        value <<= 8;
    }
    if ((value & 0xF0000000U) == 0) {
        n += 4;
        value <<= 4;
    }
    if ((value & 0xC0000000U) == 0) {
        n += 2;
        value <<= 2;
    }
    if ((value & 0x80000000U) == 0) {
        n += 1;
    }
    return n;
}

static uint32_t mj2x_intrinsic_ctz32(uint32_t value)
{
    uint32_t n = 0;
    if (value == 0) return 32;
    if ((value & 0x0000FFFFU) == 0) {
        n += 16;
        value >>= 16;
    }
    if ((value & 0x000000FFU) == 0) {
        n += 8;
        value >>= 8;
    }
    if ((value & 0x0000000FU) == 0) {
        n += 4;
        value >>= 4;
    }
    if ((value & 0x00000003U) == 0) {
        n += 2;
        value >>= 2;
    }
    if ((value & 0x00000001U) == 0) {
        n += 1;
    }
    return n;
}

static uint32_t mj2x_intrinsic_popcount32(uint32_t value)
{
    value = value - ((value >> 1) & 0x55555555U);
    value = (value & 0x33333333U) + ((value >> 2) & 0x33333333U);
    value = (value + (value >> 4)) & 0x0F0F0F0FU;
    return (value * 0x01010101U) >> 24;
}

static uint32_t mj2x_intrinsic_clz64(uint64_t value)
{
    uint32_t n = 0;
    if (value == 0) return 64;
    if ((value & UINT64_C(0xFFFFFFFF00000000)) == 0) {
        n += 32;
        value <<= 32;
    }
    if ((value & UINT64_C(0xFFFF000000000000)) == 0) {
        n += 16;
        value <<= 16;
    }
    if ((value & UINT64_C(0xFF00000000000000)) == 0) {
        n += 8;
        value <<= 8;
    }
    if ((value & UINT64_C(0xF000000000000000)) == 0) {
        n += 4;
        value <<= 4;
    }
    if ((value & UINT64_C(0xC000000000000000)) == 0) {
        n += 2;
        value <<= 2;
    }
    if ((value & UINT64_C(0x8000000000000000)) == 0) {
        n += 1;
    }
    return n;
}

static uint32_t mj2x_intrinsic_ctz64(uint64_t value)
{
    uint32_t n = 0;
    if (value == 0) return 64;
    if ((value & UINT64_C(0x00000000FFFFFFFF)) == 0) {
        n += 32;
        value >>= 32;
    }
    if ((value & UINT64_C(0x000000000000FFFF)) == 0) {
        n += 16;
        value >>= 16;
    }
    if ((value & UINT64_C(0x00000000000000FF)) == 0) {
        n += 8;
        value >>= 8;
    }
    if ((value & UINT64_C(0x000000000000000F)) == 0) {
        n += 4;
        value >>= 4;
    }
    if ((value & UINT64_C(0x0000000000000003)) == 0) {
        n += 2;
        value >>= 2;
    }
    if ((value & UINT64_C(0x0000000000000001)) == 0) {
        n += 1;
    }
    return n;
}

static uint32_t mj2x_intrinsic_popcount64(uint64_t value)
{
    value = value - ((value >> 1) & UINT64_C(0x5555555555555555));
    value = (value & UINT64_C(0x3333333333333333)) +
            ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (uint32_t)((value * UINT64_C(0x0101010101010101)) >> 56);
}

int32_t mj2x_intrinsic_int_rotate_left(int32_t value, int32_t distance)
{
    const uint32_t v = (uint32_t)value;
    const uint32_t d = (uint32_t)distance & 31U;
    if (d == 0) return value;
    return (int32_t)((v << d) | (v >> (32U - d)));
}

int32_t mj2x_intrinsic_int_rotate_right(int32_t value, int32_t distance)
{
    const uint32_t v = (uint32_t)value;
    const uint32_t d = (uint32_t)distance & 31U;
    if (d == 0) return value;
    return (int32_t)((v >> d) | (v << (32U - d)));
}

int32_t mj2x_intrinsic_int_reverse(int32_t value)
{
    uint32_t v = (uint32_t)value;
    v = ((v & 0x55555555U) << 1) | ((v >> 1) & 0x55555555U);
    v = ((v & 0x33333333U) << 2) | ((v >> 2) & 0x33333333U);
    v = ((v & 0x0F0F0F0FU) << 4) | ((v >> 4) & 0x0F0F0F0FU);
    v = ((v & 0x00FF00FFU) << 8) | ((v >> 8) & 0x00FF00FFU);
    v = (v << 16) | (v >> 16);
    return (int32_t)v;
}

int32_t mj2x_intrinsic_int_reverse_bytes(int32_t value)
{
    uint32_t v = (uint32_t)value;
    v = ((v & 0x00FF00FFU) << 8) | ((v >> 8) & 0x00FF00FFU);
    v = (v << 16) | (v >> 16);
    return (int32_t)v;
}

int32_t mj2x_intrinsic_int_bit_count(int32_t value)
{
    return (int32_t)mj2x_intrinsic_popcount32((uint32_t)value);
}

int32_t mj2x_intrinsic_int_leading_zeros(int32_t value)
{
    return (int32_t)mj2x_intrinsic_clz32((uint32_t)value);
}

int32_t mj2x_intrinsic_int_trailing_zeros(int32_t value)
{
    return (int32_t)mj2x_intrinsic_ctz32((uint32_t)value);
}

int32_t mj2x_intrinsic_int_highest_one_bit(int32_t value)
{
    if (value == 0) return 0;
    return (int32_t)(0x80000000U >> mj2x_intrinsic_clz32((uint32_t)value));
}

int32_t mj2x_intrinsic_int_lowest_one_bit(int32_t value)
{
    const uint32_t v = (uint32_t)value;
    return (int32_t)(v & (0U - v));
}

int32_t mj2x_intrinsic_int_signum(int32_t value)
{
    return (value > 0) - (value < 0);
}

int64_t mj2x_intrinsic_long_rotate_left(int64_t value, int32_t distance)
{
    const uint64_t v = (uint64_t)value;
    const uint32_t d = (uint32_t)distance & 63U;
    if (d == 0) return value;
    return (int64_t)((v << d) | (v >> (64U - d)));
}

int64_t mj2x_intrinsic_long_rotate_right(int64_t value, int32_t distance)
{
    const uint64_t v = (uint64_t)value;
    const uint32_t d = (uint32_t)distance & 63U;
    if (d == 0) return value;
    return (int64_t)((v >> d) | (v << (64U - d)));
}

int64_t mj2x_intrinsic_long_reverse(int64_t value)
{
    uint64_t v = (uint64_t)value;
    v = ((v & UINT64_C(0x5555555555555555)) << 1) | ((v >> 1) & UINT64_C(0x5555555555555555));
    v = ((v & UINT64_C(0x3333333333333333)) << 2) | ((v >> 2) & UINT64_C(0x3333333333333333));
    v = ((v & UINT64_C(0x0F0F0F0F0F0F0F0F)) << 4) | ((v >> 4) & UINT64_C(0x0F0F0F0F0F0F0F0F));
    v = ((v & UINT64_C(0x00FF00FF00FF00FF)) << 8) | ((v >> 8) & UINT64_C(0x00FF00FF00FF00FF));
    v = ((v & UINT64_C(0x0000FFFF0000FFFF)) << 16) | ((v >> 16) & UINT64_C(0x0000FFFF0000FFFF));
    v = (v << 32) | (v >> 32);
    return (int64_t)v;
}

int64_t mj2x_intrinsic_long_reverse_bytes(int64_t value)
{
    uint64_t v = (uint64_t)value;
    v = ((v & UINT64_C(0x00FF00FF00FF00FF)) << 8) | ((v >> 8) & UINT64_C(0x00FF00FF00FF00FF));
    v = ((v & UINT64_C(0x0000FFFF0000FFFF)) << 16) | ((v >> 16) & UINT64_C(0x0000FFFF0000FFFF));
    v = (v << 32) | (v >> 32);
    return (int64_t)v;
}

int32_t mj2x_intrinsic_long_bit_count(int64_t value)
{
    return (int32_t)mj2x_intrinsic_popcount64((uint64_t)value);
}

int32_t mj2x_intrinsic_long_leading_zeros(int64_t value)
{
    return (int32_t)mj2x_intrinsic_clz64((uint64_t)value);
}

int32_t mj2x_intrinsic_long_trailing_zeros(int64_t value)
{
    return (int32_t)mj2x_intrinsic_ctz64((uint64_t)value);
}

int64_t mj2x_intrinsic_long_highest_one_bit(int64_t value)
{
    if (value == 0) return 0;
    return (int64_t)(UINT64_C(0x8000000000000000) >> mj2x_intrinsic_clz64((uint64_t)value));
}

int64_t mj2x_intrinsic_long_lowest_one_bit(int64_t value)
{
    const uint64_t v = (uint64_t)value;
    return (int64_t)(v & (UINT64_C(0) - v));
}

int32_t mj2x_intrinsic_long_signum(int64_t value)
{
    return (value > 0) - (value < 0);
}

// ---- 无符号 radix 字符串（Integer/Long.toHexString/toOctalString/toBinaryString）----
//
// Java 语义：无符号位型 + 小写 digits，无前缀、无补零；0 -> "0"。

static mj2x_ref mj2x_intrinsic_unsigned_to_string(mj2x_regc_context* context, uint64_t value,
                                                  uint32_t shift)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL) return NULL;
    static const char kDigits[] = "0123456789abcdef";
    const uint64_t    mask      = (UINT64_C(1) << shift) - UINT64_C(1);
    char              buffer[66];
    int               pos = (int)sizeof(buffer) - 1;
    buffer[pos]           = '\0';
    do {
        buffer[--pos] = kDigits[value & mask];
        value >>= shift;
    } while (value != 0 && pos > 0);
    return (mj2x_ref)(*env)->NewStringUTF(env, buffer + pos);
}

mj2x_ref mj2x_intrinsic_int_to_hex_string(mj2x_regc_context* context, int32_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint32_t)value, 4);
}

mj2x_ref mj2x_intrinsic_int_to_octal_string(mj2x_regc_context* context, int32_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint32_t)value, 3);
}

mj2x_ref mj2x_intrinsic_int_to_binary_string(mj2x_regc_context* context, int32_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint32_t)value, 1);
}

mj2x_ref mj2x_intrinsic_long_to_hex_string(mj2x_regc_context* context, int64_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint64_t)value, 4);
}

mj2x_ref mj2x_intrinsic_long_to_octal_string(mj2x_regc_context* context, int64_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint64_t)value, 3);
}

mj2x_ref mj2x_intrinsic_long_to_binary_string(mj2x_regc_context* context, int64_t value)
{
    return mj2x_intrinsic_unsigned_to_string(context, (uint64_t)value, 1);
}

// ---- java.lang.Enum.name() / ordinal() 直接字段读取 ----

static jfieldID mj2x_intrinsic_enum_name_field(JNIEnv* env)
{
    static jfieldID cached = NULL;
    if (cached != NULL) return cached;
    jclass cls = (*env)->FindClass(env, "java/lang/Enum");
    if (cls == NULL) return NULL;
    cached = (*env)->GetFieldID(env, cls, "name", "Ljava/lang/String;");
    (*env)->DeleteLocalRef(env, cls);
    return cached;
}

static jfieldID mj2x_intrinsic_enum_ordinal_field(JNIEnv* env)
{
    static jfieldID cached = NULL;
    if (cached != NULL) return cached;
    jclass cls = (*env)->FindClass(env, "java/lang/Enum");
    if (cls == NULL) return NULL;
    cached = (*env)->GetFieldID(env, cls, "ordinal", "I");
    (*env)->DeleteLocalRef(env, cls);
    return cached;
}

mj2x_ref mj2x_intrinsic_enum_name(mj2x_regc_context* context, mj2x_ref receiver)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL || receiver == NULL) return NULL;
    const jfieldID field = mj2x_intrinsic_enum_name_field(env);
    if (field == NULL) return NULL;
    return (mj2x_ref)(*env)->GetObjectField(env, (jobject)receiver, field);
}

int32_t mj2x_intrinsic_enum_ordinal(mj2x_regc_context* context, mj2x_ref receiver)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL || receiver == NULL) return 0;
    const jfieldID field = mj2x_intrinsic_enum_ordinal_field(env);
    if (field == NULL) return 0;
    return (*env)->GetIntField(env, (jobject)receiver, field);
}

// ---- getClass().getName().hashCode() 融合（双槽 receiver 身份缓存） ----

#if defined(_WIN32)
#include <intrin.h>
static volatile long g_mj2x_cnh_lock = 0;
static void mj2x_cnh_lock_acquire(void)
{
    while (_InterlockedCompareExchange((volatile long*)&g_mj2x_cnh_lock, 1, 0) != 0) {
    }
}
static void mj2x_cnh_lock_release(void)
{
    _InterlockedExchange((volatile long*)&g_mj2x_cnh_lock, 0);
}
#else
static volatile int g_mj2x_cnh_lock = 0;
static void mj2x_cnh_lock_acquire(void)
{
    while (__sync_val_compare_and_swap(&g_mj2x_cnh_lock, 0, 1) != 0) {
    }
}
static void mj2x_cnh_lock_release(void)
{
    __sync_lock_release(&g_mj2x_cnh_lock);
}
#endif

/// 读写都持锁：无锁读会撞上槽位 {key, hash} 的撕裂更新（key 已换、hash 未换）。
int32_t mj2x_intrinsic_class_name_hash(mj2x_regc_context*             context,
                                       mj2x_ref                        receiver,
                                       mj2x_regc_class_name_hash_site* site)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL || site == NULL) return 0;
    if (receiver == NULL) {
        mj2x_regc_throw_exception(context, "java/lang/NullPointerException", "getClass");
        return 0;
    }
    mj2x_cnh_lock_acquire();
    for (int i = 0; i < 2; ++i) {
        if (site->slots[i].key != NULL &&
            (*env)->IsSameObject(env, site->slots[i].key, (jobject)receiver)) {
            const int32_t hit = site->slots[i].hash;
            mj2x_cnh_lock_release();
            return hit;
        }
    }
    mj2x_cnh_lock_release();

    // miss：经 JDK 计算（getName/hashCode 语义与消息由 JDK 保证），锁内回填。
    static jclass    classClass  = NULL;
    static jclass    stringClass = NULL;
    static jmethodID getName     = NULL;
    static jmethodID hashCode    = NULL;
    if (getName == NULL || hashCode == NULL) {
        jclass clsLocal = (*env)->FindClass(env, "java/lang/Class");
        if (clsLocal == NULL) return 0;
        if (classClass == NULL) classClass = (jclass)(*env)->NewGlobalRef(env, clsLocal);
        getName = (*env)->GetMethodID(env, clsLocal, "getName", "()Ljava/lang/String;");
        (*env)->DeleteLocalRef(env, clsLocal);
        jclass strLocal = (*env)->FindClass(env, "java/lang/String");
        if (strLocal == NULL) return 0;
        if (stringClass == NULL) stringClass = (jclass)(*env)->NewGlobalRef(env, strLocal);
        hashCode = (*env)->GetMethodID(env, strLocal, "hashCode", "()I");
        (*env)->DeleteLocalRef(env, strLocal);
        if (getName == NULL || hashCode == NULL) return 0;
    }
    jclass   cls = (*env)->GetObjectClass(env, (jobject)receiver);
    if (cls == NULL) return 0;
    jstring name = (jstring)(*env)->CallObjectMethodA(env, cls, getName, NULL);
    if (name == NULL) {
        (*env)->DeleteLocalRef(env, cls);
        return 0;   // pending exception 保持
    }
    jint hash = (*env)->CallIntMethodA(env, name, hashCode, NULL);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, name);

    jobject keyGlobal = (*env)->NewGlobalRef(env, (jobject)receiver);
    if (keyGlobal == NULL) return hash;   // OOM：不缓存，直接返回
    mj2x_cnh_lock_acquire();
    const jobject oldKey1 = site->slots[1].key;
    if (site->slots[0].key != NULL) {
        site->slots[1]       = site->slots[0];
    }
    site->slots[0].key  = keyGlobal;
    site->slots[0].hash = (int32_t)hash;
    if (oldKey1 != NULL && oldKey1 != site->slots[1].key) {
        (*env)->DeleteGlobalRef(env, oldKey1);
    }
    mj2x_cnh_lock_release();
    return (int32_t)hash;
}

// ---- javac 合成 values() / valueOf(String) ----

mj2x_ref mj2x_intrinsic_enum_values(mj2x_regc_context* context, mj2x_ref values,
                                    mj2x_regc_enum_values_site* site)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL || values == NULL || site == NULL) return NULL;
    if (site->clone == NULL) {
        jclass        local = (*env)->GetObjectClass(env, (jobject)values);
        if (local == NULL) return NULL;
        jmethodID clone = (*env)->GetMethodID(env, local, "clone", "()Ljava/lang/Object;");
        if (clone == NULL) {
            (*env)->DeleteLocalRef(env, local);
            return NULL;   // pending exception 保持
        }
        jclass global = (jclass)(*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        if (global == NULL) return NULL;
        site->arrayClass = global;
        site->clone      = clone;
    }
    return (mj2x_ref)(*env)->CallObjectMethodA(env, (jobject)values, site->clone, NULL);
}

mj2x_ref mj2x_intrinsic_enum_valueof(mj2x_regc_context* context, mj2x_ref name,
                                     mj2x_regc_enum_valueof_site* site)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL || site == NULL) return NULL;
    if (site->enumValueOf == NULL) {
        jclass local = (*env)->FindClass(env, site->ownerDesc);
        if (local == NULL) return NULL;
        jclass ownerGlobal = (jclass)(*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        if (ownerGlobal == NULL) return NULL;
        jclass enumLocal = (*env)->FindClass(env, "java/lang/Enum");
        if (enumLocal == NULL) return NULL;
        jclass enumGlobal = (jclass)(*env)->NewGlobalRef(env, enumLocal);
        (*env)->DeleteLocalRef(env, enumLocal);
        if (enumGlobal == NULL) return NULL;
        jmethodID valueOf =
            (*env)->GetStaticMethodID(env, enumGlobal, "valueOf",
                                      "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;");
        if (valueOf == NULL) return NULL;
        site->ownerClass = ownerGlobal;
        site->enumClass  = enumGlobal;
        site->enumValueOf = valueOf;
    }
    jvalue args[2];
    args[0].l = (jobject)site->ownerClass;
    args[1].l = (jobject)name;
    return (mj2x_ref)(*env)->CallStaticObjectMethodA(env, site->enumClass, site->enumValueOf,
                                                     args);
}

// ---- java.lang.System.arraycopy 安全子集 ----

/// @brief 回退 JNI System.arraycopy（对象数组 / 类型不兼容 / 不支持情形）。
///
/// 懒缓存 class + methodID；失败（如异常挂起）时保持 pending exception。
static void mj2x_intrinsic_arraycopy_fallback(JNIEnv* env, jobject src, jint srcPos,
                                              jobject dst, jint dstPos, jint len)
{
    static jclass    system_class = NULL;
    static jmethodID arraycopy    = NULL;
    if (system_class == NULL) {
        jclass local = (*env)->FindClass(env, "java/lang/System");
        if (local == NULL) return;
        system_class = (jclass)(*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    if (arraycopy == NULL && system_class != NULL) {
        arraycopy = (*env)->GetStaticMethodID(env, system_class, "arraycopy",
                                              "(Ljava/lang/Object;ILjava/lang/Object;II)V");
    }
    if (arraycopy == NULL) return;
    (*env)->CallStaticVoidMethod(env, system_class, arraycopy, src, srcPos, dst, dstPos, len);
}

/// @brief 8 种 primitive 数组的 IsInstanceOf jclass（按数组描述符索引懒缓存）。
static jclass mj2x_intrinsic_arraycopy_prim_class(JNIEnv* env, int idx)
{
    static const char* kNames[8] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    static jclass      cached[8] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    if (cached[idx] != NULL) return cached[idx];
    jclass local = (*env)->FindClass(env, kNames[idx]);
    if (local == NULL) return NULL;
    cached[idx] = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return cached[idx];
}

void mj2x_intrinsic_arraycopy(mj2x_regc_context* context, mj2x_ref src, int32_t srcPos,
                              mj2x_ref dst, int32_t dstPos, int32_t len)
{
    JNIEnv* env = (JNIEnv*)(context == NULL ? NULL : context->env);
    if (env == NULL) return;
    jobject srcObj = (jobject)src;
    jobject dstObj = (jobject)dst;

    if (srcObj == NULL || dstObj == NULL) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(env, "java/lang/NullPointerException",
                                    (srcObj == NULL) ? "source array is null"
                                                     : "destination array is null");
        }
        return;
    }
    if (len < 0) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(env, "java/lang/ArrayIndexOutOfBoundsException",
                                    "negative arraycopy length");
        }
        return;
    }
    if (srcPos < 0 || dstPos < 0) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(env, "java/lang/ArrayIndexOutOfBoundsException",
                                    "negative arraycopy position");
        }
        return;
    }

    const jsize srcLen = (*env)->GetArrayLength(env, (jarray)srcObj);
    const jsize dstLen = (*env)->GetArrayLength(env, (jarray)dstObj);
    if ((jsize)srcPos > srcLen - (jsize)len || (jsize)dstPos > dstLen - (jsize)len) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(env, "java/lang/ArrayIndexOutOfBoundsException",
                                    "arraycopy range out of bounds");
        }
        return;
    }

    jclass srcClass = (*env)->GetObjectClass(env, srcObj);
    jclass dstClass = (*env)->GetObjectClass(env, dstObj);
    if (srcClass == NULL || dstClass == NULL) return;
    const jboolean sameType = (*env)->IsSameObject(env, srcClass, dstClass);
    (*env)->DeleteLocalRef(env, srcClass);
    (*env)->DeleteLocalRef(env, dstClass);
    if (!sameType) {
        /* 类型不兼容（不同 primitive 类型 / 对象数组子类关系等）→ 回退 JNI 保语义 */
        mj2x_intrinsic_arraycopy_fallback(env, srcObj, (jint)srcPos, dstObj, (jint)dstPos,
                                          (jint)len);
        return;
    }

    /* 同类型数组：先识别 primitive 元素类型，非 primitive（对象数组）回退 JNI
     * （System.arraycopy 自身有对象路径优化）。复制走 Get/Set<Type>ArrayRegion
     * 临时缓冲——重叠安全且不依赖数组内存布局。 */
    int elemKind = -1; /* 0=[Z 1=[B 2=[C 3=[S 4=[I 5=[J 6=[F 7=[D */
    for (int t = 0; t < 8; ++t) {
        jclass c = mj2x_intrinsic_arraycopy_prim_class(env, t);
        if (c == NULL) continue;
        if ((*env)->IsInstanceOf(env, srcObj, c)) {
            elemKind = t;
            break;
        }
    }
    if (elemKind < 0) {
        /* 对象数组（或未知数组类型）→ 回退 JNI */
        mj2x_intrinsic_arraycopy_fallback(env, srcObj, (jint)srcPos, dstObj, (jint)dstPos,
                                          (jint)len);
        return;
    }

    static const int kElemSizes[8] = {1, 1, 2, 2, 4, 8, 4, 8};
    const size_t     bytes        = (size_t)(jsize)len * (size_t)kElemSizes[elemKind];
    void*            tmp          = malloc(bytes > 0 ? bytes : 1);
    if (tmp == NULL) return; /* OOM：保持无副作用 */
    switch (elemKind) {
    case 0: /* boolean[] */
        (*env)->GetBooleanArrayRegion(env, (jbooleanArray)srcObj, (jsize)srcPos, (jsize)len,
                                      (jboolean*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetBooleanArrayRegion(env, (jbooleanArray)dstObj, (jsize)dstPos, (jsize)len,
                                          (jboolean*)tmp);
        break;
    case 1: /* byte[] */
        (*env)->GetByteArrayRegion(env, (jbyteArray)srcObj, (jsize)srcPos, (jsize)len,
                                   (jbyte*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetByteArrayRegion(env, (jbyteArray)dstObj, (jsize)dstPos, (jsize)len,
                                       (jbyte*)tmp);
        break;
    case 2: /* char[] */
        (*env)->GetCharArrayRegion(env, (jcharArray)srcObj, (jsize)srcPos, (jsize)len,
                                   (jchar*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetCharArrayRegion(env, (jcharArray)dstObj, (jsize)dstPos, (jsize)len,
                                       (jchar*)tmp);
        break;
    case 3: /* short[] */
        (*env)->GetShortArrayRegion(env, (jshortArray)srcObj, (jsize)srcPos, (jsize)len,
                                    (jshort*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetShortArrayRegion(env, (jshortArray)dstObj, (jsize)dstPos, (jsize)len,
                                        (jshort*)tmp);
        break;
    case 4: /* int[] */
        (*env)->GetIntArrayRegion(env, (jintArray)srcObj, (jsize)srcPos, (jsize)len, (jint*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetIntArrayRegion(env, (jintArray)dstObj, (jsize)dstPos, (jsize)len,
                                      (jint*)tmp);
        break;
    case 5: /* long[] */
        (*env)->GetLongArrayRegion(env, (jlongArray)srcObj, (jsize)srcPos, (jsize)len,
                                   (jlong*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetLongArrayRegion(env, (jlongArray)dstObj, (jsize)dstPos, (jsize)len,
                                       (jlong*)tmp);
        break;
    case 6: /* float[] */
        (*env)->GetFloatArrayRegion(env, (jfloatArray)srcObj, (jsize)srcPos, (jsize)len,
                                    (jfloat*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetFloatArrayRegion(env, (jfloatArray)dstObj, (jsize)dstPos, (jsize)len,
                                        (jfloat*)tmp);
        break;
    default: /* double[] */
        (*env)->GetDoubleArrayRegion(env, (jdoubleArray)srcObj, (jsize)srcPos, (jsize)len,
                                     (jdouble*)tmp);
        if (!(*env)->ExceptionCheck(env))
            (*env)->SetDoubleArrayRegion(env, (jdoubleArray)dstObj, (jsize)dstPos, (jsize)len,
                                         (jdouble*)tmp);
        break;
    }
    free(tmp);
}

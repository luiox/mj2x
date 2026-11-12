/// @file        mj2x_regc_intrinsic_inline.h
/// @brief       纯标量 intrinsic 的单源实现：生成码 static inline（免跨 TU 调用），
///              mj2x_regc_intrinsic.c 经宏模式同文外联（归档 ABI 保留）。
///
/// @details
/// T1 性能（对标 os30 13-probe）：Arithmetic/Control flow 形态的热循环里，
/// rotateLeft/reverse 等此前每次调用跨编译单元（mj2x-rt 独立 TU、生成工程无
/// LTO）——真函数调用每次 ~2ns，而 HotSpot JIT 把 Integer.rotateLeft 内联成
/// 单条 rol 指令。实现体收敛到本头单源：
///   - 生成 C（include 本头，缺省模式）：static inline，MSVC /O2 原地展开成
///     rol/bswap/popcnt 级指令；
///   - 归档（mj2x_regc_intrinsic.c 定义 MJ2X_INTRINSIC_INLINE_EXTERNAL 后
///     include 本头）：同名函数外部链接定义一次，归档符号保持（老消费方/
///     调试器可见），零漂移。
/// 语义逐位保持 .c 原实现（含 OpenJDK 位型口径与 INT_MIN abs 特例）；
/// float/double min/max（NaN/±0 分流语义敏感）与 libm 族不内联，维持外联。
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(MJ2X_INTRINSIC_INLINE_EXTERNAL)
#define MJ2X_IINLINE
#else
#define MJ2X_IINLINE static inline
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// ---- 位计数辅助（此前 .c 内 static；消费方仅经由下方 intrinsic 触达）----

MJ2X_IINLINE uint32_t mj2x_intrinsic_clz32(uint32_t value)
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

MJ2X_IINLINE uint32_t mj2x_intrinsic_ctz32(uint32_t value)
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

MJ2X_IINLINE uint32_t mj2x_intrinsic_popcount32(uint32_t value)
{
    value = value - ((value >> 1) & 0x55555555U);
    value = (value & 0x33333333U) + ((value >> 2) & 0x33333333U);
    value = (value + (value >> 4)) & 0x0F0F0F0FU;
    return (value * 0x01010101U) >> 24;
}

MJ2X_IINLINE uint32_t mj2x_intrinsic_clz64(uint64_t value)
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
    if ((value & 0x8000000000000000U) == 0) {
        n += 1;
    }
    return n;
}

MJ2X_IINLINE uint32_t mj2x_intrinsic_ctz64(uint64_t value)
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

MJ2X_IINLINE uint32_t mj2x_intrinsic_popcount64(uint64_t value)
{
    value = value - ((value >> 1) & UINT64_C(0x5555555555555555));
    value = (value & UINT64_C(0x3333333333333333)) +
            ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (uint32_t)((value * UINT64_C(0x0101010101010101)) >> 56);
}

// ---- java.lang.Math 整数族 + 浮点 abs ----

MJ2X_IINLINE int32_t mj2x_math_abs_i32(int32_t value)
{
    return value == INT32_MIN ? INT32_MIN : (value < 0 ? -value : value);
}

MJ2X_IINLINE int64_t mj2x_math_abs_i64(int64_t value)
{
    return value == INT64_MIN ? INT64_MIN : (value < 0 ? -value : value);
}

MJ2X_IINLINE float mj2x_math_abs_f32(float value)
{
    return fabsf(value);   // 三目会丢 -0.0 -> +0.0；fabs 位型口径与 JDK 一致
}

MJ2X_IINLINE double mj2x_math_abs_f64(double value)
{
    return fabs(value);
}

MJ2X_IINLINE int32_t mj2x_math_min_i32(int32_t lhs, int32_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

MJ2X_IINLINE int32_t mj2x_math_max_i32(int32_t lhs, int32_t rhs)
{
    return lhs > rhs ? lhs : rhs;
}

MJ2X_IINLINE int64_t mj2x_math_min_i64(int64_t lhs, int64_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

MJ2X_IINLINE int64_t mj2x_math_max_i64(int64_t lhs, int64_t rhs)
{
    return lhs > rhs ? lhs : rhs;
}

// ---- Integer 位操作 ----

MJ2X_IINLINE int32_t mj2x_intrinsic_int_rotate_left(int32_t value, int32_t distance)
{
    const uint32_t v = (uint32_t)value;
    const uint32_t d = (uint32_t)distance & 31U;
    if (d == 0) return value;
    return (int32_t)((v << d) | (v >> (32U - d)));
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_rotate_right(int32_t value, int32_t distance)
{
    const uint32_t v = (uint32_t)value;
    const uint32_t d = (uint32_t)distance & 31U;
    if (d == 0) return value;
    return (int32_t)((v >> d) | (v << (32U - d)));
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_reverse(int32_t value)
{
    uint32_t v = (uint32_t)value;
    v = ((v & 0x55555555U) << 1) | ((v >> 1) & 0x55555555U);
    v = ((v & 0x33333333U) << 2) | ((v >> 2) & 0x33333333U);
    v = ((v & 0x0F0F0F0FU) << 4) | ((v >> 4) & 0x0F0F0F0FU);
    v = ((v & 0x00FF00FFU) << 8) | ((v >> 8) & 0x00FF00FFU);
    v = (v << 16) | (v >> 16);
    return (int32_t)v;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_reverse_bytes(int32_t value)
{
    uint32_t v = (uint32_t)value;
    v = ((v & 0x00FF00FFU) << 8) | ((v >> 8) & 0x00FF00FFU);
    v = (v << 16) | (v >> 16);
    return (int32_t)v;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_bit_count(int32_t value)
{
    return (int32_t)mj2x_intrinsic_popcount32((uint32_t)value);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_leading_zeros(int32_t value)
{
    return (int32_t)mj2x_intrinsic_clz32((uint32_t)value);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_trailing_zeros(int32_t value)
{
    return (int32_t)mj2x_intrinsic_ctz32((uint32_t)value);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_highest_one_bit(int32_t value)
{
    if (value == 0) return 0;
    return (int32_t)(0x80000000U >> mj2x_intrinsic_clz32((uint32_t)value));
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_lowest_one_bit(int32_t value)
{
    const uint32_t v = (uint32_t)value;
    return (int32_t)(v & (0U - v));
}

MJ2X_IINLINE int32_t mj2x_intrinsic_int_signum(int32_t value)
{
    return (value > 0) - (value < 0);
}

// ---- Long 位操作 ----

MJ2X_IINLINE int64_t mj2x_intrinsic_long_rotate_left(int64_t value, int32_t distance)
{
    const uint64_t v = (uint64_t)value;
    const uint32_t d = (uint32_t)distance & 63U;
    if (d == 0) return value;
    return (int64_t)((v << d) | (v >> (64U - d)));
}

MJ2X_IINLINE int64_t mj2x_intrinsic_long_rotate_right(int64_t value, int32_t distance)
{
    const uint64_t v = (uint64_t)value;
    const uint32_t d = (uint32_t)distance & 63U;
    if (d == 0) return value;
    return (int64_t)((v >> d) | (v << (64U - d)));
}

MJ2X_IINLINE int64_t mj2x_intrinsic_long_reverse(int64_t value)
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

MJ2X_IINLINE int64_t mj2x_intrinsic_long_reverse_bytes(int64_t value)
{
    uint64_t v = (uint64_t)value;
    v = ((v & UINT64_C(0x00FF00FF00FF00FF)) << 8) | ((v >> 8) & UINT64_C(0x00FF00FF00FF00FF));
    v = ((v & UINT64_C(0x0000FFFF0000FFFF)) << 16) | ((v >> 16) & UINT64_C(0x0000FFFF0000FFFF));
    v = (v << 32) | (v >> 32);
    return (int64_t)v;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_long_bit_count(int64_t value)
{
    return (int32_t)mj2x_intrinsic_popcount64((uint64_t)value);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_long_leading_zeros(int64_t value)
{
    return (int32_t)mj2x_intrinsic_clz64((uint64_t)value);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_long_trailing_zeros(int64_t value)
{
    return (int32_t)mj2x_intrinsic_ctz64((uint64_t)value);
}

MJ2X_IINLINE int64_t mj2x_intrinsic_long_highest_one_bit(int64_t value)
{
    if (value == 0) return 0;
    return (int64_t)(UINT64_C(0x8000000000000000) >> mj2x_intrinsic_clz64((uint64_t)value));
}

MJ2X_IINLINE int64_t mj2x_intrinsic_long_lowest_one_bit(int64_t value)
{
    const uint64_t v = (uint64_t)value;
    return (int64_t)(v & (UINT64_C(0) - v));
}

MJ2X_IINLINE int32_t mj2x_intrinsic_long_signum(int64_t value)
{
    return (value > 0) - (value < 0);
}

// ---- Float/Double bits 视图（OpenJDK Float/Double 位型口径）----
//
// 位型双关一律 memcpy（严格别名安全，与 mj2x_math_round 的位路径同款）；
// 非 Raw 版把任意 NaN 位型规范化（float → 0x7FC00000、double →
// 0x7FF8000000000000，Infinity 与 ±0.0 位型原样保留），Raw 版逐位保留。

MJ2X_IINLINE int32_t mj2x_intrinsic_float_to_raw_int_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return (int32_t)bits;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_float_to_int_bits(float value)
{
    const uint32_t bits     = (uint32_t)mj2x_intrinsic_float_to_raw_int_bits(value);
    const uint32_t exponent = bits & 0x7F800000U;
    const uint32_t mantissa = bits & 0x007FFFFFU;
    if (exponent == 0x7F800000U && mantissa != 0U) return (int32_t)0x7FC00000U;
    return (int32_t)bits;
}

MJ2X_IINLINE float mj2x_intrinsic_int_bits_to_float(int32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

MJ2X_IINLINE int64_t mj2x_intrinsic_double_to_raw_long_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return (int64_t)bits;
}

MJ2X_IINLINE int64_t mj2x_intrinsic_double_to_long_bits(double value)
{
    const uint64_t bits     = (uint64_t)mj2x_intrinsic_double_to_raw_long_bits(value);
    const uint64_t exponent = (bits >> 52) & UINT64_C(0x7FF);
    const uint64_t mantissa = bits & UINT64_C(0x000FFFFFFFFFFFFF);
    if (exponent == UINT64_C(0x7FF) && mantissa != 0) {
        return (int64_t)UINT64_C(0x7FF8000000000000);
    }
    return (int64_t)bits;
}

MJ2X_IINLINE double mj2x_intrinsic_long_bits_to_double(int64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

// ---- Integer/Long 无符号比较（compareUnsigned）----

MJ2X_IINLINE int32_t mj2x_intrinsic_int_compare_unsigned(int32_t lhs, int32_t rhs)
{
    const uint32_t u = (uint32_t)lhs;
    const uint32_t v = (uint32_t)rhs;
    return u < v ? -1 : (u > v ? 1 : 0);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_long_compare_unsigned(int64_t lhs, int64_t rhs)
{
    const uint64_t u = (uint64_t)lhs;
    const uint64_t v = (uint64_t)rhs;
    return u < v ? -1 : (u > v ? 1 : 0);
}

// ---- Character 静态代理族：纯位计算不抛异常 ----
//
// toCodePoint/highSurrogate/lowSurrogate 对非法输入（非代理对 / 越界 code
// point）的结果按 Javadoc 未定义口径取同款公式值（负 int 域用无符号移位复刻
// Java >>>）。

MJ2X_IINLINE int32_t mj2x_intrinsic_char_to_code_point(uint16_t high, uint16_t low)
{
    // 0x35FDC00 = 0x10000 - (0xD800 << 10) - 0xDC00，三段展开式合一的常量。
    return (((int32_t)high << 10) + (int32_t)low - 0x35FDC00);
}

MJ2X_IINLINE uint16_t mj2x_intrinsic_char_high_surrogate(int32_t code_point)
{
    return (uint16_t)(((uint32_t)code_point >> 10) + 0xD7C0U);
}

MJ2X_IINLINE uint16_t mj2x_intrinsic_char_low_surrogate(int32_t code_point)
{
    return (uint16_t)(((uint32_t)code_point & 0x3FFU) + 0xDC00U);
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_high_surrogate(uint16_t ch)
{
    return (ch >= 0xD800U && ch <= 0xDBFFU) ? 1 : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_low_surrogate(uint16_t ch)
{
    return (ch >= 0xDC00U && ch <= 0xDFFFU) ? 1 : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_surrogate(uint16_t ch)
{
    return (ch >= 0xD800U && ch <= 0xDFFFU) ? 1 : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_surrogate_pair(uint16_t high, uint16_t low)
{
    return (mj2x_intrinsic_char_is_high_surrogate(high) &&
            mj2x_intrinsic_char_is_low_surrogate(low))
               ? 1
               : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_bmp_code_point(int32_t code_point)
{
    return ((uint32_t)code_point >> 16) == 0U ? 1 : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_is_valid_code_point(int32_t code_point)
{
    // OpenJDK 口径：(codePoint >>> 16) < 0x11，即 [0, 0x10FFFF]；负值无符号
    // 右移出高段大数 → 0。
    return ((uint32_t)code_point >> 16) < 0x11U ? 1 : 0;
}

MJ2X_IINLINE int32_t mj2x_intrinsic_char_char_count(int32_t code_point)
{
    return code_point >= 0x10000 ? 2 : 1;
}

#if defined(__cplusplus)
}
#endif

/// @file mj2x_regc_intrinsic.h
/// @brief Distributable C99 ABI for RegC scalar intrinsics.
///
/// @details
/// RegC emits these entry points for Math, Random, and non-escaping primitive
/// wrapper operations. The definitions live in the C99 CRT pack so `mj2x-cli
/// --compile` can link an emitted C translation unit without the in-tree C++
/// runtime library.
#pragma once

#include "regc/mj2x_regc_crt.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mj2x_int
{
    int32_t value;
} mj2x_int;

typedef struct mj2x_long
{
    int64_t value;
} mj2x_long;

typedef struct mj2x_short
{
    int16_t value;
} mj2x_short;

typedef struct mj2x_byte
{
    int8_t value;
} mj2x_byte;

typedef struct mj2x_float
{
    float value;
} mj2x_float;

typedef struct mj2x_double
{
    double value;
} mj2x_double;

typedef struct mj2x_bool
{
    int8_t value;
} mj2x_bool;

typedef struct mj2x_char
{
    uint16_t value;
} mj2x_char;

typedef struct mj2x_random
{
    uint64_t seed;
} mj2x_random;

mj2x_int   mj2x_int_value_of(int32_t value);
int32_t     mj2x_int_int_value(mj2x_int box);
mj2x_long  mj2x_long_value_of(int64_t value);
int64_t     mj2x_long_long_value(mj2x_long box);
mj2x_short mj2x_short_value_of(int16_t value);
int16_t     mj2x_short_short_value(mj2x_short box);
mj2x_byte  mj2x_byte_value_of(int8_t value);
int8_t      mj2x_byte_byte_value(mj2x_byte box);
mj2x_float mj2x_float_value_of(float value);
float       mj2x_float_float_value(mj2x_float box);
mj2x_double mj2x_double_value_of(double value);
double       mj2x_double_double_value(mj2x_double box);
mj2x_bool   mj2x_bool_value_of(int8_t value);
int8_t       mj2x_bool_bool_value(mj2x_bool box);
mj2x_char   mj2x_char_value_of(uint16_t value);
uint16_t     mj2x_char_char_value(mj2x_char box);

void    mj2x_random_init(mj2x_random* state, int64_t seed);
int32_t mj2x_random_next(mj2x_random* state, int bits);
int32_t mj2x_random_next_int(mj2x_random* state);
int32_t mj2x_random_next_int_bound(mj2x_random* state, int32_t bound);
int64_t mj2x_random_next_long(mj2x_random* state);
int8_t  mj2x_random_next_boolean(mj2x_random* state);
float   mj2x_random_next_float(mj2x_random* state);
double  mj2x_random_next_double(mj2x_random* state);

int32_t mj2x_math_abs_i32(int32_t value);
int64_t mj2x_math_abs_i64(int64_t value);
float   mj2x_math_abs_f32(float value);
double  mj2x_math_abs_f64(double value);
int32_t mj2x_math_min_i32(int32_t lhs, int32_t rhs);
int32_t mj2x_math_max_i32(int32_t lhs, int32_t rhs);
int64_t mj2x_math_min_i64(int64_t lhs, int64_t rhs);
int64_t mj2x_math_max_i64(int64_t lhs, int64_t rhs);
float   mj2x_math_min_f32(float lhs, float rhs);
float   mj2x_math_max_f32(float lhs, float rhs);
double  mj2x_math_min_f64(double lhs, double rhs);
double  mj2x_math_max_f64(double lhs, double rhs);
double  mj2x_math_sin(double value);
double  mj2x_math_cos(double value);
double  mj2x_math_tan(double value);
double  mj2x_math_sqrt(double value);
double  mj2x_math_pow(double base, double exponent);
double  mj2x_math_floor(double value);
double  mj2x_math_ceil(double value);
/// java.lang.Math.round(double)：OpenJDK 17 位运算实现（#1144）——最接近的
/// long、tie→+∞；NaN→0，越界饱和到 long 边界。与 HotSpot 边界值逐项对拍。
int64_t mj2x_math_round(double value);
/// java.lang.Math.round(float)：OpenJDK 17 位运算实现（#1144）——最接近的
/// int、tie→+∞；NaN→0，越界饱和到 int 边界。不再用 (int)floor(a+0.5f) 公式
/// （在 0.49999997f 与 [2^23,2^24) 奇整数上与 HotSpot 可观测发散）。
int32_t mj2x_math_round_f32(float value);
/// java.lang.Math.signum(float/double)：NaN/±0.0 按位型原样返回，其余 ±1.0。
float   mj2x_math_signum_f32(float value);
double  mj2x_math_signum_f64(double value);
/// java.lang.Math.exp/log/log10：libm 半纯实现（1 ulp 档，与 pow 同口径）。
double  mj2x_math_exp(double value);
double  mj2x_math_log(double value);
double  mj2x_math_log10(double value);
double  mj2x_math_random(void);

/// @brief java.lang.Integer / java.lang.Long 位操作与无符号 radix 字符串 intrinsic。
///
/// 位操作与 OpenJDK 语义一致（distance 按 32/64 取模、无符号旋转与反转、
/// 符号位扩展仅发生在返回值）；radix 字符串按 Java 语义输出无符号小写
/// （Integer.toHexString(-1) == "ffffffff"，无前缀、无补零）。
int32_t mj2x_intrinsic_int_rotate_left(int32_t value, int32_t distance);
int32_t mj2x_intrinsic_int_rotate_right(int32_t value, int32_t distance);
int32_t mj2x_intrinsic_int_reverse(int32_t value);
int32_t mj2x_intrinsic_int_reverse_bytes(int32_t value);
int32_t mj2x_intrinsic_int_bit_count(int32_t value);
int32_t mj2x_intrinsic_int_leading_zeros(int32_t value);
int32_t mj2x_intrinsic_int_trailing_zeros(int32_t value);
int32_t mj2x_intrinsic_int_highest_one_bit(int32_t value);
int32_t mj2x_intrinsic_int_lowest_one_bit(int32_t value);
int32_t mj2x_intrinsic_int_signum(int32_t value);
int64_t mj2x_intrinsic_long_rotate_left(int64_t value, int32_t distance);
int64_t mj2x_intrinsic_long_rotate_right(int64_t value, int32_t distance);
int64_t mj2x_intrinsic_long_reverse(int64_t value);
int64_t mj2x_intrinsic_long_reverse_bytes(int64_t value);
int32_t mj2x_intrinsic_long_bit_count(int64_t value);
int32_t mj2x_intrinsic_long_leading_zeros(int64_t value);
int32_t mj2x_intrinsic_long_trailing_zeros(int64_t value);
int64_t mj2x_intrinsic_long_highest_one_bit(int64_t value);
int64_t mj2x_intrinsic_long_lowest_one_bit(int64_t value);
int32_t mj2x_intrinsic_long_signum(int64_t value);

/// @brief Float/Double bits 视图（实现为 mj2x_regc_intrinsic_inline.h 的
/// static inline，本头声明归档 ABI 面）。
///
/// 非 Raw 版把任意 NaN 位型规范化（float → 0x7FC00000、double →
/// 0x7FF8000000000000），Raw 版逐位保留；bits→浮点反向直通（NaN 位型不
/// 规范化）。位型双关经 memcpy（严格别名安全）。
int32_t mj2x_intrinsic_float_to_int_bits(float value);
int32_t mj2x_intrinsic_float_to_raw_int_bits(float value);
float   mj2x_intrinsic_int_bits_to_float(int32_t bits);
int64_t mj2x_intrinsic_double_to_long_bits(double value);
int64_t mj2x_intrinsic_double_to_raw_long_bits(double value);
double  mj2x_intrinsic_long_bits_to_double(int64_t bits);

/// @brief Integer/Long.compareUnsigned：无符号序三值比较（-1/0/1）。
int32_t mj2x_intrinsic_int_compare_unsigned(int32_t lhs, int32_t rhs);
int32_t mj2x_intrinsic_long_compare_unsigned(int64_t lhs, int64_t rhs);

/// @brief Character 静态代理族：纯位计算不抛异常。
///
/// 非代理对 / 越界 code point 的结果按 Javadoc 未定义口径取同款公式值；
/// char 形参取 uint16_t 位型。实现为 mj2x_regc_intrinsic_inline.h 的 static inline。
int32_t  mj2x_intrinsic_char_to_code_point(uint16_t high, uint16_t low);
int32_t  mj2x_intrinsic_char_is_surrogate_pair(uint16_t high, uint16_t low);
uint16_t mj2x_intrinsic_char_high_surrogate(int32_t code_point);
uint16_t mj2x_intrinsic_char_low_surrogate(int32_t code_point);
int32_t  mj2x_intrinsic_char_is_high_surrogate(uint16_t ch);
int32_t  mj2x_intrinsic_char_is_low_surrogate(uint16_t ch);
int32_t  mj2x_intrinsic_char_is_surrogate(uint16_t ch);
int32_t  mj2x_intrinsic_char_is_bmp_code_point(int32_t code_point);
int32_t  mj2x_intrinsic_char_is_valid_code_point(int32_t code_point);
int32_t  mj2x_intrinsic_char_char_count(int32_t code_point);

mj2x_ref mj2x_intrinsic_int_to_hex_string(mj2x_regc_context* context, int32_t value);
mj2x_ref mj2x_intrinsic_int_to_octal_string(mj2x_regc_context* context, int32_t value);
mj2x_ref mj2x_intrinsic_int_to_binary_string(mj2x_regc_context* context, int32_t value);
mj2x_ref mj2x_intrinsic_long_to_hex_string(mj2x_regc_context* context, int64_t value);
mj2x_ref mj2x_intrinsic_long_to_octal_string(mj2x_regc_context* context, int64_t value);
mj2x_ref mj2x_intrinsic_long_to_binary_string(mj2x_regc_context* context, int64_t value);

/// @brief Integer/Long.toUnsignedString：无符号位型 radix 字符串（小写、无前缀）。
///
/// 非法 radix（<2 或 >36）不抛异常，静默回落 radix 10（Integer/Long.toString
/// 同款口径）；0 输出 "0"。返回 NewStringUTF 的 local ref。
mj2x_ref mj2x_intrinsic_int_to_unsigned_string(mj2x_regc_context* context, int32_t value);
mj2x_ref mj2x_intrinsic_int_to_unsigned_string_radix(mj2x_regc_context* context, int32_t value,
                                                     int32_t radix);
mj2x_ref mj2x_intrinsic_long_to_unsigned_string(mj2x_regc_context* context, int64_t value);
mj2x_ref mj2x_intrinsic_long_to_unsigned_string_radix(mj2x_regc_context* context, int64_t value,
                                                      int32_t radix);

/// @brief Integer/Long.divideUnsigned/remainderUnsigned：无符号除法/取余。
///
/// 除零挂 pending ArithmeticException("/ by zero") 并返回 0（RegCLocalHelper
/// 形态，调用后 ExceptionCheck 保留）；非零除数为纯无符号运算，无
/// INT_MIN/-1 型陷阱。
int32_t mj2x_intrinsic_int_divide_unsigned(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int32_t mj2x_intrinsic_int_remainder_unsigned(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_intrinsic_long_divide_unsigned(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
int64_t mj2x_intrinsic_long_remainder_unsigned(mj2x_regc_context* context, int64_t lhs, int64_t rhs);

/// @brief java.lang.Math.floorDiv/floorMod：向负无穷取整的除法/取余。
///
/// 除零挂 pending ArithmeticException("/ by zero") 并返回 0（RegCLocalHelper
/// 形态，同 divideUnsigned 口径）；INT_MIN/-1（INT64_MIN/-1）按 JVM 位型
/// 回绕（floorDiv 得 INT_MIN、floorMod 得 0），规避 C 有符号除法未定义行为。
/// 商/余恒等式 floorMod(x,y) == x - floorDiv(x,y)*y 在回绕语义下成立。
int32_t mj2x_intrinsic_math_floor_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_intrinsic_math_floor_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
int32_t mj2x_intrinsic_math_floor_mod_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_intrinsic_math_floor_mod_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);

/// @brief java.lang.Enum.name() / ordinal() 直接字段读取。
///
/// 所有枚举类继承 java/lang/Enum 的 final 字段 name/ordinal，缓存的 jfieldID
/// 对所有枚举实例有效；receiver 为 NULL 时返回 NULL（等价调用路径抛 NPE，
/// 由后续检查兜底）。
mj2x_ref  mj2x_intrinsic_enum_name(mj2x_regc_context* context, mj2x_ref receiver);
int32_t   mj2x_intrinsic_enum_ordinal(mj2x_regc_context* context, mj2x_ref receiver);

/// @brief java.lang.System.arraycopy(Object,int,Object,int,int) 安全子集。
///
/// 覆盖 JVM 语义：null → NPE、len<0 / 越界 → ArrayIndexOutOfBoundsException、
/// 同类型 primitive 数组 → Get/Set<Type>ArrayRegion 复制（重叠安全）；
/// 对象数组、类型不兼容与任何不支持情形回退 JNI System.arraycopy（语义保真）。
void mj2x_intrinsic_arraycopy(mj2x_regc_context* context, mj2x_ref src, int32_t srcPos,
                              mj2x_ref dst, int32_t dstPos, int32_t len);

#ifdef __cplusplus
}
#endif

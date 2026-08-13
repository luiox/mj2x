#include "regc/mj2x_regc_intrinsic.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

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

float mj2x_math_min_f32(float lhs, float rhs)
{
    return isnan(lhs) || isnan(rhs) ? NAN : (lhs < rhs ? lhs : rhs);
}

float mj2x_math_max_f32(float lhs, float rhs)
{
    return isnan(lhs) || isnan(rhs) ? NAN : (lhs > rhs ? lhs : rhs);
}

double mj2x_math_min_f64(double lhs, double rhs)
{
    return isnan(lhs) || isnan(rhs) ? NAN : (lhs < rhs ? lhs : rhs);
}

double mj2x_math_max_f64(double lhs, double rhs)
{
    return isnan(lhs) || isnan(rhs) ? NAN : (lhs > rhs ? lhs : rhs);
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

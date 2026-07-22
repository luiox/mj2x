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

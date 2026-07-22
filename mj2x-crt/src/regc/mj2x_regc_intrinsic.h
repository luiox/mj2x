/// @file mj2x_regc_intrinsic.h
/// @brief Distributable C99 ABI for RegC scalar intrinsics.
///
/// @details
/// RegC emits these entry points for Math, Random, and non-escaping primitive
/// wrapper operations. The definitions live in the C99 CRT pack so `mj2x-cli
/// --compile` can link an emitted C translation unit without the in-tree C++
/// runtime library.
#pragma once

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
int64_t mj2x_math_round(double value);
double  mj2x_math_random(void);

#ifdef __cplusplus
}
#endif

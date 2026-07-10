#pragma once

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* mj2x_ref;

typedef void (*mj2x_regc_throw_callback)(void* env, const char* class_name, const char* message);

/**
 * @brief Result code shared by the distributable RegC runtime ABI.
 *
 * `PENDING_EXCEPTION` means a JNI call has already raised a Java exception;
 * callers must preserve it instead of throwing a second exception.  Other
 * non-zero statuses can be bridged through `mj2x_regc_report_status`.
 */
typedef enum mj2x_regc_status {
    MJ2X_REGC_STATUS_OK = 0,
    MJ2X_REGC_STATUS_INVALID_ARGUMENT = 1,
    MJ2X_REGC_STATUS_OUT_OF_MEMORY = 2,
    MJ2X_REGC_STATUS_PENDING_EXCEPTION = 3,
    MJ2X_REGC_STATUS_INVALID_STATE = 4,
    MJ2X_REGC_STATUS_UNSUPPORTED = 5,
} mj2x_regc_status;

typedef struct mj2x_regc_context {
    void* env;
    mj2x_regc_throw_callback throwException;
} mj2x_regc_context;

typedef struct mj2x_regc_class_cache {
    const char* desc;
    jclass value;
} mj2x_regc_class_cache;

typedef struct mj2x_regc_static_method_cache {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    jmethodID method;
} mj2x_regc_static_method_cache;

typedef struct mj2x_regc_instance_method_cache {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    jmethodID method;
} mj2x_regc_instance_method_cache;

typedef struct mj2x_regc_static_field_cache {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    jfieldID field;
} mj2x_regc_static_field_cache;

typedef struct mj2x_regc_instance_field_cache {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    jfieldID field;
} mj2x_regc_instance_field_cache;

/**
 * @brief Opaque UTF-16 StringBuilder state owned by mj2x-crt.
 *
 * The allocation and representation stay private so a later C++ source pack
 * can replace the implementation without changing generated-C call sites.
 */
typedef struct mj2x_string_builder mj2x_string_builder;

void mj2x_regc_init_context(mj2x_regc_context* context,
                            void* env,
                            mj2x_regc_throw_callback throwException);
void mj2x_regc_set_env(mj2x_regc_context* context, void* env);
void* mj2x_regc_env(const mj2x_regc_context* context);
void mj2x_regc_throw_exception(mj2x_regc_context* context,
                               const char* class_name,
                               const char* message);
void mj2x_regc_report_status(mj2x_regc_context* context,
                             mj2x_regc_status status,
                             const char* operation);
void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message);
int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);

jclass mj2x_regc_cached_class(JNIEnv* env, mj2x_regc_class_cache* cache);
jmethodID mj2x_regc_cached_static_method(JNIEnv* env, mj2x_regc_static_method_cache* cache);
jfieldID mj2x_regc_cached_static_field(JNIEnv* env, mj2x_regc_static_field_cache* cache);

int32_t _mj2x_intrinsic_int_compare(int32_t lhs, int32_t rhs);
int32_t _mj2x_intrinsic_long_compare(int64_t lhs, int64_t rhs);
int32_t _mj2x_intrinsic_byte_compare(int8_t lhs, int8_t rhs);
int32_t _mj2x_intrinsic_short_compare(int16_t lhs, int16_t rhs);
int32_t _mj2x_intrinsic_float_compare(float lhs, float rhs);
int32_t _mj2x_intrinsic_double_compare(double lhs, double rhs);
int32_t _mj2x_intrinsic_bool_compare(int8_t lhs, int8_t rhs);
int32_t _mj2x_intrinsic_char_compare(uint16_t lhs, uint16_t rhs);

mj2x_ref _mj2x_intrinsic_int_to_string(mj2x_regc_context* context, int32_t value);
mj2x_ref _mj2x_intrinsic_long_to_string(mj2x_regc_context* context, int64_t value);
mj2x_ref _mj2x_intrinsic_bool_to_string(mj2x_regc_context* context, int8_t value);
mj2x_ref _mj2x_intrinsic_char_to_string(mj2x_regc_context* context, uint16_t value);

/**
 * @brief Allocate or destroy an opaque StringBuilder state.
 *
 * `init_jstring` follows `StringBuilder(String)` and therefore reports a
 * `NullPointerException` for a null initial value.  `append_jstring`, like
 * Java `append(String)`, appends the literal `"null"` for a null value.
 */
mj2x_regc_status mj2x_string_builder_init(mj2x_regc_context* context,
                                          mj2x_string_builder** out_state);
mj2x_regc_status mj2x_string_builder_init_jstring(mj2x_regc_context* context,
                                                   mj2x_ref initial_value,
                                                   mj2x_string_builder** out_state);
void mj2x_string_builder_destroy(mj2x_string_builder* state);

/**
 * @brief Append Java UTF-16 code units without converting through UTF-8.
 */
mj2x_regc_status mj2x_string_builder_append_utf16(mj2x_regc_context* context,
                                                   mj2x_string_builder* state,
                                                   const uint16_t* units,
                                                   size_t length);
mj2x_regc_status mj2x_string_builder_append_jstring(mj2x_regc_context* context,
                                                     mj2x_string_builder* state,
                                                     mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_object(mj2x_regc_context* context,
                                                    mj2x_string_builder* state,
                                                    mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_i32(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 int32_t value);
mj2x_regc_status mj2x_string_builder_append_i64(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 int64_t value);
mj2x_regc_status mj2x_string_builder_append_bool(mj2x_regc_context* context,
                                                  mj2x_string_builder* state,
                                                  int8_t value);
mj2x_regc_status mj2x_string_builder_append_char(mj2x_regc_context* context,
                                                  mj2x_string_builder* state,
                                                  uint16_t value);
mj2x_regc_status mj2x_string_builder_append_char_array(mj2x_regc_context* context,
                                                        mj2x_string_builder* state,
                                                        mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_f32(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 float value);
mj2x_regc_status mj2x_string_builder_append_f64(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 double value);

/**
 * @brief Materialize a UTF-16 snapshot as a local `java.lang.String` reference.
 */
mj2x_regc_status mj2x_string_builder_to_jstring(mj2x_regc_context* context,
                                                 const mj2x_string_builder* state,
                                                 mj2x_ref* out_jstring);

/**
 * @brief Materialize a UTF-16 snapshot as a local `java.lang.StringBuilder`.
 *
 * This is the escape bridge for later lowering: it returns a real Java object
 * while preserving the opaque native state for the caller to destroy.
 */
mj2x_regc_status mj2x_string_builder_materialize(mj2x_regc_context* context,
                                                  const mj2x_string_builder* state,
                                                  mj2x_ref* out_builder);

/**
 * @brief Return the number of retained UTF-16 code units, or zero for null.
 */
size_t mj2x_string_builder_utf16_length(const mj2x_string_builder* state);

#ifdef __cplusplus
}
#endif

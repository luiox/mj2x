#pragma once

#include <jni.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* mj2x_ref;

typedef void (*mj2x_regc_throw_callback)(void* env, const char* class_name, const char* message);

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

typedef struct mj2x_regc_static_field_cache {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    jfieldID field;
} mj2x_regc_static_field_cache;

void mj2x_regc_init_context(mj2x_regc_context* context,
                            void* env,
                            mj2x_regc_throw_callback throwException);
void mj2x_regc_set_env(mj2x_regc_context* context, void* env);
void* mj2x_regc_env(const mj2x_regc_context* context);
void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message);
int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);

jclass mj2x_regc_cached_class(JNIEnv* env, mj2x_regc_class_cache* cache);
jmethodID mj2x_regc_cached_static_method(JNIEnv* env, mj2x_regc_static_method_cache* cache);
jfieldID mj2x_regc_cached_static_field(JNIEnv* env, mj2x_regc_static_field_cache* cache);

#ifdef __cplusplus
}
#endif

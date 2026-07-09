#include "regc/mj2x_regc_crt.h"

#include <limits.h>
#include <string.h>

static const char* kArithmeticException = "java/lang/ArithmeticException";
static const char* kDivideByZero = "/ by zero";

static void mj2x_regc_throw_exception(mj2x_regc_context* context,
                                      const char* class_name,
                                      const char* message) {
    if (context == NULL || context->throwException == NULL) return;
    context->throwException(context->env, class_name, message);
}

void mj2x_regc_init_context(mj2x_regc_context* context,
                            void* env,
                            mj2x_regc_throw_callback throwException) {
    if (context == NULL) return;
    context->env = env;
    context->throwException = throwException;
}

void mj2x_regc_set_env(mj2x_regc_context* context, void* env) {
    if (context == NULL) return;
    context->env = env;
}

void* mj2x_regc_env(const mj2x_regc_context* context) {
    return context == NULL ? NULL : context->env;
}

void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message) {
    mj2x_regc_throw_exception(context,
                              kArithmeticException,
                              message == NULL ? kDivideByZero : message);
}

int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT32_MIN && rhs == -1) return INT32_MIN;
    return lhs / rhs;
}

int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT64_MIN && rhs == -1) return INT64_MIN;
    return lhs / rhs;
}

int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT32_MIN && rhs == -1) return 0;
    return lhs % rhs;
}

int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT64_MIN && rhs == -1) return 0;
    return lhs % rhs;
}

void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason) {
    (void)context;
    (void)reason;
}

static jclass mj2x_regc_find_class_uncached(JNIEnv* env, const char* desc) {
    if (env == NULL || desc == NULL) return NULL;
    if (desc[0] == 'L') {
        char name[1024];
        int i = 0;
        while (desc[i + 1] != ';' && desc[i + 1] != '\0' && i < 1023) {
            name[i] = desc[i + 1];
            i++;
        }
        name[i] = '\0';
        return (*env)->FindClass(env, name);
    }
    return (*env)->FindClass(env, desc);
}

static jclass mj2x_regc_global_class(JNIEnv* env, const char* desc, jclass* slot) {
    if (env == NULL || slot == NULL) return NULL;
    if (*slot != NULL) return *slot;

    jclass local = mj2x_regc_find_class_uncached(env, desc);
    if (local == NULL) return NULL;
    jclass global = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (global == NULL) return NULL;
    *slot = global;
    return global;
}

jclass mj2x_regc_cached_class(JNIEnv* env, mj2x_regc_class_cache* cache) {
    if (cache == NULL) return NULL;
    return mj2x_regc_global_class(env, cache->desc, &cache->value);
}

jmethodID mj2x_regc_cached_static_method(JNIEnv* env, mj2x_regc_static_method_cache* cache) {
    if (env == NULL || cache == NULL) return NULL;
    if (cache->method != NULL) return cache->method;

    jclass owner = mj2x_regc_global_class(env, cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, owner, cache->name, cache->desc);
    if (method == NULL) return NULL;
    cache->method = method;
    return method;
}

jfieldID mj2x_regc_cached_static_field(JNIEnv* env, mj2x_regc_static_field_cache* cache) {
    if (env == NULL || cache == NULL) return NULL;
    if (cache->field != NULL) return cache->field;

    jclass owner = mj2x_regc_global_class(env, cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jfieldID field = (*env)->GetStaticFieldID(env, owner, cache->name, cache->desc);
    if (field == NULL) return NULL;
    cache->field = field;
    return field;
}

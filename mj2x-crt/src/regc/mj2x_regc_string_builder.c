#ifndef _WIN32
#    ifndef _POSIX_C_SOURCE
#        define _POSIX_C_SOURCE 200809L
#    endif
#endif

#include "regc/mj2x_regc_crt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <pthread.h>
#endif

struct mj2x_string_builder
{
    uint16_t* units;
    size_t    length;
    size_t    capacity;
    int       synchronized;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

static int mj2x_string_builder_lock_init(mj2x_string_builder* state)
{
#ifdef _WIN32
    InitializeCriticalSection(&state->lock);
    return 1;
#else
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) return 0;
    const int recursive_status = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    const int init_status =
        recursive_status == 0 ? pthread_mutex_init(&state->lock, &attributes) : recursive_status;
    (void)pthread_mutexattr_destroy(&attributes);
    return init_status == 0;
#endif
}

static void mj2x_string_builder_lock_destroy(mj2x_string_builder* state)
{
#ifdef _WIN32
    DeleteCriticalSection(&state->lock);
#else
    (void)pthread_mutex_destroy(&state->lock);
#endif
}

static void mj2x_string_builder_lock(mj2x_string_builder* state)
{
    if (state == NULL || !state->synchronized) return;
#ifdef _WIN32
    EnterCriticalSection(&state->lock);
#else
    (void)pthread_mutex_lock(&state->lock);
#endif
}

static void mj2x_string_builder_unlock(mj2x_string_builder* state)
{
    if (state == NULL || !state->synchronized) return;
#ifdef _WIN32
    LeaveCriticalSection(&state->lock);
#else
    (void)pthread_mutex_unlock(&state->lock);
#endif
}

static JNIEnv* mj2x_string_builder_env(mj2x_regc_context* context)
{
    return context == NULL ? NULL : (JNIEnv*)mj2x_regc_env(context);
}

static mj2x_regc_status mj2x_string_builder_invalid_state(mj2x_regc_context* context,
                                                          const char*        operation)
{
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_STATE, operation);
    return MJ2X_REGC_STATUS_INVALID_STATE;
}

static mj2x_regc_status mj2x_string_builder_reserve(mj2x_regc_context*   context,
                                                    mj2x_string_builder* state, size_t extra,
                                                    const char* operation)
{
    const size_t max_size = (size_t)-1;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, operation);
    if (extra > max_size - state->length) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    const size_t required = state->length + extra;
    if (required > (size_t)INT_MAX) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    if (required <= state->capacity) return MJ2X_REGC_STATUS_OK;

    size_t capacity = state->capacity;
    if (capacity > (max_size - 2) / 2) {
        capacity = required;
    }
    else {
        capacity = capacity * 2 + 2;
        if (capacity < required) capacity = required;
    }
    if (capacity > (size_t)INT_MAX) capacity = (size_t)INT_MAX;
    if (capacity > max_size / sizeof(uint16_t)) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    uint16_t* units = (uint16_t*)realloc(state->units, capacity * sizeof(uint16_t));
    if (units == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    state->units    = units;
    state->capacity = capacity;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_allocate(mj2x_regc_context* context,
                                                     size_t initial_capacity, int synchronized,
                                                     const char*           operation,
                                                     mj2x_string_builder** out_state)
{
    if (out_state == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_state = NULL;
    if (initial_capacity > (size_t)INT_MAX || initial_capacity > (size_t)-1 / sizeof(uint16_t)) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    mj2x_string_builder* state = (mj2x_string_builder*)calloc(1, sizeof(*state));
    if (state == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    if (initial_capacity != 0) {
        state->units = (uint16_t*)malloc(initial_capacity * sizeof(uint16_t));
        if (state->units == NULL) {
            free(state);
            mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
            return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
        }
    }
    state->capacity     = initial_capacity;
    state->synchronized = synchronized;
    if (synchronized && !mj2x_string_builder_lock_init(state)) {
        free(state->units);
        free(state);
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    *out_state = state;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_bounds(mj2x_regc_context* context,
                                                   const char*        operation)
{
    mj2x_regc_throw_exception(context, "java/lang/IndexOutOfBoundsException", operation);
    return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
}

static mj2x_regc_status mj2x_string_builder_string_bounds(mj2x_regc_context* context,
                                                          const char*        operation)
{
    mj2x_regc_throw_exception(context, "java/lang/StringIndexOutOfBoundsException", operation);
    return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
}

static mj2x_regc_status mj2x_string_builder_append_ascii(mj2x_regc_context*   context,
                                                         mj2x_string_builder* state,
                                                         const char* text, const char* operation)
{
    if (text == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    const size_t     length = strlen(text);
    mj2x_regc_status status = mj2x_string_builder_reserve(context, state, length, operation);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    for (size_t i = 0; i < length; ++i) {
        state->units[state->length + i] = (uint16_t)(unsigned char)text[i];
    }
    state->length += length;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_pending_or_oom(mj2x_regc_context* context, JNIEnv* env,
                                                           const char* operation)
{
    if (env != NULL && (*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
    return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
}

static mj2x_regc_status mj2x_string_builder_append_value_of(mj2x_regc_context*   context,
                                                            mj2x_string_builder* state,
                                                            const char* desc, jvalue value,
                                                            const char* operation)
{
    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, operation);

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) return mj2x_string_builder_pending_or_oom(context, env, operation);
    jmethodID value_of = (*env)->GetStaticMethodID(env, string_class, "valueOf", desc);
    if (value_of == NULL) {
        (*env)->DeleteLocalRef(env, string_class);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jstring text = (jstring)(*env)->CallStaticObjectMethodA(env, string_class, value_of, &value);
    (*env)->DeleteLocalRef(env, string_class);
    if (text == NULL || (*env)->ExceptionCheck(env)) {
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    mj2x_regc_status status = mj2x_string_builder_append_jstring(context, state, (mj2x_ref)text);
    (*env)->DeleteLocalRef(env, text);
    return status;
}

mj2x_regc_status mj2x_string_builder_init(mj2x_regc_context*    context,
                                          mj2x_string_builder** out_state)
{
    return mj2x_string_builder_allocate(context, 16, 0, "StringBuilder.init", out_state);
}

mj2x_regc_status mj2x_string_builder_init_jstring(mj2x_regc_context*    context,
                                                  mj2x_ref              initial_value,
                                                  mj2x_string_builder** out_state)
{
    if (initial_value == NULL) {
        mj2x_regc_throw_exception(
            context, "java/lang/NullPointerException", "StringBuilder.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.<init>");
    const jsize length = (*env)->GetStringLength(env, (jstring)initial_value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    mj2x_regc_status status = mj2x_string_builder_allocate(
        context, (size_t)length + 16, 0, "StringBuilder.<init>", out_state);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    status = mj2x_string_builder_append_jstring(context, *out_state, initial_value);
    if (status != MJ2X_REGC_STATUS_OK) {
        mj2x_string_builder_destroy(*out_state);
        *out_state = NULL;
    }
    return status;
}

mj2x_regc_status mj2x_string_builder_init_capacity(mj2x_regc_context* context, int32_t capacity,
                                                   mj2x_string_builder** out_state)
{
    if (capacity < 0) {
        mj2x_regc_throw_exception(
            context, "java/lang/NegativeArraySizeException", "StringBuilder.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    return mj2x_string_builder_allocate(
        context, (size_t)capacity, 0, "StringBuilder.<init>", out_state);
}

void mj2x_string_builder_destroy(mj2x_string_builder* state)
{
    if (state == NULL) return;
    if (state->synchronized) mj2x_string_builder_lock_destroy(state);
    free(state->units);
    free(state);
}

mj2x_regc_status mj2x_string_builder_append_utf16(mj2x_regc_context*   context,
                                                  mj2x_string_builder* state, const uint16_t* units,
                                                  size_t length)
{
    if (length == 0) {
        return state == NULL ? mj2x_string_builder_invalid_state(context, "StringBuilder.append")
                             : MJ2X_REGC_STATUS_OK;
    }
    if (units == NULL) {
        mj2x_regc_report_status(
            context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, "StringBuilder.append UTF-16");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    mj2x_regc_status status =
        mj2x_string_builder_reserve(context, state, length, "StringBuilder.append UTF-16");
    if (status != MJ2X_REGC_STATUS_OK) return status;
    memcpy(state->units + state->length, units, length * sizeof(uint16_t));
    state->length += length;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_append_jstring(mj2x_regc_context*   context,
                                                    mj2x_string_builder* state, mj2x_ref value)
{
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    if (value == NULL) {
        return mj2x_string_builder_append_ascii(context, state, "null", "StringBuilder.append");
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    const jsize length = (*env)->GetStringLength(env, (jstring)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    const jchar* chars = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL)
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
    const mj2x_regc_status status =
        mj2x_string_builder_append_utf16(context, state, (const uint16_t*)chars, (size_t)length);
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    return status;
}

mj2x_regc_status mj2x_string_builder_append_object(mj2x_regc_context*   context,
                                                   mj2x_string_builder* state, mj2x_ref value)
{
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    if (value == NULL) {
        return mj2x_string_builder_append_ascii(context, state, "null", "StringBuilder.append");
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    jclass object_class = (*env)->FindClass(env, "java/lang/Object");
    if (object_class == NULL)
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
    jmethodID to_string =
        (*env)->GetMethodID(env, object_class, "toString", "()Ljava/lang/String;");
    if (to_string == NULL) {
        (*env)->DeleteLocalRef(env, object_class);
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
    }
    jstring text = (jstring)(*env)->CallObjectMethod(env, (jobject)value, to_string);
    (*env)->DeleteLocalRef(env, object_class);
    if (text == NULL || (*env)->ExceptionCheck(env)) {
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
    }
    mj2x_regc_status status = mj2x_string_builder_append_jstring(context, state, (mj2x_ref)text);
    (*env)->DeleteLocalRef(env, text);
    return status;
}

static mj2x_regc_status mj2x_string_builder_char_sequence_length(mj2x_regc_context* context,
                                                                 mj2x_ref           value,
                                                                 int32_t*           out_length,
                                                                 const char*        operation)
{
    if (out_length == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_length = 0;
    if (value == NULL) {
        *out_length = 4;
        return MJ2X_REGC_STATUS_OK;
    }
    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, operation);
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) return mj2x_string_builder_pending_or_oom(context, env, operation);
    if ((*env)->IsInstanceOf(env, (jobject)value, string_class)) {
        *out_length = (int32_t)(*env)->GetStringLength(env, (jstring)value);
        (*env)->DeleteLocalRef(env, string_class);
        return (*env)->ExceptionCheck(env) ? MJ2X_REGC_STATUS_PENDING_EXCEPTION
                                           : MJ2X_REGC_STATUS_OK;
    }
    (*env)->DeleteLocalRef(env, string_class);

    jclass value_class = (*env)->GetObjectClass(env, (jobject)value);
    if (value_class == NULL) return mj2x_string_builder_pending_or_oom(context, env, operation);
    jmethodID length_method = (*env)->GetMethodID(env, value_class, "length", "()I");
    if (length_method == NULL) {
        (*env)->DeleteLocalRef(env, value_class);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    const jint length = (*env)->CallIntMethod(env, (jobject)value, length_method);
    (*env)->DeleteLocalRef(env, value_class);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    *out_length = (int32_t)length;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_append_char_sequence_range(mj2x_regc_context*   context,
                                                                mj2x_string_builder* state,
                                                                mj2x_ref value, int32_t start,
                                                                int32_t end)
{
    const char* operation = "StringBuilder.append(CharSequence,int,int)";
    if (state == NULL) return mj2x_string_builder_invalid_state(context, operation);
    int32_t          length = 0;
    mj2x_regc_status status =
        mj2x_string_builder_char_sequence_length(context, value, &length, operation);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    if (start < 0 || end < start || end > length) {
        return mj2x_string_builder_bounds(context, operation);
    }
    const size_t count = (size_t)(end - start);
    if (count == 0) return MJ2X_REGC_STATUS_OK;
    if (value == NULL) {
        static const uint16_t null_units[] = {'n', 'u', 'l', 'l'};
        return mj2x_string_builder_append_utf16(context, state, null_units + start, count);
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, operation);
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) return mj2x_string_builder_pending_or_oom(context, env, operation);
    if ((*env)->IsInstanceOf(env, (jobject)value, string_class)) {
        const jchar* chars = (*env)->GetStringChars(env, (jstring)value, NULL);
        (*env)->DeleteLocalRef(env, string_class);
        if (chars == NULL) return mj2x_string_builder_pending_or_oom(context, env, operation);
        status =
            mj2x_string_builder_append_utf16(context, state, (const uint16_t*)chars + start, count);
        (*env)->ReleaseStringChars(env, (jstring)value, chars);
        return status;
    }
    (*env)->DeleteLocalRef(env, string_class);

    if (count > (size_t)-1 / sizeof(uint16_t)) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    uint16_t* units = (uint16_t*)malloc(count * sizeof(uint16_t));
    if (units == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    jclass value_class = (*env)->GetObjectClass(env, (jobject)value);
    if (value_class == NULL) {
        free(units);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jmethodID char_at = (*env)->GetMethodID(env, value_class, "charAt", "(I)C");
    if (char_at == NULL) {
        (*env)->DeleteLocalRef(env, value_class);
        free(units);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    for (size_t i = 0; i < count; ++i) {
        units[i] = (uint16_t)(*env)->CallCharMethod(
            env, (jobject)value, char_at, (jint)(start + (int32_t)i));
        if ((*env)->ExceptionCheck(env)) {
            (*env)->DeleteLocalRef(env, value_class);
            free(units);
            return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
        }
    }
    (*env)->DeleteLocalRef(env, value_class);
    status = mj2x_string_builder_append_utf16(context, state, units, count);
    free(units);
    return status;
}

mj2x_regc_status mj2x_string_builder_append_char_sequence(mj2x_regc_context*   context,
                                                          mj2x_string_builder* state,
                                                          mj2x_ref             value)
{
    int32_t          length = 0;
    mj2x_regc_status status = mj2x_string_builder_char_sequence_length(
        context, value, &length, "StringBuilder.append(CharSequence)");
    if (status != MJ2X_REGC_STATUS_OK) return status;
    return mj2x_string_builder_append_char_sequence_range(context, state, value, 0, length);
}

mj2x_regc_status mj2x_string_builder_init_char_sequence(mj2x_regc_context*    context,
                                                        mj2x_ref              initial_value,
                                                        mj2x_string_builder** out_state)
{
    if (initial_value == NULL) {
        mj2x_regc_throw_exception(
            context, "java/lang/NullPointerException", "StringBuilder.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    int32_t          length = 0;
    mj2x_regc_status status = mj2x_string_builder_char_sequence_length(
        context, initial_value, &length, "StringBuilder.<init>");
    if (status != MJ2X_REGC_STATUS_OK) return status;
    if (length > INT_MAX - 16) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, "StringBuilder.<init>");
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    status = mj2x_string_builder_allocate(
        context, (size_t)length + 16, 0, "StringBuilder.<init>", out_state);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    status = mj2x_string_builder_append_char_sequence(context, *out_state, initial_value);
    if (status != MJ2X_REGC_STATUS_OK) {
        mj2x_string_builder_destroy(*out_state);
        *out_state = NULL;
    }
    return status;
}

mj2x_regc_status mj2x_string_builder_append_i32(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int32_t value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    return mj2x_string_builder_append_ascii(context, state, text, "StringBuilder.append int");
}

mj2x_regc_status mj2x_string_builder_append_i64(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%lld", (long long)value);
    return mj2x_string_builder_append_ascii(context, state, text, "StringBuilder.append long");
}

mj2x_regc_status mj2x_string_builder_append_bool(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, int8_t value)
{
    return mj2x_string_builder_append_ascii(
        context, state, value == 0 ? "false" : "true", "StringBuilder.append boolean");
}

mj2x_regc_status mj2x_string_builder_append_char(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, uint16_t value)
{
    return mj2x_string_builder_append_utf16(context, state, &value, 1);
}

mj2x_regc_status mj2x_string_builder_append_char_array(mj2x_regc_context*   context,
                                                       mj2x_string_builder* state, mj2x_ref value)
{
    if (state == NULL)
        return mj2x_string_builder_invalid_state(context, "StringBuilder.append char[]");
    if (value == NULL) {
        mj2x_regc_throw_exception(
            context, "java/lang/NullPointerException", "StringBuilder.append(char[])");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL)
        return mj2x_string_builder_invalid_state(context, "StringBuilder.append char[]");
    const jsize length = (*env)->GetArrayLength(env, (jarray)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    jchar chunk[128];
    for (jsize offset = 0; offset < length; offset += (jsize)128) {
        jsize count = length - offset;
        if (count > (jsize)128) count = (jsize)128;
        (*env)->GetCharArrayRegion(env, (jcharArray)value, offset, count, chunk);
        if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
        mj2x_regc_status status =
            mj2x_string_builder_append_utf16(context, state, (const uint16_t*)chunk, (size_t)count);
        if (status != MJ2X_REGC_STATUS_OK) return status;
    }
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_append_char_array_range(mj2x_regc_context*   context,
                                                             mj2x_string_builder* state,
                                                             mj2x_ref value, int32_t offset,
                                                             int32_t length)
{
    const char* operation = "StringBuilder.append(char[],int,int)";
    if (state == NULL) return mj2x_string_builder_invalid_state(context, operation);
    if (value == NULL) {
        mj2x_regc_throw_exception(context, "java/lang/NullPointerException", operation);
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, operation);
    const jsize array_length = (*env)->GetArrayLength(env, (jarray)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    if (offset < 0 || length < 0 || offset > array_length - length) {
        return mj2x_string_builder_bounds(context, operation);
    }
    jchar chunk[128];
    for (int32_t copied = 0; copied < length; copied += 128) {
        int32_t count = length - copied;
        if (count > 128) count = 128;
        (*env)->GetCharArrayRegion(
            env, (jcharArray)value, (jsize)(offset + copied), (jsize)count, chunk);
        if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
        mj2x_regc_status status =
            mj2x_string_builder_append_utf16(context, state, (const uint16_t*)chunk, (size_t)count);
        if (status != MJ2X_REGC_STATUS_OK) return status;
    }
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_append_code_point(mj2x_regc_context*   context,
                                                       mj2x_string_builder* state,
                                                       int32_t              code_point)
{
    if (code_point >= 0 && code_point <= 0xFFFF) {
        return mj2x_string_builder_append_char(context, state, (uint16_t)code_point);
    }
    if (code_point < 0x10000 || code_point > 0x10FFFF) {
        mj2x_regc_throw_exception(
            context, "java/lang/IllegalArgumentException", "StringBuilder.appendCodePoint");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    const uint32_t offset  = (uint32_t)code_point - 0x10000u;
    const uint16_t units[] = {
        (uint16_t)(0xD800u + (offset >> 10)),
        (uint16_t)(0xDC00u + (offset & 0x3FFu)),
    };
    return mj2x_string_builder_append_utf16(context, state, units, 2);
}

mj2x_regc_status mj2x_string_builder_append_f32(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, float value)
{
    jvalue argument;
    argument.f = (jfloat)value;
    return mj2x_string_builder_append_value_of(
        context, state, "(F)Ljava/lang/String;", argument, "StringBuilder.append float");
}

mj2x_regc_status mj2x_string_builder_append_f64(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, double value)
{
    jvalue argument;
    argument.d = (jdouble)value;
    return mj2x_string_builder_append_value_of(
        context, state, "(D)Ljava/lang/String;", argument, "StringBuilder.append double");
}

mj2x_regc_status mj2x_string_builder_to_jstring(mj2x_regc_context*         context,
                                                const mj2x_string_builder* state,
                                                mj2x_ref*                  out_jstring)
{
    if (out_jstring == NULL) {
        mj2x_regc_report_status(
            context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, "StringBuilder.toString");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_jstring = NULL;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.toString");
    if (state->length > (size_t)INT_MAX) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, "StringBuilder.toString");
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.toString");
    const jchar  empty  = 0;
    const jchar* units  = state->length == 0 ? &empty : (const jchar*)state->units;
    jstring      result = (*env)->NewString(env, units, (jsize)state->length);
    if (result == NULL)
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.toString");
    *out_jstring = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_materialize_as(
    mj2x_regc_context* context, const mj2x_string_builder* state, const char* class_name,
    const char* append_desc, const char* operation, mj2x_ref* out_value)
{
    if (out_value == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_value = NULL;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, operation);

    mj2x_ref         string_ref = NULL;
    mj2x_regc_status status     = mj2x_string_builder_to_jstring(context, state, &string_ref);
    if (status != MJ2X_REGC_STATUS_OK) return status;

    JNIEnv* env         = mj2x_string_builder_env(context);
    jclass  value_class = (*env)->FindClass(env, class_name);
    if (value_class == NULL) {
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jmethodID constructor = (*env)->GetMethodID(env, value_class, "<init>", "(I)V");
    if (constructor == NULL) {
        (*env)->DeleteLocalRef(env, value_class);
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jmethodID append = (*env)->GetMethodID(env, value_class, "append", append_desc);
    if (append == NULL) {
        (*env)->DeleteLocalRef(env, value_class);
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jobject result = (*env)->NewObject(env, value_class, constructor, (jint)state->capacity);
    if (result == NULL || (*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, value_class);
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, operation);
    }
    jobject append_result = (*env)->CallObjectMethod(env, result, append, (jstring)string_ref);
    if (append_result != NULL) (*env)->DeleteLocalRef(env, append_result);
    (*env)->DeleteLocalRef(env, value_class);
    (*env)->DeleteLocalRef(env, (jobject)string_ref);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, result);
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    *out_value = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_materialize(mj2x_regc_context*         context,
                                                 const mj2x_string_builder* state,
                                                 mj2x_ref*                  out_builder)
{
    return mj2x_string_builder_materialize_as(context,
                                              state,
                                              "java/lang/StringBuilder",
                                              "(Ljava/lang/String;)Ljava/lang/StringBuilder;",
                                              "StringBuilder.materialize",
                                              out_builder);
}

mj2x_regc_status mj2x_string_builder_length(mj2x_regc_context*         context,
                                            const mj2x_string_builder* state, int32_t* out_length)
{
    if (out_length == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, "StringBuilder.length");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_length = 0;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.length");
    *out_length = (int32_t)state->length;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_capacity(mj2x_regc_context*         context,
                                              const mj2x_string_builder* state,
                                              int32_t*                   out_capacity)
{
    if (out_capacity == NULL) {
        mj2x_regc_report_status(
            context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, "StringBuilder.capacity");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_capacity = 0;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.capacity");
    *out_capacity = (int32_t)state->capacity;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_ensure_capacity(mj2x_regc_context*   context,
                                                     mj2x_string_builder* state,
                                                     int32_t              minimum_capacity)
{
    if (state == NULL) {
        return mj2x_string_builder_invalid_state(context, "StringBuilder.ensureCapacity");
    }
    if (minimum_capacity <= 0 || (size_t)minimum_capacity <= state->capacity) {
        return MJ2X_REGC_STATUS_OK;
    }
    return mj2x_string_builder_reserve(
        context, state, (size_t)minimum_capacity - state->length, "StringBuilder.ensureCapacity");
}

mj2x_regc_status mj2x_string_builder_trim_to_size(mj2x_regc_context*   context,
                                                  mj2x_string_builder* state)
{
    if (state == NULL) {
        return mj2x_string_builder_invalid_state(context, "StringBuilder.trimToSize");
    }
    if (state->length == state->capacity) return MJ2X_REGC_STATUS_OK;
    if (state->length == 0) {
        free(state->units);
        state->units    = NULL;
        state->capacity = 0;
        return MJ2X_REGC_STATUS_OK;
    }
    uint16_t* units = (uint16_t*)realloc(state->units, state->length * sizeof(uint16_t));
    if (units == NULL) {
        mj2x_regc_report_status(
            context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, "StringBuilder.trimToSize");
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    state->units    = units;
    state->capacity = state->length;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_set_length(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int32_t new_length)
{
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.setLength");
    if (new_length < 0)
        return mj2x_string_builder_string_bounds(context, "StringBuilder.setLength");
    const size_t target = (size_t)new_length;
    if (target > state->length) {
        mj2x_regc_status status = mj2x_string_builder_reserve(
            context, state, target - state->length, "StringBuilder.setLength");
        if (status != MJ2X_REGC_STATUS_OK) return status;
        memset(state->units + state->length, 0, (target - state->length) * sizeof(uint16_t));
    }
    state->length = target;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_char_at(mj2x_regc_context*         context,
                                             const mj2x_string_builder* state, int32_t index,
                                             uint16_t* out_char)
{
    if (out_char == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, "StringBuilder.charAt");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_char = 0;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.charAt");
    if (index < 0 || (size_t)index >= state->length) {
        return mj2x_string_builder_string_bounds(context, "StringBuilder.charAt");
    }
    *out_char = state->units[index];
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_set_char_at(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, int32_t index,
                                                 uint16_t value)
{
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.setCharAt");
    if (index < 0 || (size_t)index >= state->length) {
        return mj2x_string_builder_string_bounds(context, "StringBuilder.setCharAt");
    }
    state->units[index] = value;
    return MJ2X_REGC_STATUS_OK;
}

size_t mj2x_string_builder_utf16_length(const mj2x_string_builder* state)
{
    return state == NULL ? 0 : state->length;
}

mj2x_regc_status mj2x_string_buffer_init(mj2x_regc_context* context, mj2x_string_buffer** out_state)
{
    return mj2x_string_builder_allocate(
        context, 16, 1, "StringBuffer.init", (mj2x_string_builder**)out_state);
}

mj2x_regc_status mj2x_string_buffer_init_jstring(mj2x_regc_context* context, mj2x_ref initial_value,
                                                 mj2x_string_buffer** out_state)
{
    if (initial_value == NULL) {
        mj2x_regc_throw_exception(context, "java/lang/NullPointerException", "StringBuffer.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuffer.<init>");
    const jsize length = (*env)->GetStringLength(env, (jstring)initial_value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    mj2x_regc_status status = mj2x_string_builder_allocate(
        context, (size_t)length + 16, 1, "StringBuffer.<init>", (mj2x_string_builder**)out_state);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    status = mj2x_string_builder_append_jstring(
        context, (mj2x_string_builder*)*out_state, initial_value);
    if (status != MJ2X_REGC_STATUS_OK) {
        mj2x_string_buffer_destroy(*out_state);
        *out_state = NULL;
    }
    return status;
}

mj2x_regc_status mj2x_string_buffer_init_capacity(mj2x_regc_context* context, int32_t capacity,
                                                  mj2x_string_buffer** out_state)
{
    if (capacity < 0) {
        mj2x_regc_throw_exception(
            context, "java/lang/NegativeArraySizeException", "StringBuffer.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    return mj2x_string_builder_allocate(
        context, (size_t)capacity, 1, "StringBuffer.<init>", (mj2x_string_builder**)out_state);
}

mj2x_regc_status mj2x_string_buffer_init_char_sequence(mj2x_regc_context*   context,
                                                       mj2x_ref             initial_value,
                                                       mj2x_string_buffer** out_state)
{
    if (initial_value == NULL) {
        mj2x_regc_throw_exception(context, "java/lang/NullPointerException", "StringBuffer.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    int32_t          length = 0;
    mj2x_regc_status status = mj2x_string_builder_char_sequence_length(
        context, initial_value, &length, "StringBuffer.<init>");
    if (status != MJ2X_REGC_STATUS_OK) return status;
    if (length > INT_MAX - 16) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, "StringBuffer.<init>");
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    status = mj2x_string_builder_allocate(
        context, (size_t)length + 16, 1, "StringBuffer.<init>", (mj2x_string_builder**)out_state);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    status = mj2x_string_builder_append_char_sequence(
        context, (mj2x_string_builder*)*out_state, initial_value);
    if (status != MJ2X_REGC_STATUS_OK) {
        mj2x_string_buffer_destroy(*out_state);
        *out_state = NULL;
    }
    return status;
}

void mj2x_string_buffer_destroy(mj2x_string_buffer* state)
{
    mj2x_string_builder_destroy((mj2x_string_builder*)state);
}

#define MJ2X_BUFFER_WRAP_STATUS(name, builder_name, declaration, call_args) \
    mj2x_regc_status name declaration                                       \
    {                                                                       \
        mj2x_string_builder* builder = (mj2x_string_builder*)state;         \
        mj2x_string_builder_lock(builder);                                  \
        mj2x_regc_status status = builder_name call_args;                   \
        mj2x_string_builder_unlock(builder);                                \
        return status;                                                      \
    }

MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_jstring, mj2x_string_builder_append_jstring,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_object, mj2x_string_builder_append_object,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_char_sequence,
                        mj2x_string_builder_append_char_sequence,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_char_sequence_range,
                        mj2x_string_builder_append_char_sequence_range,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value,
                         int32_t start, int32_t end),
                        (context, builder, value, start, end))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_i32, mj2x_string_builder_append_i32,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, int32_t value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_i64, mj2x_string_builder_append_i64,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, int64_t value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_bool, mj2x_string_builder_append_bool,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, int8_t value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_char, mj2x_string_builder_append_char,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, uint16_t value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_char_array, mj2x_string_builder_append_char_array,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_char_array_range,
                        mj2x_string_builder_append_char_array_range,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, mj2x_ref value,
                         int32_t offset, int32_t length),
                        (context, builder, value, offset, length))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_code_point, mj2x_string_builder_append_code_point,
                        (mj2x_regc_context * context, mj2x_string_buffer* state,
                         int32_t code_point),
                        (context, builder, code_point))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_f32, mj2x_string_builder_append_f32,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, float value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_append_f64, mj2x_string_builder_append_f64,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, double value),
                        (context, builder, value))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_to_jstring, mj2x_string_builder_to_jstring,
                        (mj2x_regc_context * context, const mj2x_string_buffer* state,
                         mj2x_ref* out_jstring),
                        (context, builder, out_jstring))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_length, mj2x_string_builder_length,
                        (mj2x_regc_context * context, const mj2x_string_buffer* state,
                         int32_t* out_length),
                        (context, builder, out_length))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_capacity, mj2x_string_builder_capacity,
                        (mj2x_regc_context * context, const mj2x_string_buffer* state,
                         int32_t* out_capacity),
                        (context, builder, out_capacity))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_ensure_capacity, mj2x_string_builder_ensure_capacity,
                        (mj2x_regc_context * context, mj2x_string_buffer* state,
                         int32_t minimum_capacity),
                        (context, builder, minimum_capacity))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_trim_to_size, mj2x_string_builder_trim_to_size,
                        (mj2x_regc_context * context, mj2x_string_buffer* state),
                        (context, builder))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_set_length, mj2x_string_builder_set_length,
                        (mj2x_regc_context * context, mj2x_string_buffer* state,
                         int32_t new_length),
                        (context, builder, new_length))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_char_at, mj2x_string_builder_char_at,
                        (mj2x_regc_context * context, const mj2x_string_buffer* state,
                         int32_t index, uint16_t* out_char),
                        (context, builder, index, out_char))
MJ2X_BUFFER_WRAP_STATUS(mj2x_string_buffer_set_char_at, mj2x_string_builder_set_char_at,
                        (mj2x_regc_context * context, mj2x_string_buffer* state, int32_t index,
                         uint16_t value),
                        (context, builder, index, value))

#undef MJ2X_BUFFER_WRAP_STATUS

mj2x_regc_status mj2x_string_buffer_materialize(mj2x_regc_context*        context,
                                                const mj2x_string_buffer* state,
                                                mj2x_ref*                 out_buffer)
{
    mj2x_string_builder* builder = (mj2x_string_builder*)state;
    mj2x_string_builder_lock(builder);
    mj2x_regc_status status =
        mj2x_string_builder_materialize_as(context,
                                           builder,
                                           "java/lang/StringBuffer",
                                           "(Ljava/lang/String;)Ljava/lang/StringBuffer;",
                                           "StringBuffer.materialize",
                                           out_buffer);
    mj2x_string_builder_unlock(builder);
    return status;
}

size_t mj2x_string_buffer_utf16_length(const mj2x_string_buffer* state)
{
    mj2x_string_builder* builder = (mj2x_string_builder*)state;
    mj2x_string_builder_lock(builder);
    const size_t length = mj2x_string_builder_utf16_length(builder);
    mj2x_string_builder_unlock(builder);
    return length;
}

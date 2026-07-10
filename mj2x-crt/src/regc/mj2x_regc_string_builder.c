#include "regc/mj2x_regc_crt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mj2x_string_builder {
    uint16_t* units;
    size_t length;
    size_t capacity;
};

static JNIEnv* mj2x_string_builder_env(mj2x_regc_context* context) {
    return context == NULL ? NULL : (JNIEnv*)mj2x_regc_env(context);
}

static mj2x_regc_status mj2x_string_builder_invalid_state(mj2x_regc_context* context,
                                                           const char* operation) {
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_STATE, operation);
    return MJ2X_REGC_STATUS_INVALID_STATE;
}

static mj2x_regc_status mj2x_string_builder_reserve(mj2x_regc_context* context,
                                                     mj2x_string_builder* state,
                                                     size_t extra,
                                                     const char* operation) {
    const size_t max_size = (size_t)-1;
    if (state == NULL) return mj2x_string_builder_invalid_state(context, operation);
    if (extra > max_size - state->length) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    const size_t required = state->length + extra;
    if (required <= state->capacity) return MJ2X_REGC_STATUS_OK;

    size_t capacity = state->capacity == 0 ? 16 : state->capacity;
    while (capacity < required) {
        if (capacity > max_size / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    if (capacity > max_size / sizeof(uint16_t)) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    uint16_t* units = (uint16_t*)realloc(state->units, capacity * sizeof(uint16_t));
    if (units == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    state->units = units;
    state->capacity = capacity;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_append_ascii(mj2x_regc_context* context,
                                                          mj2x_string_builder* state,
                                                          const char* text,
                                                          const char* operation) {
    if (text == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    const size_t length = strlen(text);
    mj2x_regc_status status = mj2x_string_builder_reserve(context, state, length, operation);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    for (size_t i = 0; i < length; ++i) {
        state->units[state->length + i] = (uint16_t)(unsigned char)text[i];
    }
    state->length += length;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_builder_pending_or_oom(mj2x_regc_context* context,
                                                            JNIEnv* env,
                                                            const char* operation) {
    if (env != NULL && (*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
    return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
}

static mj2x_regc_status mj2x_string_builder_append_value_of(mj2x_regc_context* context,
                                                             mj2x_string_builder* state,
                                                             const char* desc,
                                                             jvalue value,
                                                             const char* operation) {
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

mj2x_regc_status mj2x_string_builder_init(mj2x_regc_context* context,
                                          mj2x_string_builder** out_state) {
    if (out_state == NULL) {
        mj2x_regc_report_status(context,
                                MJ2X_REGC_STATUS_INVALID_ARGUMENT,
                                "StringBuilder.init");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_state = NULL;

    mj2x_string_builder* state = (mj2x_string_builder*)calloc(1, sizeof(*state));
    if (state == NULL) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, "StringBuilder.init");
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    *out_state = state;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_init_jstring(mj2x_regc_context* context,
                                                   mj2x_ref initial_value,
                                                   mj2x_string_builder** out_state) {
    mj2x_regc_status status = mj2x_string_builder_init(context, out_state);
    if (status != MJ2X_REGC_STATUS_OK) return status;
    if (initial_value == NULL) {
        mj2x_string_builder_destroy(*out_state);
        *out_state = NULL;
        mj2x_regc_throw_exception(context,
                                  "java/lang/NullPointerException",
                                  "StringBuilder.<init>");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }
    status = mj2x_string_builder_append_jstring(context, *out_state, initial_value);
    if (status != MJ2X_REGC_STATUS_OK) {
        mj2x_string_builder_destroy(*out_state);
        *out_state = NULL;
    }
    return status;
}

void mj2x_string_builder_destroy(mj2x_string_builder* state) {
    if (state == NULL) return;
    free(state->units);
    free(state);
}

mj2x_regc_status mj2x_string_builder_append_utf16(mj2x_regc_context* context,
                                                   mj2x_string_builder* state,
                                                   const uint16_t* units,
                                                   size_t length) {
    if (length == 0) {
        return state == NULL ? mj2x_string_builder_invalid_state(context, "StringBuilder.append")
                             : MJ2X_REGC_STATUS_OK;
    }
    if (units == NULL) {
        mj2x_regc_report_status(context,
                                MJ2X_REGC_STATUS_INVALID_ARGUMENT,
                                "StringBuilder.append UTF-16");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    mj2x_regc_status status =
        mj2x_string_builder_reserve(context, state, length, "StringBuilder.append UTF-16");
    if (status != MJ2X_REGC_STATUS_OK) return status;
    memcpy(state->units + state->length, units, length * sizeof(uint16_t));
    state->length += length;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_append_jstring(mj2x_regc_context* context,
                                                     mj2x_string_builder* state,
                                                     mj2x_ref value) {
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    if (value == NULL) {
        return mj2x_string_builder_append_ascii(context, state, "null", "StringBuilder.append");
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    const jsize length = (*env)->GetStringLength(env, (jstring)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    const jchar* chars = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL) return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
    const mj2x_regc_status status =
        mj2x_string_builder_append_utf16(context, state, (const uint16_t*)chars, (size_t)length);
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    return status;
}

mj2x_regc_status mj2x_string_builder_append_object(mj2x_regc_context* context,
                                                    mj2x_string_builder* state,
                                                    mj2x_ref value) {
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    if (value == NULL) {
        return mj2x_string_builder_append_ascii(context, state, "null", "StringBuilder.append");
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append");
    jclass object_class = (*env)->FindClass(env, "java/lang/Object");
    if (object_class == NULL) return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.append");
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

mj2x_regc_status mj2x_string_builder_append_i32(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 int32_t value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    return mj2x_string_builder_append_ascii(context, state, text, "StringBuilder.append int");
}

mj2x_regc_status mj2x_string_builder_append_i64(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 int64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%lld", (long long)value);
    return mj2x_string_builder_append_ascii(context, state, text, "StringBuilder.append long");
}

mj2x_regc_status mj2x_string_builder_append_bool(mj2x_regc_context* context,
                                                  mj2x_string_builder* state,
                                                  int8_t value) {
    return mj2x_string_builder_append_ascii(
        context, state, value == 0 ? "false" : "true", "StringBuilder.append boolean");
}

mj2x_regc_status mj2x_string_builder_append_char(mj2x_regc_context* context,
                                                  mj2x_string_builder* state,
                                                  uint16_t value) {
    return mj2x_string_builder_append_utf16(context, state, &value, 1);
}

mj2x_regc_status mj2x_string_builder_append_char_array(mj2x_regc_context* context,
                                                        mj2x_string_builder* state,
                                                        mj2x_ref value) {
    if (state == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append char[]");
    if (value == NULL) {
        mj2x_regc_throw_exception(context,
                                  "java/lang/NullPointerException",
                                  "StringBuilder.append(char[])");
        return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    }

    JNIEnv* env = mj2x_string_builder_env(context);
    if (env == NULL) return mj2x_string_builder_invalid_state(context, "StringBuilder.append char[]");
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

mj2x_regc_status mj2x_string_builder_append_f32(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 float value) {
    jvalue argument;
    argument.f = (jfloat)value;
    return mj2x_string_builder_append_value_of(
        context, state, "(F)Ljava/lang/String;", argument, "StringBuilder.append float");
}

mj2x_regc_status mj2x_string_builder_append_f64(mj2x_regc_context* context,
                                                 mj2x_string_builder* state,
                                                 double value) {
    jvalue argument;
    argument.d = (jdouble)value;
    return mj2x_string_builder_append_value_of(
        context, state, "(D)Ljava/lang/String;", argument, "StringBuilder.append double");
}

mj2x_regc_status mj2x_string_builder_to_jstring(mj2x_regc_context* context,
                                                 const mj2x_string_builder* state,
                                                 mj2x_ref* out_jstring) {
    if (out_jstring == NULL) {
        mj2x_regc_report_status(context,
                                MJ2X_REGC_STATUS_INVALID_ARGUMENT,
                                "StringBuilder.toString");
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
    const jchar empty = 0;
    const jchar* units = state->length == 0 ? &empty : (const jchar*)state->units;
    jstring result = (*env)->NewString(env, units, (jsize)state->length);
    if (result == NULL) return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.toString");
    *out_jstring = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_builder_materialize(mj2x_regc_context* context,
                                                  const mj2x_string_builder* state,
                                                  mj2x_ref* out_builder) {
    if (out_builder == NULL) {
        mj2x_regc_report_status(context,
                                MJ2X_REGC_STATUS_INVALID_ARGUMENT,
                                "StringBuilder.materialize");
        return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
    }
    *out_builder = NULL;

    mj2x_ref string_ref = NULL;
    mj2x_regc_status status = mj2x_string_builder_to_jstring(context, state, &string_ref);
    if (status != MJ2X_REGC_STATUS_OK) return status;

    JNIEnv* env = mj2x_string_builder_env(context);
    jclass builder_class = (*env)->FindClass(env, "java/lang/StringBuilder");
    if (builder_class == NULL) {
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.materialize");
    }
    jmethodID constructor =
        (*env)->GetMethodID(env, builder_class, "<init>", "(Ljava/lang/String;)V");
    if (constructor == NULL) {
        (*env)->DeleteLocalRef(env, builder_class);
        (*env)->DeleteLocalRef(env, (jobject)string_ref);
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.materialize");
    }
    jobject result = (*env)->NewObject(env, builder_class, constructor, (jstring)string_ref);
    (*env)->DeleteLocalRef(env, builder_class);
    (*env)->DeleteLocalRef(env, (jobject)string_ref);
    if (result == NULL || (*env)->ExceptionCheck(env)) {
        return mj2x_string_builder_pending_or_oom(context, env, "StringBuilder.materialize");
    }
    *out_builder = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

size_t mj2x_string_builder_utf16_length(const mj2x_string_builder* state) {
    return state == NULL ? 0 : state->length;
}

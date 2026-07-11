#include "regc/mj2x_regc_crt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static JNIEnv* mj2x_string_env(mj2x_regc_context* context)
{
    return context == NULL ? NULL : (JNIEnv*)mj2x_regc_env(context);
}

static mj2x_regc_status mj2x_string_invalid_state(mj2x_regc_context* context, const char* operation)
{
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_STATE, operation);
    return MJ2X_REGC_STATUS_INVALID_STATE;
}

static mj2x_regc_status mj2x_string_invalid_argument(mj2x_regc_context* context,
                                                     const char*        operation)
{
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_INVALID_ARGUMENT, operation);
    return MJ2X_REGC_STATUS_INVALID_ARGUMENT;
}

static mj2x_regc_status mj2x_string_pending_or_oom(mj2x_regc_context* context, JNIEnv* env,
                                                   const char* operation)
{
    if (env != NULL && (*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
    return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
}

static mj2x_regc_status mj2x_string_null(mj2x_regc_context* context, const char* operation)
{
    mj2x_regc_throw_exception(context, "java/lang/NullPointerException", operation);
    return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
}

static mj2x_regc_status mj2x_string_bounds(mj2x_regc_context* context, const char* operation)
{
    mj2x_regc_throw_exception(context, "java/lang/StringIndexOutOfBoundsException", operation);
    return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
}

static mj2x_regc_status mj2x_string_copy_ref(mj2x_regc_context* context, mj2x_ref value,
                                             mj2x_ref* out_string, const char* operation)
{
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    jobject result = (*env)->NewLocalRef(env, (jobject)value);
    if (result == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    *out_string = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

static int mj2x_string_is_literal_split_pattern(const jchar* pattern, jsize length)
{
    if (pattern == NULL || length == 0) return 0;
    for (jsize i = 0; i < length; ++i) {
        switch (pattern[i]) {
        case '\\':
        case '.':
        case '^':
        case '$':
        case '|':
        case '?':
        case '*':
        case '+':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}': return 0;
        default: break;
        }
    }
    return 1;
}

static mj2x_regc_status mj2x_string_split_jdk(mj2x_regc_context* context, mj2x_ref value,
                                              mj2x_ref regex, int32_t limit, int has_limit,
                                              mj2x_ref* out_array)
{
    const char* operation = "String.split";
    JNIEnv*     env       = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    const char* desc  = has_limit ? "(Ljava/lang/String;I)[Ljava/lang/String;"
                                  : "(Ljava/lang/String;)[Ljava/lang/String;";
    jmethodID   split = (*env)->GetMethodID(env, string_class, "split", desc);
    if (split == NULL) {
        (*env)->DeleteLocalRef(env, string_class);
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    jobject result =
        has_limit
            ? (*env)->CallObjectMethod(env, (jobject)value, split, (jstring)regex, (jint)limit)
            : (*env)->CallObjectMethod(env, (jobject)value, split, (jstring)regex);
    (*env)->DeleteLocalRef(env, string_class);
    if (result == NULL || (*env)->ExceptionCheck(env)) {
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    *out_array = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_split_literal(mj2x_regc_context* context, mj2x_ref value,
                                                  const jchar* delimiter, jsize delimiter_length,
                                                  int32_t limit, mj2x_ref* out_array)
{
    const char* operation    = "String.split";
    JNIEnv*     env          = mj2x_string_env(context);
    const jsize value_length = (*env)->GetStringLength(env, (jstring)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    const jchar* value_chars = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (value_chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);

    const size_t max_parts = (size_t)value_length + 1;
    int32_t*     starts    = (int32_t*)malloc(max_parts * sizeof(int32_t));
    int32_t*     lengths   = (int32_t*)malloc(max_parts * sizeof(int32_t));
    if (starts == NULL || lengths == NULL) {
        free(lengths);
        free(starts);
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    size_t        part_count    = 0;
    int32_t       segment_start = 0;
    int32_t       search        = 0;
    int32_t       match_count   = 0;
    const int32_t max_splits    = limit > 0 ? limit - 1 : INT_MAX;
    while (match_count < max_splits &&
           search <= (int32_t)value_length - (int32_t)delimiter_length) {
        if (memcmp(value_chars + search, delimiter, (size_t)delimiter_length * sizeof(jchar)) ==
            0) {
            starts[part_count]  = segment_start;
            lengths[part_count] = search - segment_start;
            ++part_count;
            ++match_count;
            search += (int32_t)delimiter_length;
            segment_start = search;
        }
        else {
            ++search;
        }
    }
    starts[part_count]  = segment_start;
    lengths[part_count] = (int32_t)value_length - segment_start;
    ++part_count;
    if (limit == 0 && match_count > 0) {
        while (part_count > 0 && lengths[part_count - 1] == 0) --part_count;
    }

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) {
        free(lengths);
        free(starts);
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    jobjectArray result = (*env)->NewObjectArray(env, (jsize)part_count, string_class, NULL);
    (*env)->DeleteLocalRef(env, string_class);
    if (result == NULL) {
        free(lengths);
        free(starts);
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    const jchar empty = 0;
    for (size_t i = 0; i < part_count; ++i) {
        const jchar* begin = lengths[i] == 0 ? &empty : value_chars + starts[i];
        jstring      item  = (*env)->NewString(env, begin, (jsize)lengths[i]);
        if (item == NULL) {
            (*env)->DeleteLocalRef(env, result);
            free(lengths);
            free(starts);
            (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
            return mj2x_string_pending_or_oom(context, env, operation);
        }
        (*env)->SetObjectArrayElement(env, result, (jsize)i, item);
        (*env)->DeleteLocalRef(env, item);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->DeleteLocalRef(env, result);
            free(lengths);
            free(starts);
            (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
            return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
        }
    }
    free(lengths);
    free(starts);
    (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
    *out_array = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

static mj2x_regc_status mj2x_string_split_impl(mj2x_regc_context* context, mj2x_ref value,
                                               mj2x_ref regex, int32_t limit, int has_limit,
                                               mj2x_ref* out_array)
{
    const char* operation = "String.split";
    if (out_array == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_array = NULL;
    if (value == NULL || regex == NULL) return mj2x_string_null(context, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize regex_length = (*env)->GetStringLength(env, (jstring)regex);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    const jchar* regex_chars = (*env)->GetStringChars(env, (jstring)regex, NULL);
    if (regex_chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    if (!mj2x_string_is_literal_split_pattern(regex_chars, regex_length)) {
        (*env)->ReleaseStringChars(env, (jstring)regex, regex_chars);
        return mj2x_string_split_jdk(context, value, regex, limit, has_limit, out_array);
    }
    mj2x_regc_status status =
        mj2x_string_split_literal(context, value, regex_chars, regex_length, limit, out_array);
    (*env)->ReleaseStringChars(env, (jstring)regex, regex_chars);
    return status;
}

mj2x_regc_status mj2x_string_split(mj2x_regc_context* context, mj2x_ref value, mj2x_ref regex,
                                   mj2x_ref* out_array)
{
    return mj2x_string_split_impl(context, value, regex, 0, 0, out_array);
}

mj2x_regc_status mj2x_string_split_limit(mj2x_regc_context* context, mj2x_ref value, mj2x_ref regex,
                                         int32_t limit, mj2x_ref* out_array)
{
    return mj2x_string_split_impl(context, value, regex, limit, 1, out_array);
}

static mj2x_regc_status mj2x_string_substring_impl(mj2x_regc_context* context, mj2x_ref value,
                                                   int32_t begin_index, int32_t end_index,
                                                   mj2x_ref* out_string, const char* operation)
{
    if (out_string == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_string = NULL;
    if (value == NULL) return mj2x_string_null(context, operation);

    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize length = (*env)->GetStringLength(env, (jstring)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    if (begin_index < 0 || end_index > length || begin_index > end_index) {
        return mj2x_string_bounds(context, operation);
    }
    if (begin_index == 0 && end_index == length) {
        return mj2x_string_copy_ref(context, value, out_string, operation);
    }

    const jchar* chars = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    const jsize  result_length = (jsize)(end_index - begin_index);
    const jchar  empty         = 0;
    const jchar* begin         = result_length == 0 ? &empty : chars + begin_index;
    jstring      result        = (*env)->NewString(env, begin, result_length);
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    if (result == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    *out_string = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_concat(mj2x_regc_context* context, mj2x_ref value, mj2x_ref suffix,
                                    mj2x_ref* out_string)
{
    const char* operation = "String.concat";
    if (out_string == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_string = NULL;
    if (value == NULL || suffix == NULL) return mj2x_string_null(context, operation);

    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize value_length  = (*env)->GetStringLength(env, (jstring)value);
    const jsize suffix_length = (*env)->GetStringLength(env, (jstring)suffix);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    if (suffix_length == 0) return mj2x_string_copy_ref(context, value, out_string, operation);
    if (value_length > INT_MAX - suffix_length) {
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }

    const jchar* value_chars = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (value_chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    const jchar* suffix_chars = (*env)->GetStringChars(env, (jstring)suffix, NULL);
    if (suffix_chars == NULL) {
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        return mj2x_string_pending_or_oom(context, env, operation);
    }

    const size_t result_length = (size_t)value_length + (size_t)suffix_length;
    jchar*       units         = (jchar*)malloc(result_length * sizeof(jchar));
    if (units == NULL) {
        (*env)->ReleaseStringChars(env, (jstring)suffix, suffix_chars);
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
        return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
    }
    memcpy(units, value_chars, (size_t)value_length * sizeof(jchar));
    memcpy(units + value_length, suffix_chars, (size_t)suffix_length * sizeof(jchar));
    (*env)->ReleaseStringChars(env, (jstring)suffix, suffix_chars);
    (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
    jstring result = (*env)->NewString(env, units, (jsize)result_length);
    free(units);
    if (result == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    *out_string = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_substring_from(mj2x_regc_context* context, mj2x_ref value,
                                            int32_t begin_index, mj2x_ref* out_string)
{
    if (value == NULL) return mj2x_string_null(context, "String.substring");
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, "String.substring");
    const jsize length = (*env)->GetStringLength(env, (jstring)value);
    if ((*env)->ExceptionCheck(env)) return MJ2X_REGC_STATUS_PENDING_EXCEPTION;
    return mj2x_string_substring_impl(
        context, value, begin_index, length, out_string, "String.substring");
}

mj2x_regc_status mj2x_string_substring_range(mj2x_regc_context* context, mj2x_ref value,
                                             int32_t begin_index, int32_t end_index,
                                             mj2x_ref* out_string)
{
    return mj2x_string_substring_impl(
        context, value, begin_index, end_index, out_string, "String.substring");
}

mj2x_regc_status mj2x_string_compare_to(mj2x_regc_context* context, mj2x_ref value, mj2x_ref other,
                                        int32_t* out_result)
{
    const char* operation = "String.compareTo";
    if (out_result == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_result = 0;
    if (value == NULL || other == NULL) return mj2x_string_null(context, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize  value_length = (*env)->GetStringLength(env, (jstring)value);
    const jsize  other_length = (*env)->GetStringLength(env, (jstring)other);
    const jchar* value_chars  = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (value_chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    const jchar* other_chars = (*env)->GetStringChars(env, (jstring)other, NULL);
    if (other_chars == NULL) {
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    const jsize common = value_length < other_length ? value_length : other_length;
    for (jsize i = 0; i < common; ++i) {
        if (value_chars[i] != other_chars[i]) {
            *out_result = (int32_t)value_chars[i] - (int32_t)other_chars[i];
            break;
        }
    }
    if (*out_result == 0) *out_result = (int32_t)value_length - (int32_t)other_length;
    (*env)->ReleaseStringChars(env, (jstring)other, other_chars);
    (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
    return MJ2X_REGC_STATUS_OK;
}

static int32_t mj2x_string_index_of_units(const jchar* value, jsize value_length,
                                          const jchar* needle, jsize needle_length,
                                          int32_t from_index)
{
    int32_t from = from_index < 0 ? 0 : from_index;
    if (needle_length == 0) return from > value_length ? value_length : from;
    if (from > value_length - needle_length) return -1;
    for (jsize i = (jsize)from; i <= value_length - needle_length; ++i) {
        if (value[i] == needle[0] &&
            memcmp(value + i, needle, (size_t)needle_length * sizeof(jchar)) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

mj2x_regc_status mj2x_string_index_of_char_from(mj2x_regc_context* context, mj2x_ref value,
                                                int32_t code_point, int32_t from_index,
                                                int32_t* out_index)
{
    const char* operation = "String.indexOf";
    if (out_index == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_index = -1;
    if (value == NULL) return mj2x_string_null(context, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize  length = (*env)->GetStringLength(env, (jstring)value);
    const jchar* chars  = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    jchar needle[2];
    jsize needle_length = 0;
    if (code_point >= 0 && code_point <= 0xFFFF) {
        needle[0]     = (jchar)code_point;
        needle_length = 1;
    }
    else if (code_point >= 0x10000 && code_point <= 0x10FFFF) {
        const uint32_t offset = (uint32_t)code_point - 0x10000u;
        needle[0]             = (jchar)(0xD800u + (offset >> 10));
        needle[1]             = (jchar)(0xDC00u + (offset & 0x3FFu));
        needle_length         = 2;
    }
    if (needle_length != 0) {
        *out_index = mj2x_string_index_of_units(chars, length, needle, needle_length, from_index);
    }
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_index_of_char(mj2x_regc_context* context, mj2x_ref value,
                                           int32_t code_point, int32_t* out_index)
{
    return mj2x_string_index_of_char_from(context, value, code_point, 0, out_index);
}

mj2x_regc_status mj2x_string_index_of_string_from(mj2x_regc_context* context, mj2x_ref value,
                                                  mj2x_ref needle, int32_t from_index,
                                                  int32_t* out_index)
{
    const char* operation = "String.indexOf";
    if (out_index == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_index = -1;
    if (value == NULL || needle == NULL) return mj2x_string_null(context, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize  value_length  = (*env)->GetStringLength(env, (jstring)value);
    const jsize  needle_length = (*env)->GetStringLength(env, (jstring)needle);
    const jchar* value_chars   = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (value_chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    const jchar* needle_chars = (*env)->GetStringChars(env, (jstring)needle, NULL);
    if (needle_chars == NULL) {
        (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
        return mj2x_string_pending_or_oom(context, env, operation);
    }
    *out_index = mj2x_string_index_of_units(
        value_chars, value_length, needle_chars, needle_length, from_index);
    (*env)->ReleaseStringChars(env, (jstring)needle, needle_chars);
    (*env)->ReleaseStringChars(env, (jstring)value, value_chars);
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_index_of_string(mj2x_regc_context* context, mj2x_ref value,
                                             mj2x_ref needle, int32_t* out_index)
{
    return mj2x_string_index_of_string_from(context, value, needle, 0, out_index);
}

mj2x_regc_status mj2x_string_replace_char(mj2x_regc_context* context, mj2x_ref value,
                                          uint16_t old_char, uint16_t new_char,
                                          mj2x_ref* out_string)
{
    const char* operation = "String.replace";
    if (out_string == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_string = NULL;
    if (value == NULL) return mj2x_string_null(context, operation);
    if (old_char == new_char) return mj2x_string_copy_ref(context, value, out_string, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize  length = (*env)->GetStringLength(env, (jstring)value);
    const jchar* chars  = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    jchar* units = NULL;
    for (jsize i = 0; i < length; ++i) {
        if (chars[i] == (jchar)old_char) {
            units = (jchar*)malloc((size_t)length * sizeof(jchar));
            if (units == NULL) {
                (*env)->ReleaseStringChars(env, (jstring)value, chars);
                mj2x_regc_report_status(context, MJ2X_REGC_STATUS_OUT_OF_MEMORY, operation);
                return MJ2X_REGC_STATUS_OUT_OF_MEMORY;
            }
            memcpy(units, chars, (size_t)length * sizeof(jchar));
            break;
        }
    }
    if (units == NULL) {
        (*env)->ReleaseStringChars(env, (jstring)value, chars);
        return mj2x_string_copy_ref(context, value, out_string, operation);
    }
    for (jsize i = 0; i < length; ++i) {
        if (units[i] == (jchar)old_char) units[i] = (jchar)new_char;
    }
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    jstring result = (*env)->NewString(env, units, length);
    free(units);
    if (result == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    *out_string = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

mj2x_regc_status mj2x_string_trim(mj2x_regc_context* context, mj2x_ref value, mj2x_ref* out_string)
{
    const char* operation = "String.trim";
    if (out_string == NULL) return mj2x_string_invalid_argument(context, operation);
    *out_string = NULL;
    if (value == NULL) return mj2x_string_null(context, operation);
    JNIEnv* env = mj2x_string_env(context);
    if (env == NULL) return mj2x_string_invalid_state(context, operation);
    const jsize  length = (*env)->GetStringLength(env, (jstring)value);
    const jchar* chars  = (*env)->GetStringChars(env, (jstring)value, NULL);
    if (chars == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    jsize begin = 0;
    jsize end   = length;
    while (begin < end && chars[begin] <= 0x20) ++begin;
    while (end > begin && chars[end - 1] <= 0x20) --end;
    if (begin == 0 && end == length) {
        (*env)->ReleaseStringChars(env, (jstring)value, chars);
        return mj2x_string_copy_ref(context, value, out_string, operation);
    }
    const jchar  empty  = 0;
    const jchar* units  = begin == end ? &empty : chars + begin;
    jstring      result = (*env)->NewString(env, units, end - begin);
    (*env)->ReleaseStringChars(env, (jstring)value, chars);
    if (result == NULL) return mj2x_string_pending_or_oom(context, env, operation);
    *out_string = (mj2x_ref)result;
    return MJ2X_REGC_STATUS_OK;
}

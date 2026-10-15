#include "regc/mj2x_regc_crt.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char* kArithmeticException = "java/lang/ArithmeticException";
static const char* kDivideByZero        = "/ by zero";

void mj2x_regc_throw_exception(mj2x_regc_context* context, const char* class_name,
                               const char* message)
{
    if (context == NULL || context->throwException == NULL) return;
    context->throwException(context->env, class_name, message);
}

void mj2x_regc_report_status(mj2x_regc_context* context, mj2x_regc_status status,
                             const char* operation)
{
    const char* class_name = NULL;
    if (status == MJ2X_REGC_STATUS_OK || status == MJ2X_REGC_STATUS_PENDING_EXCEPTION) return;

    switch (status) {
    case MJ2X_REGC_STATUS_INVALID_ARGUMENT:
        class_name = "java/lang/IllegalArgumentException";
        break;
    case MJ2X_REGC_STATUS_OUT_OF_MEMORY: class_name = "java/lang/OutOfMemoryError"; break;
    case MJ2X_REGC_STATUS_INVALID_STATE: class_name = "java/lang/IllegalStateException"; break;
    case MJ2X_REGC_STATUS_UNSUPPORTED:
        class_name = "java/lang/UnsupportedOperationException";
        break;
    default: class_name = "java/lang/RuntimeException"; break;
    }
    mj2x_regc_throw_exception(
        context, class_name, operation == NULL ? "mj2x runtime operation" : operation);
}

void mj2x_regc_init_context(mj2x_regc_context* context, void* env,
                            mj2x_regc_throw_callback throwException)
{
    if (context == NULL) return;
    context->env            = env;
    context->throwException = throwException;
}

void mj2x_regc_set_env(mj2x_regc_context* context, void* env)
{
    if (context == NULL) return;
    context->env = env;
}

void* mj2x_regc_env(const mj2x_regc_context* context)
{
    return context == NULL ? NULL : context->env;
}

void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message)
{
    mj2x_regc_throw_exception(
        context, kArithmeticException, message == NULL ? kDivideByZero : message);
}

int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs)
{
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT32_MIN && rhs == -1) return INT32_MIN;
    return lhs / rhs;
}

int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs)
{
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT64_MIN && rhs == -1) return INT64_MIN;
    return lhs / rhs;
}

int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs)
{
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT32_MIN && rhs == -1) return 0;
    return lhs % rhs;
}

int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs)
{
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == INT64_MIN && rhs == -1) return 0;
    return lhs % rhs;
}

void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason)
{
    (void)context;
    (void)reason;
}

/* LTO 不可折叠不透明常量：volatile 读迫使编译器每次真实加载——cross-TU
   IPSCCP/常量传播无法把 const_key(site) 折成常量（否则 E ^ K 被折回 C，
   立即数隐藏白做；--native-opt-level 的 LTO 档同样守得住）。
   值与 RegIR pass（reg_transform_passes.cpp kOpaqueConstant）同源，改必须同步。 */
static volatile uint64_t g_const_key_opaque = 0x517CC1B727220A95ull;

/* 立即数隐藏密钥派生：splitmix64 单步（黄金比例增量后四步混合），输入先掺
   不透明常量。与 reg_transform_passes.cpp 的 splitmix64（ca::random::
   SplitMix64 正典，#1027 A3 逐位锁定）+ kOpaqueConstant 同源——改常数/
   移位/顺序即使 E ^ K != C，两端必须同步改。site 为函数内局部计数器；不掺
   seed 的理由见头文件契约。 */
int64_t mj2x_regc_const_key(int64_t site)
{
    const uint64_t x     = (uint64_t)site ^ g_const_key_opaque;
    uint64_t       state = x + 0x9E3779B97F4A7C15ull;
    uint64_t       z     = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (int64_t)(z ^ (z >> 31));
}

/* JVM f2i/f2l/d2i/d2l 语义（JLS 5.1.3）：NaN->0、向零截断、超界饱和。
 * C 裸 cast 对 NaN/越界是 UB（#1027 D2），故浮点->整型一律走这两个 helper。
 * 饱和点取 (double)INT32_MAX + 1.0 = 2^31（INT64 侧 (double)INT64_MAX 因
 * 53 位尾数本就舍入为 2^63），不用硬编码魔数。与 vmc4 运行时
 * mj2x_vm_f2i32/f2i64 逐分支同语义。 */
int32_t mj2x_regc_f2i32(double value)
{
    if (value != value) return 0; /* NaN */
    if (value >= (double)INT32_MAX + 1.0) return INT32_MAX;
    if (value < (double)INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

int64_t mj2x_regc_f2i64(double value)
{
    if (value != value) return 0; /* NaN */
    if (value >= (double)INT64_MAX) return INT64_MAX;
    if (value < (double)INT64_MIN) return INT64_MIN;
    return (int64_t)value;
}

static jclass mj2x_regc_find_class_uncached(JNIEnv* env, const char* desc)
{
    if (env == NULL || desc == NULL) return NULL;
    if (desc[0] == 'L') {
        char name[1024];
        int  i = 0;
        while (desc[i + 1] != ';' && desc[i + 1] != '\0' && i < 1023) {
            name[i] = desc[i + 1];
            i++;
        }
        name[i] = '\0';
        return (*env)->FindClass(env, name);
    }
    return (*env)->FindClass(env, desc);
}

static jclass mj2x_regc_global_class(JNIEnv* env, const char* desc, jclass* slot)
{
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

jclass mj2x_regc_cached_class(JNIEnv* env, mj2x_regc_class_cache* cache)
{
    if (cache == NULL) return NULL;
    return mj2x_regc_global_class(env, cache->desc, &cache->value);
}

jmethodID mj2x_regc_cached_static_method(JNIEnv* env, mj2x_regc_static_method_cache* cache)
{
    if (env == NULL || cache == NULL) return NULL;
    if (cache->method != NULL) return cache->method;

    jclass owner = mj2x_regc_global_class(env, cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, owner, cache->name, cache->desc);
    if (method == NULL) return NULL;
    cache->method = method;
    return method;
}

jfieldID mj2x_regc_cached_static_field(JNIEnv* env, mj2x_regc_static_field_cache* cache)
{
    if (env == NULL || cache == NULL) return NULL;
    if (cache->field != NULL) return cache->field;

    jclass owner = mj2x_regc_global_class(env, cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jfieldID field = (*env)->GetStaticFieldID(env, owner, cache->name, cache->desc);
    if (field == NULL) return NULL;
    cache->field = field;
    return field;
}

int32_t _mj2x_intrinsic_int_compare(int32_t lhs, int32_t rhs)
{
    return (lhs > rhs) - (lhs < rhs);
}

int32_t _mj2x_intrinsic_long_compare(int64_t lhs, int64_t rhs)
{
    return (lhs > rhs) - (lhs < rhs);
}

int32_t _mj2x_intrinsic_byte_compare(int8_t lhs, int8_t rhs)
{
    return (int32_t)lhs - (int32_t)rhs;
}

int32_t _mj2x_intrinsic_short_compare(int16_t lhs, int16_t rhs)
{
    return (int32_t)lhs - (int32_t)rhs;
}

int32_t _mj2x_intrinsic_float_compare(float lhs, float rhs)
{
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    uint32_t lhs_bits;
    uint32_t rhs_bits;
    memcpy(&lhs_bits, &lhs, sizeof(lhs_bits));
    memcpy(&rhs_bits, &rhs, sizeof(rhs_bits));
    if ((lhs_bits & 0x7fffffffU) > 0x7f800000U) lhs_bits = 0x7fc00000U;
    if ((rhs_bits & 0x7fffffffU) > 0x7f800000U) rhs_bits = 0x7fc00000U;
    if (lhs_bits == rhs_bits) return 0;
    return ((int32_t)lhs_bits < (int32_t)rhs_bits) ? -1 : 1;
}

int32_t _mj2x_intrinsic_double_compare(double lhs, double rhs)
{
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    uint64_t lhs_bits;
    uint64_t rhs_bits;
    memcpy(&lhs_bits, &lhs, sizeof(lhs_bits));
    memcpy(&rhs_bits, &rhs, sizeof(rhs_bits));
    if ((lhs_bits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) {
        lhs_bits = 0x7ff8000000000000ULL;
    }
    if ((rhs_bits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) {
        rhs_bits = 0x7ff8000000000000ULL;
    }
    if (lhs_bits == rhs_bits) return 0;
    return ((int64_t)lhs_bits < (int64_t)rhs_bits) ? -1 : 1;
}

int32_t _mj2x_intrinsic_bool_compare(int8_t lhs, int8_t rhs)
{
    return ((lhs != 0) > (rhs != 0)) - ((lhs != 0) < (rhs != 0));
}

int32_t _mj2x_intrinsic_char_compare(uint16_t lhs, uint16_t rhs)
{
    return (int32_t)lhs - (int32_t)rhs;
}

static JNIEnv* mj2x_regc_context_env(mj2x_regc_context* context)
{
    if (context == NULL) return NULL;
    return (JNIEnv*)context->env;
}

mj2x_ref _mj2x_intrinsic_int_to_string(mj2x_regc_context* context, int32_t value)
{
    JNIEnv* env = mj2x_regc_context_env(context);
    if (env == NULL) return NULL;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    return (mj2x_ref)(*env)->NewStringUTF(env, buffer);
}

mj2x_ref _mj2x_intrinsic_long_to_string(mj2x_regc_context* context, int64_t value)
{
    JNIEnv* env = mj2x_regc_context_env(context);
    if (env == NULL) return NULL;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    return (mj2x_ref)(*env)->NewStringUTF(env, buffer);
}

mj2x_ref _mj2x_intrinsic_bool_to_string(mj2x_regc_context* context, int8_t value)
{
    JNIEnv* env = mj2x_regc_context_env(context);
    if (env == NULL) return NULL;
    return (mj2x_ref)(*env)->NewStringUTF(env, value ? "true" : "false");
}

mj2x_ref _mj2x_intrinsic_char_to_string(mj2x_regc_context* context, uint16_t value)
{
    JNIEnv* env = mj2x_regc_context_env(context);
    if (env == NULL) return NULL;
    char    buffer[4];
    int32_t length = 0;
    if (value == 0) {
        buffer[0] = (char)0xC0;
        buffer[1] = (char)0x80;
        length    = 2;
    }
    else if (value <= 0x7F) {
        buffer[0] = (char)value;
        length    = 1;
    }
    else if (value <= 0x7FF) {
        buffer[0] = (char)(0xC0 | (value >> 6));
        buffer[1] = (char)(0x80 | (value & 0x3F));
        length    = 2;
    }
    else {
        buffer[0] = (char)(0xE0 | (value >> 12));
        buffer[1] = (char)(0x80 | ((value >> 6) & 0x3F));
        buffer[2] = (char)(0x80 | (value & 0x3F));
        length    = 3;
    }
    buffer[length] = '\0';
    return (mj2x_ref)(*env)->NewStringUTF(env, buffer);
}

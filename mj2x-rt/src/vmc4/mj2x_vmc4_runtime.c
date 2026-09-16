/// @file mj2x_vmc4_runtime.c
/// @brief VMC4 运行时实现(#894 运行时/模板分离下沉)。
///
/// 原为 vm_interpreter_emit.cpp::emitRuntimePrelude 的逐 TU 内联字符串,
/// 现物理下沉 mj2x-rt,生成工程经编译链接消费。语义逐位不变。

#include "vmc4/mj2x_vmc4_runtime.h"

#include <limits.h>

// JVM 整数除法语义:INT_MIN / -1 不抛(回绕到 INT_MIN),其余交给硬件。
int32_t mj2x_vm_div_i32(int32_t a, int32_t b)
{
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}

int64_t mj2x_vm_div_i64(int64_t a, int64_t b)
{
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}

int32_t mj2x_vm_rem_i32(int32_t a, int32_t b)
{
    if (a == INT32_MIN && b == -1) return 0;
    return a % b;
}

int64_t mj2x_vm_rem_i64(int64_t a, int64_t b)
{
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

// JVM f2i/f2l 语义:NaN -> 0,向零截断;截断后超界才饱和(C cast 对超界/NaN
// 是 UB)。饱和点取 (double)INTxx_MAX + 1.0 = 2^31/2^63((double)INT64_MAX 因
// 53 位尾数本就舍入为 2^63),不用硬编码魔数。
int32_t mj2x_vm_f2i32(double v)
{
    if (v != v) return 0;
    if (v >= (double)INT32_MAX + 1.0) return INT32_MAX;
    if (v < (double)INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

int64_t mj2x_vm_f2i64(double v)
{
    if (v != v) return 0;
    if (v >= (double)INT64_MAX) return INT64_MAX;
    if (v < (double)INT64_MIN) return INT64_MIN;
    return (int64_t)v;
}

// 抛 NullPointerException(数组/对象 null 访问)。pending exception 由调用方
// 检测并 return,让异常传播回 Java。
void mj2x_vm_throw_npe(JNIEnv* env, const char* msg)
{
    jclass _c = (*env)->FindClass(env, "java/lang/NullPointerException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, msg);
        (*env)->DeleteLocalRef(env, _c);
    }
}

// 抛已有异常对象(athrow):null -> NPE(JVM 语义)。
void mj2x_vm_throw(JNIEnv* env, void* value)
{
    if (value == NULL) {
        mj2x_vm_throw_npe(env, "athrow null");
        return;
    }
    (*env)->Throw(env, (jthrowable)value);
}

// 除零抛 ArithmeticException(JVM 语义)。
void mj2x_vm_throw_arith(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/ArithmeticException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "divide by zero");
        (*env)->DeleteLocalRef(env, _c);
    }
}

// 异常类型匹配:catch-all(caught_type 空/NULL)恒匹配;FindClass 失败(类型
// 不可解析)清 pending 并按不匹配处理(对齐 RegC 语义)。
int32_t mj2x_vm_exception_matches(JNIEnv* env, jobject ex, const char* caught_type)
{
    if (caught_type == NULL || caught_type[0] == '\0') return 1;
    jclass _c = (*env)->FindClass(env, caught_type);
    if (_c == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    int32_t _r = (*env)->IsInstanceOf(env, ex, _c) ? 1 : 0;
    (*env)->DeleteLocalRef(env, _c);
    return _r;
}

// ---------------------------------------------------------------------------
// 程序 blob at-rest 解密(issue #760 首切片)。
//
// 密钥流形态与 native lib 加密(mj2x-core native_lib_crypto.hpp / NativeLoader
// 字节码内实现)保持一致:xorshift32 逐步推进,每字节取 state & 0xFF 异或。
// 产物 C 源/二进制中只有密文 blob + 现场派生的 32 位 seed 常量,不出现
// 明文程序流,也不出现整块 key/keystream 数组(密钥流由循环现场重算)。
// ---------------------------------------------------------------------------

// xorshift32 一步推进(x ^= x<<13; x ^= x>>17; x ^= x<<5)。
static uint32_t mj2x_vm_blob_xorshift32(uint32_t state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void mj2x_vm_blob_decrypt(unsigned char* data, unsigned int size, uint32_t key)
{
    // key == 0 回退固定种子:0 是 xorshift32 的死锁态(密钥流恒 0,等价未加密)。
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    unsigned int i;
    for (i = 0; i < size; ++i) {
        state = mj2x_vm_blob_xorshift32(state);
        data[i] = (unsigned char)(data[i] ^ (unsigned char)(state & 0xFFu));
    }
}

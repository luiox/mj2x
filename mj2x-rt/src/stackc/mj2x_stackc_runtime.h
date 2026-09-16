#pragma once

// StackC 运行时 ABI(#894 运行时/模板分离下沉)。
//
// 生成 TU 的 per-callsite 缓存槽(数据,生成期发射)经本头提供的无锁缓存
// 原语访问;类/方法/字段解析 slow path 实现位于 mj2x_stackc_runtime.c
// (mj2x-rt 编译),生成工程经编译链接消费。fast-path 宏与 static __inline
// 必须留在头内(内联语义要求同 TU)。

#include <jni.h>

#include <stddef.h>
#include <stdint.h>

#ifndef MJ2X_STACKC_API
#define MJ2X_STACKC_API
#endif

// ---- 分支预测平台宏 + 无锁缓存原语(对齐 JNIC 3.7.0) ----
#if defined(_MSC_VER)
#include <intrin.h>
#define _MJ2X_LIKELY(x)   (x)
#define _MJ2X_UNLIKELY(x) (x)
static __inline void* _mj2x_cache_load(void* volatile* slot) { return *slot; }
static __inline void* _mj2x_cache_cas(void* volatile* slot, void* expected, void* desired)
{
    return _InterlockedCompareExchangePointer(slot, desired, expected);
}
#else
#define _MJ2X_LIKELY(x)   __builtin_expect((x), 1)
#define _MJ2X_UNLIKELY(x) __builtin_expect((x), 0)
static __inline void* _mj2x_cache_load(void* volatile* slot)
{
    return __atomic_load_n(slot, __ATOMIC_RELAXED);
}
static __inline void* _mj2x_cache_cas(void* volatile* slot, void* expected, void* desired)
{
    void* previous = expected;
    __atomic_compare_exchange_n(slot, &previous, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return previous;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// owner 类加载器感知的类解析(binary name '/'->'.' 后 Class.forName 回退)。
MJ2X_STACKC_API jclass _mj2x_find_class(JNIEnv* env, jclass owner, const char* name);

// 类描述符('L...;'/'[...')解析到 Class。
MJ2X_STACKC_API jclass _mj2x_component_class(JNIEnv* env, jclass owner, const char* desc);

// 多维数组分配(逐维递归,primitive 分支直调 New<X>Array)。
MJ2X_STACKC_API jobject _mj2x_new_multi_array(JNIEnv* env, jclass owner, const char* desc,
                                              jint* dims, int dimCount);

// 方法/字段/类缓存 miss slow path:解析后 CAS 回填共享缓存槽。
MJ2X_STACKC_API jmethodID _mj2x_mid_slow(JNIEnv* env, jmethodID* slot, jclass cls,
                                         const char* name, const char* desc, int is_static);
MJ2X_STACKC_API jfieldID _mj2x_fid_slow(JNIEnv* env, jfieldID* slot, jclass cls,
                                        const char* name, const char* desc, int is_static);
MJ2X_STACKC_API jclass _mj2x_class_slow(JNIEnv* env, jclass* slot, jclass owner, const char* name);

// ---- 字符串池 at-rest 解密(--strings-encrypt) ----
/// 对生成 TU 的 UTF-16 字符串池(_mj2x_string_pool)原地解密:xorshift32 密钥流
/// 逐单位 XOR(先推进 state 再取低 16 位),与发射侧 StackCBackend::emitStringPool
/// 的加密逐位一致(与 core/native_lib_crypto.hpp 的 xorshift32 同式)。
/// key == 0 时使用固定种子(与 xorStreamBytes 同约定)。
/// 调用时机:生成代码在 JNI_OnLoad 内批量调用一次——JVM 保证 JNI_OnLoad 单线程
/// 且先于任何 RegisterNatives/native 方法,无需调用点同步(取舍见发射侧注释)。
MJ2X_STACKC_API void mj2x_stackc_string_pool_decrypt(uint16_t* pool, size_t unit_count,
                                                     uint32_t key);

#ifdef __cplusplus
}
#endif

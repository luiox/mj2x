#pragma once

// VMC4 运行时 ABI(#894 运行时/模板分离下沉)。
//
// 解释器生成代码只含方法级结构(常量池/字节码数组、dispatcher、handler);
// 本头提供跨 TU 固定的类型与 JVM 语义 helper 声明。实现位于
// mj2x_vmc4_runtime.c(mj2x-rt 编译),生成工程经编译链接消费,
// 不再由发射侧逐 TU 内联。

#include <jni.h>

#include <stddef.h>
#include <stdint.h>

#ifndef MJ2X_VMC4_API
#define MJ2X_VMC4_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 寄存器槽联合体 ----
// i 用于 I32/I64(I32 写入时由 handler 截断到 32 位),f 用于 F32/F64(统一用
// double)。ref 用 void* 而非 jobject 保持头自包含语义(存取时双向 cast)。
typedef union mj2x_vm_slot {
    int64_t i;
    double  f;
    void*   ref;
} mj2x_vm_slot;

// Call helper 函数指针类型:解释器经函数指针表分派到 per-callsite 的 JNI
// helper。参数:(env, regs, recv, argIdxArr, argc, dest)。
typedef void (*mj2x_vm_call_fn)(JNIEnv*, mj2x_vm_slot*, unsigned int, unsigned int*, unsigned char,
                                unsigned int);

// ---- JVM 整数除法语义(INT_MIN / -1 不抛,回绕) ----
MJ2X_VMC4_API int32_t mj2x_vm_div_i32(int32_t a, int32_t b);
MJ2X_VMC4_API int64_t mj2x_vm_div_i64(int64_t a, int64_t b);
MJ2X_VMC4_API int32_t mj2x_vm_rem_i32(int32_t a, int32_t b);
MJ2X_VMC4_API int64_t mj2x_vm_rem_i64(int64_t a, int64_t b);

// ---- JVM f2i/f2l 饱和转换(NaN->0,向零截断,超界饱和) ----
MJ2X_VMC4_API int32_t mj2x_vm_f2i32(double v);
MJ2X_VMC4_API int64_t mj2x_vm_f2i64(double v);

// ---- 异常抛出与匹配 ----
MJ2X_VMC4_API void mj2x_vm_throw_npe(JNIEnv* env, const char* msg);
MJ2X_VMC4_API void mj2x_vm_throw(JNIEnv* env, void* value);
MJ2X_VMC4_API void mj2x_vm_throw_arith(JNIEnv* env);
MJ2X_VMC4_API int32_t mj2x_vm_exception_matches(JNIEnv* env, jobject ex, const char* caught_type);

// ---- 程序 blob at-rest 解密（issue #760 首切片） ----
// 发射期（mj2x 侧）以 xorshift32 密钥流逐字节 XOR 加密 opcode 字节流数据段；
// 本函数在生成代码的 JNI_OnLoad（库加载期，先于任何 native 调用）被调用，
// 对 blob 原地一次性解密。密钥流与打包侧 mj2x-core native_lib_crypto.hpp 的
// xorStreamBytes 逐字节一致（每字节先推进 state 再取 state & 0xFF 异或）；
// XOR 为对合运算，同一函数兼作加密/解密。key == 0 回退固定种子（与打包侧
// 约定一致；xorshift32 的 0 态是死锁态，密钥流会退化为全零）。
MJ2X_VMC4_API void mj2x_vm_blob_decrypt(unsigned char* data, unsigned int size, uint32_t key);

#ifdef __cplusplus
}
#endif

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

#ifdef __cplusplus
}
#endif

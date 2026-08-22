#pragma once

// LLVM IR 后端运行时 ABI(#894 mj2x-llvmrt 下沉,阶段一)。
//
// 原 llvm_ir_helpers.cpp 以 `define internal` 向每模块内联的固定 IR 函数,
// 阶段一先下沉四个最小体(异常抛出/查询);生成 .ll 改为 declare 并链接本
// 实现(mj2x-rt 编译,addCrtSources 递归收集自动接线)。符号名与 ABI 与原
// IR 定义严格一致:i8=jboolean, i16=jchar/jshort, ptr=对象引用/JNIEnv*。

#include <jni.h>

#include <stdint.h>

#ifndef MJ2X_LLVMIR_API
#define MJ2X_LLVMIR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

MJ2X_LLVMIR_API void mj2x_llvm_throw_null(JNIEnv* env);
MJ2X_LLVMIR_API void mj2x_llvm_throw_arith(JNIEnv* env);
MJ2X_LLVMIR_API jobject mj2x_llvm_exception_occurred(JNIEnv* env);
MJ2X_LLVMIR_API void mj2x_llvm_exception_clear(JNIEnv* env);

// ---- 阶段二 ----
MJ2X_LLVMIR_API void* mj2x_llvm_new_object(JNIEnv* env, const char* name);
MJ2X_LLVMIR_API void* mj2x_llvm_check_cast(JNIEnv* env, void* obj, const char* desc);
MJ2X_LLVMIR_API int32_t mj2x_llvm_instance_of(JNIEnv* env, void* obj, const char* desc);
MJ2X_LLVMIR_API int mj2x_llvm_exception_check(JNIEnv* env);
MJ2X_LLVMIR_API int mj2x_llvm_exception_matches(JNIEnv* env, void* ex, const char* catch_type);
MJ2X_LLVMIR_API void mj2x_llvm_throw(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API void* mj2x_llvm_string_const(JNIEnv* env, const char* utf8);
MJ2X_LLVMIR_API int mj2x_llvm_ref_eq(JNIEnv* env, void* a, void* b);

#ifdef __cplusplus
}
#endif

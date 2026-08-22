/// @file mj2x_llvmir_runtime.c
/// @brief LLVM IR 后端运行时实现(#894 mj2x-llvmrt 下沉,阶段一)。
///
/// 原 llvm_ir_helpers.cpp 的 `define internal` IR 体逐语义翻译为 C99
/// (IR 手搓 JNIEnv 槽位分发 ≙ (*env)->调用)。异常消息与控制流保持一致:
/// 抛出后由生成代码的 ExceptionCheck 面感知,本层不额外清理。

#include "llvmir/mj2x_llvmir_runtime.h"

void mj2x_llvm_throw_null(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/NullPointerException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "null");
        (*env)->DeleteLocalRef(env, _c);
    }
}

void mj2x_llvm_throw_arith(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/ArithmeticException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "/ by zero");
        (*env)->DeleteLocalRef(env, _c);
    }
}

jobject mj2x_llvm_exception_occurred(JNIEnv* env) { return (*env)->ExceptionOccurred(env); }

void mj2x_llvm_exception_clear(JNIEnv* env) { (*env)->ExceptionClear(env); }

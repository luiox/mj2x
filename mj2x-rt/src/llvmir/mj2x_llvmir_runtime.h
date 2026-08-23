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

// ---- 阶段三:数组族 ----
MJ2X_LLVMIR_API void* mj2x_llvm_new_array(JNIEnv* env, const char* desc, int32_t count);
MJ2X_LLVMIR_API void* mj2x_llvm_new_multi_array(JNIEnv* env, const char* desc, const int32_t* dims, int32_t ndims);
MJ2X_LLVMIR_API void* mj2x_llvm_new_object_array(JNIEnv* env, int32_t count, void* cls);
MJ2X_LLVMIR_API void mj2x_llvm_set_object_array_element(JNIEnv* env, void* arr, int32_t idx, void* val);
MJ2X_LLVMIR_API int32_t mj2x_llvm_array_length(JNIEnv* env, void* arr);

// ---- 阶段三b:字段族(load/store ×9 kind;static/instance 合一,is_static
// 非 0 走静态路径;实例 receiver 为 null 抛 NPE 返零值;窄类型 load 宽化,
// store 前窄化,bool 归一 0/1) ----
#define MJ2X_FIELD_PROTOS(kind, RetT)                                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_field_load_##kind(JNIEnv* env, void* rcv,           \
        const char* owner, const char* name, const char* desc, int32_t is_static);      \
    MJ2X_LLVMIR_API void mj2x_llvm_field_store_##kind(JNIEnv* env, void* rcv,          \
        const char* owner, const char* name, const char* desc, int32_t is_static,       \
        RetT val);

MJ2X_FIELD_PROTOS(bool, int32_t)
MJ2X_FIELD_PROTOS(byte, int32_t)
MJ2X_FIELD_PROTOS(char, int32_t)
MJ2X_FIELD_PROTOS(short, int32_t)
MJ2X_FIELD_PROTOS(int, int32_t)
MJ2X_FIELD_PROTOS(long, int64_t)
MJ2X_FIELD_PROTOS(float, float)
MJ2X_FIELD_PROTOS(double, double)
MJ2X_FIELD_PROTOS(ref, void*)

// ---- 阶段三c:数组元素族(load/store ×8 kind;数组为 null 抛 NPE 返零值;
// byte kind 按实际类型分派 byte[]/boolean[],ref 直用
// Get/SetObjectArrayElement) ----
#define MJ2X_ARRAY_PROTOS(kind, RetT)                                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_array_load_##kind(JNIEnv* env, void* arr,           \
        int32_t idx);                                                                   \
    MJ2X_LLVMIR_API void mj2x_llvm_array_store_##kind(JNIEnv* env, void* arr,          \
        int32_t idx, RetT val);

MJ2X_ARRAY_PROTOS(byte, int32_t)
MJ2X_ARRAY_PROTOS(short, int32_t)
MJ2X_ARRAY_PROTOS(char, int32_t)
MJ2X_ARRAY_PROTOS(int, int32_t)
MJ2X_ARRAY_PROTOS(long, int64_t)
MJ2X_ARRAY_PROTOS(float, float)
MJ2X_ARRAY_PROTOS(double, double)
MJ2X_ARRAY_PROTOS(ref, void*)

// ---- 阶段三d:方法调用族(×10 kind × virtual/nonvirtual/static;MethodA
// 形态,jvalue* 为参数数组;实例 receiver 为 null 抛 NPE 返零值;
// nonvirtual 的解析类取自 receiver 实际类而非 owner) ----
#define MJ2X_CALL_PROTOS(kind, RetT)                                                    \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_##kind(JNIEnv* env, void* rcv,                 \
        const char* owner, const char* name, const char* desc, jvalue* jv);             \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_nonvirtual_##kind(JNIEnv* env, void* rcv,      \
        const char* owner, const char* name, const char* desc, jvalue* jv);             \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_static_##kind(JNIEnv* env, const char* owner,  \
        const char* name, const char* desc, jvalue* jv);

MJ2X_CALL_PROTOS(object, void*)
MJ2X_CALL_PROTOS(bool, int8_t)
MJ2X_CALL_PROTOS(byte, int8_t)
MJ2X_CALL_PROTOS(char, int16_t)
MJ2X_CALL_PROTOS(short, int16_t)
MJ2X_CALL_PROTOS(int, int32_t)
MJ2X_CALL_PROTOS(long, int64_t)
MJ2X_CALL_PROTOS(float, float)
MJ2X_CALL_PROTOS(double, double)
MJ2X_CALL_PROTOS(void, void)

// ---- 阶段四a:类常量与按名抛异常 ----
MJ2X_LLVMIR_API void* mj2x_llvm_class_const(JNIEnv* env, const char* name);
MJ2X_LLVMIR_API void mj2x_llvm_throw_new(JNIEnv* env, const char* cls_name,
    const char* msg);

// ---- 阶段四b:monitor(null → NPE) ----
MJ2X_LLVMIR_API void mj2x_llvm_monitor_enter(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API void mj2x_llvm_monitor_exit(JNIEnv* env, void* obj);

#ifdef __cplusplus
}
#endif

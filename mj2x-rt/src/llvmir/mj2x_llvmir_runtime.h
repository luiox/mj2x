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

// ---- 阶段四c:indy 直调面 + 装箱/拆箱(cls+mid 已解析的 MethodA 直调;
// box=包装类.valueOf,unbox=XxxValue) ----
MJ2X_LLVMIR_API void* mj2x_llvm_indy_call_static_mid(JNIEnv* env, void* cls, void* mid,
    jvalue* jv);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_call_obj_mid(JNIEnv* env, void* obj, void* cls,
    void* mid, jvalue* jv);

MJ2X_LLVMIR_API void* mj2x_llvm_box_boolean(JNIEnv* env, int32_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_byte(JNIEnv* env, int32_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_char(JNIEnv* env, int32_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_short(JNIEnv* env, int32_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_int(JNIEnv* env, int32_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_long(JNIEnv* env, int64_t v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_float(JNIEnv* env, float v);
MJ2X_LLVMIR_API void* mj2x_llvm_box_double(JNIEnv* env, double v);

MJ2X_LLVMIR_API int32_t mj2x_llvm_unbox_boolean(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API int32_t mj2x_llvm_unbox_byte(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API int32_t mj2x_llvm_unbox_char(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API int32_t mj2x_llvm_unbox_short(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API int32_t mj2x_llvm_unbox_int(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API int64_t mj2x_llvm_unbox_long(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API float mj2x_llvm_unbox_float(JNIEnv* env, void* obj);
MJ2X_LLVMIR_API double mj2x_llvm_unbox_double(JNIEnv* env, void* obj);

// ---- 阶段四d:indy 固定解析 helper 组(MethodType/包装 TYPE/Class 对象/
// Lookup.find* Handle;bsm_<tag> 与站点级常量为发射期模板,不在本层) ----
MJ2X_LLVMIR_API void* mj2x_llvm_indy_method_type(JNIEnv* env, void* owner,
    const char* desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_wrapper_type(JNIEnv* env, const char* name);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_class_object(JNIEnv* env, const char* desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_field_handle(JNIEnv* env, void* lkcls, void* lookup,
    void* ocls, const char* name, const char* desc, const char* find_name,
    const char* find_desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_method_handle3(JNIEnv* env, void* lkcls, void* lookup,
    void* ocls, const char* name, void* type, const char* find_name,
    const char* find_desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_method_handle4(JNIEnv* env, void* lkcls, void* lookup,
    void* ocls, const char* name, void* type, void* special_cls, const char* find_name,
    const char* find_desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_method_handle2(JNIEnv* env, void* lkcls, void* lookup,
    void* ocls, void* type, const char* find_name, const char* find_desc);
MJ2X_LLVMIR_API void* mj2x_llvm_indy_handle(JNIEnv* env, void* lookup, void* owner_class,
    int32_t tag, const char* owner, const char* name, const char* desc);

// ---- 阶段五:立即数隐藏 per-site 密钥派生 ----
// 对标 jvmtp obfuscation.constants：applyConstantHiding 把整型立即数 C 改写为
// E ^ mj2x_llvm_const_key(site)（E = C ^ K 编译期物化，K 运行时派生）。splitmix64
// 单步（输入先掺 volatile 不透明常量 g_const_key_opaque，LTO 不可折叠）与
// reg_transform_passes.cpp 的 splitmix64（ca::random::SplitMix64 正典，#1027 A3
// 逐位锁定）+ kOpaqueConstant 同源——改常数/移位/顺序即使 E ^ K != C。site 为
// 函数内局部计数器，不掺 seed（site 必物化在产物里，掺 seed 不增加分析者成本）。
MJ2X_LLVMIR_API int64_t mj2x_llvm_const_key(int64_t site);

// ---- 阶段六:调用目标间接化 per-signature dispatcher ----
// 对标 jvmtp obfuscation.calls.*：applyCallIndirections 把外部（JDK/库）方法
// 调用点改写为按签名分派的运行时调用——调用点只带 (table, index)，目标身份
// （owner/name/desc 字符串）从调用点移出，静态分析须先解析 index → 条目才能
// 还原调用边。首次分派解析 owner 类 + jmethodID 并缓存入条目。
// invokeKind：0 = virtual/interface，1 = static，2 = special。
//
// 缓存纪律（与 mj2x-rt regc 侧 _mj2x_regc_cached_instance_method 对齐）：
// - 存进条目的 jclass 必须是 global ref：FindClass/GetObjectClass 返回 local ref，
//   只在当次 native 调用内有效，直接缓存会在下一次调用时悬垂。
// - virtual/special 按 receiver 实际类解析，不按声明 owner：抽象/接口 owner 上
//   按声明类解析会拿到抽象条目，CallXxxMethodA 触发 AbstractMethodError
//   （HotSpot 17 实测 ACCESS_VIOLATION）。故另设 per-receiver-class 槽。
typedef struct mj2x_llvm_call_site
{
    void*       ownerClass;   // jclass 缓存（global ref；NULL = 未解析）
    void*       method;       // jmethodID 缓存（NULL = 未解析）
    const char* owner;
    const char* name;
    const char* desc;
    int32_t     invokeKind;
    void*       lastReceiverClass;    // virtual/special 的 per-receiver-class 槽（global ref）
    void*       lastReceiverMethod;   // 与 lastReceiverClass 配对的解析结果
} mj2x_llvm_call_site;

MJ2X_LLVMIR_API int32_t mj2x_llvm_dispatch_i32(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);
MJ2X_LLVMIR_API int64_t mj2x_llvm_dispatch_i64(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);
MJ2X_LLVMIR_API float mj2x_llvm_dispatch_f32(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);
MJ2X_LLVMIR_API double mj2x_llvm_dispatch_f64(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);
MJ2X_LLVMIR_API void* mj2x_llvm_dispatch_ref(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);
MJ2X_LLVMIR_API void mj2x_llvm_dispatch_void(JNIEnv* env, mj2x_llvm_call_site* table,
    int32_t index, void* rcv, jvalue* jv);

#ifdef __cplusplus
}
#endif

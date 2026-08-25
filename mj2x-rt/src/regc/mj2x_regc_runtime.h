#pragma once

// RegC 运行时 helper ABI(#894 B 类真下沉)。
//
// 实现位于 mj2x_regc_runtime.c(mj2x-rt 编译);生成的 native TU 经本头
// 获得声明。TLS 状态(env/ctx/owner_class)由运行时定义、生成代码经
// extern 直接访问——三编译器宏与实现侧严格一致。

#include "regc/mj2x_regc_crt.h"

// helper 实现依赖的 C 标准库(原生成 TU 由发射侧 include 清单提供)
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 静态链接进生成工程,无导出需求;保留扩展点。
#ifndef MJ2X_REGC_API
#define MJ2X_REGC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- trace 开关(--trace-call-log 的运行时形态) ----
MJ2X_REGC_API void mj2x_regc_set_trace_call_log(int on);
MJ2X_REGC_API void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache,
                                                        int32_t arg_count, ...);

// ---- 共享类型(resolved dispatch,backend 逐程序声明实例) ----
typedef void (*mj2x_regc_fn_ptr_t)(void);
typedef struct mj2x_regc_resolved_fn {
    const char* owner;
    const char* name;
    const char* desc;
    jclass ownerClass;
    mj2x_regc_fn_ptr_t fn;
} mj2x_regc_resolved_fn_t;
typedef struct mj2x_regc_resolved_call_site {
    jmethodID method;
    mj2x_regc_fn_ptr_t fn;
    jclass implClass;
    jclass receiverClass;
    jmethodID method1;
    mj2x_regc_fn_ptr_t fn1;
    jclass implClass1;
    jclass receiverClass1;
} mj2x_regc_resolved_call_site_t;

// ---- 线程态(运行时定义,生成代码 extern 直访) ----
#if defined(__cplusplus)
extern thread_local JNIEnv* _mj2x_regc_env;
extern thread_local mj2x_regc_context _mj2x_regc_ctx;
extern thread_local jclass _mj2x_regc_owner_class;
#elif defined(_MSC_VER)
extern __declspec(thread) JNIEnv* _mj2x_regc_env;
extern __declspec(thread) mj2x_regc_context _mj2x_regc_ctx;
extern __declspec(thread) jclass _mj2x_regc_owner_class;
#else
extern _Thread_local JNIEnv* _mj2x_regc_env;
extern _Thread_local mj2x_regc_context _mj2x_regc_ctx;
extern _Thread_local jclass _mj2x_regc_owner_class;
#endif

// ---- 角色单例缓存(运行时定义,生成代码可复用;头文件仅 extern 声明) ----
/// String 类 / 异常类等固定角色的 jclass 全局缓存（NewGlobalRef 稳定句柄，
/// CAS 发布——并发首解析输家自删并复用赢家值）。
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_string_class_cache;
MJ2X_REGC_API extern mj2x_regc_instance_method_cache _mj2x_rt_object_to_string_method_cache;
MJ2X_REGC_API extern mj2x_regc_static_method_cache _mj2x_rt_string_value_of_float_cache;
MJ2X_REGC_API extern mj2x_regc_static_method_cache _mj2x_rt_string_value_of_double_cache;
MJ2X_REGC_API extern mj2x_regc_static_method_cache _mj2x_rt_float_parse_float_cache;
MJ2X_REGC_API extern mj2x_regc_static_method_cache _mj2x_rt_double_parse_double_cache;
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_npe_class_cache;
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_cce_class_cache;
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_oom_class_cache;
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_sioobe_class_cache;
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_rt_uoe_class_cache;

// ---- helper 原型 ----
/// 类解析：cached_* 走 cache 槽（global ref 钉住，进程级复用）；find_class
/// 每次现场解析返回 local ref（调用方负责释放或即刻使用）。
MJ2X_REGC_API jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache);
MJ2X_REGC_API mj2x_regc_context* _mj2x_regc_current_context(void);
/// 位模式直转浮点（JVM float/double 字面量的精确位语义）。
MJ2X_REGC_API float _mj2x_regc_f32_from_bits(uint32_t bits);
MJ2X_REGC_API double _mj2x_regc_f64_from_bits(uint64_t bits);
/// 异常抛出：throw_runtime 显式 env；throw_null/throw_unsupported 走 TLS env
/// 与固定异常类缓存，置 pending 后由调用点的异常分发消费。
MJ2X_REGC_API void _mj2x_regc_throw_runtime(void* env, const char* class_name, const char* message);
MJ2X_REGC_API void _mj2x_regc_throw_unsupported(const char* feature);
MJ2X_REGC_API void _mj2x_regc_throw_null(const char* message);
MJ2X_REGC_API jclass _mj2x_regc_find_class(const char* desc);
/// 全局类槽：NewGlobalRef + CAS 发布；miss 时 FindClass。*slot 为进程级
/// 稳定句柄，跨线程共享安全（并发首解析输家自删复用赢家值）。
MJ2X_REGC_API jclass _mj2x_regc_global_class(const char* desc, jclass* slot);
MJ2X_REGC_API jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache);
/// 方法 ID 缓存：static/owner 按声明类解析一次；instance 按 receiver 实际
/// 类解析（接口/抽象 owner 场景 lastReceiverClass 只增不删换占，见 .c 契约
/// 注释）。解析失败返回 NULL（pending 异常由调用点处理）。
MJ2X_REGC_API jmethodID _mj2x_regc_cached_static_method(mj2x_regc_static_method_cache* cache);
MJ2X_REGC_API jmethodID _mj2x_regc_cached_instance_method(mj2x_regc_instance_method_cache* cache, jobject receiver);
MJ2X_REGC_API jmethodID _mj2x_regc_cached_owner_method(mj2x_regc_instance_method_cache* cache, jobject receiver);
/// 字段 ID 缓存：静态/实例各一槽，按声明 owner 解析一次。
MJ2X_REGC_API jfieldID _mj2x_regc_cached_static_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API jfieldID _mj2x_regc_cached_instance_field(mj2x_regc_instance_field_cache* cache);
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_object(const char* type_desc);
/// 分配族：alloc_object/array/multi_array 每次现场解析类；*_cached 走
/// class_cache 槽（发射端对固定描述符做编译期 CSE 入池）。越界交 JNI 抛
/// AIOOBE，失败返回 NULL。
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache);
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length);
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_ref_array_cached(mj2x_regc_class_cache* component_cache, int32_t length);
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, int32_t* dims);
MJ2X_REGC_API mj2x_ref _mj2x_regc_alloc_multi_array_cached(const char* type_desc, int32_t dim_count, int32_t* dims, mj2x_regc_class_cache** component_caches);
MJ2X_REGC_API int32_t _mj2x_regc_array_length(mj2x_ref array_ref);
/// 类型检查：checkcast 失败置 pending ClassCastException 返回 NULL，
/// instanceof 返回 0/1（null 输入为 0）。
MJ2X_REGC_API mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc);
MJ2X_REGC_API mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, mj2x_regc_class_cache* cache);
MJ2X_REGC_API int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc);
MJ2X_REGC_API int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, mj2x_regc_class_cache* cache);
MJ2X_REGC_API void _mj2x_regc_monitor_enter(mj2x_ref value);
MJ2X_REGC_API void _mj2x_regc_monitor_exit(mj2x_ref value);
/// 异常面：exception_matches 按描述符 IsInstanceOf（catch-all 由调用点
/// 直接判定）；throw 对 null 值抛 NPE。
MJ2X_REGC_API int32_t _mj2x_regc_exception_check(void);
MJ2X_REGC_API mj2x_ref _mj2x_regc_exception_occurred(void);
MJ2X_REGC_API void _mj2x_regc_exception_clear(void);
MJ2X_REGC_API int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type);
MJ2X_REGC_API void _mj2x_regc_throw(mj2x_ref value);
MJ2X_REGC_API jclass _mj2x_regc_field_class(int32_t is_static, mj2x_ref receiver, const char* owner);
/// 字段族：load/store × 9 类型 × 三形态（按名现场解析 / 静态缓存槽 / 实例
/// 缓存槽）。null receiver 置 pending NPE。
MJ2X_REGC_API jfieldID _mj2x_regc_field_id(int32_t is_static, jclass c, const char* name, const char* desc);
MJ2X_REGC_API bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* cache);
MJ2X_REGC_API bool _mj2x_regc_load_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API int8_t _mj2x_regc_load_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API uint16_t _mj2x_regc_load_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API int16_t _mj2x_regc_load_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API int32_t _mj2x_regc_load_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API int64_t _mj2x_regc_load_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API float _mj2x_regc_load_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API double _mj2x_regc_load_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API mj2x_ref _mj2x_regc_load_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);
MJ2X_REGC_API void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, bool value);
MJ2X_REGC_API void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int8_t value);
MJ2X_REGC_API void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, uint16_t value);
MJ2X_REGC_API void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int16_t value);
MJ2X_REGC_API void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int32_t value);
MJ2X_REGC_API void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int64_t value);
MJ2X_REGC_API void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, float value);
MJ2X_REGC_API void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, double value);
MJ2X_REGC_API void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, mj2x_ref value);
MJ2X_REGC_API void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, bool value);
MJ2X_REGC_API void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, int8_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, uint16_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, int16_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, int32_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, int64_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, float value);
MJ2X_REGC_API void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, double value);
MJ2X_REGC_API void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, mj2x_ref value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value);
MJ2X_REGC_API void _mj2x_regc_store_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value);
MJ2X_REGC_API extern mj2x_regc_class_cache _mj2x_regc_boolean_array_cache;
MJ2X_REGC_API int32_t _mj2x_regc_is_boolean_array(mj2x_ref array_ref);
MJ2X_REGC_API int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index);
MJ2X_REGC_API void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value);
MJ2X_REGC_API void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t value);
MJ2X_REGC_API void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t value);
MJ2X_REGC_API void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t value);
MJ2X_REGC_API void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t value);
MJ2X_REGC_API void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value);
MJ2X_REGC_API void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double value);
MJ2X_REGC_API void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref value);
MJ2X_REGC_API const char* _mj2x_regc_skip_type(const char* p);
MJ2X_REGC_API int32_t _mj2x_regc_count_params(const char* desc);
MJ2X_REGC_API jobject _mj2x_regc_collect_args(const char* desc, int32_t arg_count, va_list ap, jvalue* values);
/// 通用调用族：call_<type> 按名现场解析（varargs 装配 jvalue 数组）；
/// call_cached_<type> 走静态方法缓存槽；call_cached_instance_<type> 走
/// 实例方法缓存（虚分派按 receiver 实际类）。null receiver 置 pending NPE。
MJ2X_REGC_API jmethodID _mj2x_regc_method_id(jobject receiver, int32_t is_static, const char* owner, const char* name, const char* desc);
MJ2X_REGC_API int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API float _mj2x_regc_call_f32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API double _mj2x_regc_call_f64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API void _mj2x_regc_call_void(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);
MJ2X_REGC_API int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API int32_t _mj2x_regc_call_cached_instance_i32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API int64_t _mj2x_regc_call_cached_instance_i64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API float _mj2x_regc_call_cached_instance_f32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API double _mj2x_regc_call_cached_instance_f64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API mj2x_ref _mj2x_regc_call_cached_instance_ref(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);
MJ2X_REGC_API extern int _mj2x_rt_trace_call_log;
MJ2X_REGC_API extern int _mj2x_rt_trace_depth;
/// intrinsic 族：String LDC 物化（TU 级 32 槽缓存）、类字面量、parseInt/
/// Long/Short/Byte radix 解析器（NumberFormatException 经 context 回调路由）、
/// String 只读热路径（UTF-16 直读，越界置 pending）、装箱/拆箱。
/// 这些是 RegC intrinsic（capabilities §3）的 CRT 侧实现。
MJ2X_REGC_API mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8);
MJ2X_REGC_API mj2x_ref _mj2x_regc_class_literal(mj2x_regc_context* context, const char* internal_name);
MJ2X_REGC_API int32_t _mj2x_intrinsic_digit_value(unsigned char ch);
MJ2X_REGC_API void _mj2x_intrinsic_throw_number_format(mj2x_regc_context* context);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_short(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_byte(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_parse_boolean(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API float _mj2x_intrinsic_parse_float(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API double _mj2x_intrinsic_parse_double(mj2x_regc_context* context, mj2x_ref jstr_ref);
MJ2X_REGC_API mj2x_ref _mj2x_intrinsic_float_to_string(mj2x_regc_context* context, float value);
MJ2X_REGC_API mj2x_ref _mj2x_intrinsic_double_to_string(mj2x_regc_context* context, double value);
MJ2X_REGC_API mj2x_ref _mj2x_intrinsic_object_to_string(mj2x_regc_context* context, mj2x_ref value);
MJ2X_REGC_API int32_t _mj2x_intrinsic_is_string(mj2x_ref value);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_equals(mj2x_ref lhs, mj2x_ref rhs);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_length(mj2x_ref value);
MJ2X_REGC_API uint16_t _mj2x_intrinsic_string_char_at(mj2x_ref value, int32_t index);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_hash_code(mj2x_ref value);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_is_empty(mj2x_ref value);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_starts_with(mj2x_ref value, mj2x_ref prefix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_ends_with(mj2x_ref value, mj2x_ref suffix);
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_index_of(mj2x_ref value, mj2x_ref needle);
MJ2X_REGC_API extern jmethodID _mj2x_intrinsic_string_contains_method;
MJ2X_REGC_API int32_t _mj2x_intrinsic_string_contains(mj2x_ref value, mj2x_ref seq);
MJ2X_REGC_API jobject _mj2x_regc_box_i32(int32_t value);
MJ2X_REGC_API jobject _mj2x_regc_box_i64(int64_t value);
MJ2X_REGC_API jobject _mj2x_regc_box_f32(float value);
MJ2X_REGC_API jobject _mj2x_regc_box_f64(double value);
MJ2X_REGC_API int32_t _mj2x_regc_unbox_i32(jobject value);
MJ2X_REGC_API int64_t _mj2x_regc_unbox_i64(jobject value);
MJ2X_REGC_API float _mj2x_regc_unbox_f32(jobject value);
MJ2X_REGC_API double _mj2x_regc_unbox_f64(jobject value);

#ifdef __cplusplus
}
#endif

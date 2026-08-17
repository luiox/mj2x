/// @file regc_runtime_template.cpp
/// @brief RegC 运行时 C-source 定义发射（从 regc_backend.cpp 抽出，字节不变）。
///
/// @details 见 regc_runtime_template.hpp。三个函数原先在 regc_backend.cpp 匿名
/// namespace，现搬到此处命名空间 mj2x 下，供 RegCBackend::finalize 调用。
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {

// emitRegCFieldAccessHelpers / emitRegCMethodCallHelpers 用 CSourceStream 包装
// raw-string 发射；对齐 regc_backend.cpp 的 using 约定，避免逐处加 codegen:: 前缀。
using codegen::CSourceStream;

void emitRegCFieldAccessHelpers(ca::str::Utf8StringBuilder& out)
{
    CSourceStream src;
    src.emitRaw(R"mj2x(
static jclass _mj2x_regc_field_class(int32_t is_static, mj2x_ref receiver, const char* owner) {
    if (is_static) return _mj2x_regc_find_class(owner);
    if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return NULL; }
    return (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, (jobject)receiver);
}
static jfieldID _mj2x_regc_field_id(int32_t is_static, jclass c, const char* name, const char* desc) {
    if (c == NULL) return NULL;
    return is_static ? (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, c, name, desc) : (*_mj2x_regc_env)->GetFieldID(_mj2x_regc_env, c, name, desc);
}
static bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return false; return is_static ? (*_mj2x_regc_env)->GetStaticBooleanField(_mj2x_regc_env, c, f) == JNI_TRUE : (*_mj2x_regc_env)->GetBooleanField(_mj2x_regc_env, (jobject)receiver, f) == JNI_TRUE; }
static int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (int8_t)(is_static ? (*_mj2x_regc_env)->GetStaticByteField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetByteField(_mj2x_regc_env, (jobject)receiver, f)); }
static uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (uint16_t)(is_static ? (*_mj2x_regc_env)->GetStaticCharField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetCharField(_mj2x_regc_env, (jobject)receiver, f)); }
static int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (int16_t)(is_static ? (*_mj2x_regc_env)->GetStaticShortField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetShortField(_mj2x_regc_env, (jobject)receiver, f)); }
static int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return is_static ? (*_mj2x_regc_env)->GetStaticIntField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetIntField(_mj2x_regc_env, (jobject)receiver, f); }
static int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return is_static ? (*_mj2x_regc_env)->GetStaticLongField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetLongField(_mj2x_regc_env, (jobject)receiver, f); }
static float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0.0f; return is_static ? (*_mj2x_regc_env)->GetStaticFloatField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetFloatField(_mj2x_regc_env, (jobject)receiver, f); }
static double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0.0; return is_static ? (*_mj2x_regc_env)->GetStaticDoubleField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetDoubleField(_mj2x_regc_env, (jobject)receiver, f); }
static mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return NULL; return is_static ? (mj2x_ref)(*_mj2x_regc_env)->GetStaticObjectField(_mj2x_regc_env, c, f) : (mj2x_ref)(*_mj2x_regc_env)->GetObjectField(_mj2x_regc_env, (jobject)receiver, f); }
static bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return false; return (*_mj2x_regc_env)->GetStaticBooleanField(_mj2x_regc_env, cache->ownerClass, f) == JNI_TRUE; }
static int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (int8_t)(*_mj2x_regc_env)->GetStaticByteField(_mj2x_regc_env, cache->ownerClass, f); }
static uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (uint16_t)(*_mj2x_regc_env)->GetStaticCharField(_mj2x_regc_env, cache->ownerClass, f); }
static int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (int16_t)(*_mj2x_regc_env)->GetStaticShortField(_mj2x_regc_env, cache->ownerClass, f); }
static int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->GetStaticIntField(_mj2x_regc_env, cache->ownerClass, f); }
static int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->GetStaticLongField(_mj2x_regc_env, cache->ownerClass, f); }
static float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0.0f; return (*_mj2x_regc_env)->GetStaticFloatField(_mj2x_regc_env, cache->ownerClass, f); }
static double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0.0; return (*_mj2x_regc_env)->GetStaticDoubleField(_mj2x_regc_env, cache->ownerClass, f); }
static mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->GetStaticObjectField(_mj2x_regc_env, cache->ownerClass, f); }

)mj2x");
    src.emitRaw(R"mj2x(

static bool _mj2x_regc_load_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return false; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return false; return (*_mj2x_regc_env)->GetBooleanField(_mj2x_regc_env, (jobject)receiver, f) == JNI_TRUE; }
static int8_t _mj2x_regc_load_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (int8_t)(*_mj2x_regc_env)->GetByteField(_mj2x_regc_env, (jobject)receiver, f); }
static uint16_t _mj2x_regc_load_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (uint16_t)(*_mj2x_regc_env)->GetCharField(_mj2x_regc_env, (jobject)receiver, f); }
static int16_t _mj2x_regc_load_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (int16_t)(*_mj2x_regc_env)->GetShortField(_mj2x_regc_env, (jobject)receiver, f); }
static int32_t _mj2x_regc_load_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (*_mj2x_regc_env)->GetIntField(_mj2x_regc_env, (jobject)receiver, f); }
static int64_t _mj2x_regc_load_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (*_mj2x_regc_env)->GetLongField(_mj2x_regc_env, (jobject)receiver, f); }
static float _mj2x_regc_load_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0.0f; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0.0f; return (*_mj2x_regc_env)->GetFloatField(_mj2x_regc_env, (jobject)receiver, f); }
static double _mj2x_regc_load_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0.0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0.0; return (*_mj2x_regc_env)->GetDoubleField(_mj2x_regc_env, (jobject)receiver, f); }
static mj2x_ref _mj2x_regc_load_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return NULL; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->GetObjectField(_mj2x_regc_env, (jobject)receiver, f); }

)mj2x");
    src.emitRaw(R"mj2x(

static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, bool value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticBooleanField(_mj2x_regc_env, c, f, value ? JNI_TRUE : JNI_FALSE); else (*_mj2x_regc_env)->SetBooleanField(_mj2x_regc_env, (jobject)receiver, f, value ? JNI_TRUE : JNI_FALSE); }
static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int8_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticByteField(_mj2x_regc_env, c, f, (jbyte)value); else (*_mj2x_regc_env)->SetByteField(_mj2x_regc_env, (jobject)receiver, f, (jbyte)value); }
static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, uint16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticCharField(_mj2x_regc_env, c, f, (jchar)value); else (*_mj2x_regc_env)->SetCharField(_mj2x_regc_env, (jobject)receiver, f, (jchar)value); }
static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticShortField(_mj2x_regc_env, c, f, (jshort)value); else (*_mj2x_regc_env)->SetShortField(_mj2x_regc_env, (jobject)receiver, f, (jshort)value); }
static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int32_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticIntField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetIntField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int64_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticLongField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetLongField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, float value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticFloatField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetFloatField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, double value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticDoubleField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetDoubleField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, mj2x_ref value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticObjectField(_mj2x_regc_env, c, f, (jobject)value); else (*_mj2x_regc_env)->SetObjectField(_mj2x_regc_env, (jobject)receiver, f, (jobject)value); }
static void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, bool value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticBooleanField(_mj2x_regc_env, cache->ownerClass, f, value ? JNI_TRUE : JNI_FALSE); }
static void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, int8_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticByteField(_mj2x_regc_env, cache->ownerClass, f, (jbyte)value); }
static void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, uint16_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticCharField(_mj2x_regc_env, cache->ownerClass, f, (jchar)value); }
static void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, int16_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticShortField(_mj2x_regc_env, cache->ownerClass, f, (jshort)value); }
static void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, int32_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticIntField(_mj2x_regc_env, cache->ownerClass, f, value); }
static void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, int64_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticLongField(_mj2x_regc_env, cache->ownerClass, f, value); }
static void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, float value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticFloatField(_mj2x_regc_env, cache->ownerClass, f, value); }
static void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, double value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticDoubleField(_mj2x_regc_env, cache->ownerClass, f, value); }
static void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, mj2x_ref value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticObjectField(_mj2x_regc_env, cache->ownerClass, f, (jobject)value); }

)mj2x");
    src.emitRaw(R"mj2x(

static void _mj2x_regc_store_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetBooleanField(_mj2x_regc_env, (jobject)receiver, f, value ? JNI_TRUE : JNI_FALSE); }
static void _mj2x_regc_store_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetByteField(_mj2x_regc_env, (jobject)receiver, f, (jbyte)value); }
static void _mj2x_regc_store_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetCharField(_mj2x_regc_env, (jobject)receiver, f, (jchar)value); }
static void _mj2x_regc_store_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetShortField(_mj2x_regc_env, (jobject)receiver, f, (jshort)value); }
static void _mj2x_regc_store_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetIntField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetLongField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetFloatField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetDoubleField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetObjectField(_mj2x_regc_env, (jobject)receiver, f, (jobject)value); }

)mj2x");
    {
        const std::string rendered = src.str();
        out.append(rendered.data(), rendered.size());
    }
}

void emitRegCMethodCallHelpers(ca::str::Utf8StringBuilder& out)
{
    CSourceStream src;
    src.emitRaw(R"mj2x(

static jmethodID _mj2x_regc_method_id(jobject receiver, int32_t is_static, const char* owner, const char* name, const char* desc) {
    if (is_static) return (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, _mj2x_regc_find_class(owner), name, desc);
    if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return NULL; }
    return (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, receiver), name, desc);
}
static int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0; return is_static ? (*_mj2x_regc_env)->CallStaticIntMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallIntMethodA(_mj2x_regc_env, receiver, m, values); }
static int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0; return is_static ? (*_mj2x_regc_env)->CallStaticLongMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallLongMethodA(_mj2x_regc_env, receiver, m, values); }
static float _mj2x_regc_call_f32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0.0f; return is_static ? (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallFloatMethodA(_mj2x_regc_env, receiver, m, values); }
static double _mj2x_regc_call_f64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0.0; return is_static ? (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallDoubleMethodA(_mj2x_regc_env, receiver, m, values); }
static mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return NULL; return is_static ? (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethodA(_mj2x_regc_env, receiver, m, values); }
static void _mj2x_regc_call_void(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return; if (is_static) (*_mj2x_regc_env)->CallStaticVoidMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values); else (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); }

static int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->CallStaticIntMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->CallStaticLongMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0.0f; return (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0.0; return (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->CallStaticVoidMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
static int32_t _mj2x_regc_call_cached_instance_i32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallIntMethodA(_mj2x_regc_env, receiver, m, values); }
static int64_t _mj2x_regc_call_cached_instance_i64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallLongMethodA(_mj2x_regc_env, receiver, m, values); }
static float _mj2x_regc_call_cached_instance_f32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0f; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0.0f; return (*_mj2x_regc_env)->CallFloatMethodA(_mj2x_regc_env, receiver, m, values); }
static double _mj2x_regc_call_cached_instance_f64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0.0; return (*_mj2x_regc_env)->CallDoubleMethodA(_mj2x_regc_env, receiver, m, values); }
static mj2x_ref _mj2x_regc_call_cached_instance_ref(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return NULL; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethodA(_mj2x_regc_env, receiver, m, values); }

)mj2x");
    {
        const std::string rendered = src.str();
        out.append(rendered.data(), rendered.size());
    }
}

void emitRuntimeHelpers(ca::str::Utf8StringBuilder& out, const std::string& cacheDeclarations,
                        const std::string& stringClassCacheName,
                        const std::string& objectToStringMethodCacheName,
                        const std::string& stringValueOfFloatMethodCacheName,
                        const std::string& stringValueOfDoubleMethodCacheName,
                        const std::string& floatParseFloatMethodCacheName,
                        const std::string& doubleParseDoubleMethodCacheName,
                        const std::string& nullPointerExceptionClassCacheName,
                        const std::string& classCastExceptionClassCacheName,
                        const std::string& outOfMemoryErrorClassCacheName,
                        const std::string& stringIndexOutOfBoundsClassCacheName,
                        const std::string& unsupportedOperationExceptionClassCacheName,
                        bool traceCallLog, codegen::CForwardDeclarations* fwd)
{
    // resolved native dispatch 的类型定义必须先于 cache 声明（cache 声明里
    // 有 mj2x_regc_resolved_call_site_t 静态变量）。帮手函数体在 finalize 阶段
    // 的表之后发射，前向声明由 fwd 注册表渲染在此处。
    out.append("typedef void (*mj2x_regc_fn_ptr_t)(void);\n");
    out.append("typedef struct mj2x_regc_resolved_fn {\n");
    out.append("    const char* owner;\n");
    out.append("    const char* name;\n");
    out.append("    const char* desc;\n");
    out.append("    jclass ownerClass;\n");
    out.append("    mj2x_regc_fn_ptr_t fn;\n");
    out.append("} mj2x_regc_resolved_fn_t;\n");
    out.append("typedef struct mj2x_regc_resolved_call_site {\n");
    out.append("    jmethodID method;\n");
    out.append("    mj2x_regc_fn_ptr_t fn;\n");
    out.append("    /* fn 声明类（global ref）。快路径 IsInstanceOf(rcv, implClass) 单 JNI\n");
    out.append("       命中即复用 method/fn（假设 implClass 之下无未转换 override）。\n");
    out.append("       无已转换实现（fn==NULL）时 implClass 为 NULL。 */\n");
    out.append("    jclass implClass;\n");
    out.append("    /* 最近解析的 receiver 精确类（global ref）。fn==NULL 的调用点仍\n");
    out.append("       按类缓存 jmethodID：GetObjectClass + IsSameObject 双 JNI 命中。 */\n");
    out.append("    jclass receiverClass;\n");
    out.append("    /* 槽 1（交替 receiver 优化，issue #780）：与槽 0 完全独立的\n");
    out.append("       {method, fn, implClass, receiverClass} 完整组。refresh 只写槽 1，\n");
    out.append("       槽 0 保留上次命中——A/B 交替 receiver 时两槽各自命中，避免每轮\n");
    out.append("       refresh（每轮 5-6 个 JNI）。命中槽 1 时两槽交换，让热 receiver\n");
    out.append("       留在槽 0。method/fn/implClass/receiverClass 必须来自同一次解析，\n");
    out.append("       槽 1 单独成组，杜绝 methodID 与 receiver 错配。 */\n");
    out.append("    jmethodID method1;\n");
    out.append("    mj2x_regc_fn_ptr_t fn1;\n");
    out.append("    jclass implClass1;\n");
    out.append("    jclass receiverClass1;\n");
    out.append("} mj2x_regc_resolved_call_site_t;\n");
    out.append("\n");
    if (fwd != nullptr && !fwd->empty()) {
        const std::string declarations = fwd->render();
        out.append(declarations.data(), declarations.size());
        out.append("\n");
    }
    if (!cacheDeclarations.empty()) {
        out.append(cacheDeclarations.data(), cacheDeclarations.size());
    }
    out.append("static jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache);\n");
    out.append("\n");
    out.append("// Per-thread env/ctx: concurrent native calls from multiple threads must not\n");
    out.append("// share JNIEnv state. C++ uses thread_local; MSVC C uses __declspec(thread);\n");
    out.append("// other C11 uses _Thread_local. Keep comments ASCII for MSVC codepage safety.\n");
    out.append("#if defined(__cplusplus)\n");
    out.append("static thread_local JNIEnv* _mj2x_regc_env = NULL;\n");
    out.append("static thread_local mj2x_regc_context _mj2x_regc_ctx;\n");
    out.append("static thread_local jclass _mj2x_regc_owner_class = NULL;\n");
    out.append("#elif defined(_MSC_VER)\n");
    out.append("static __declspec(thread) JNIEnv* _mj2x_regc_env = NULL;\n");
    out.append("static __declspec(thread) mj2x_regc_context _mj2x_regc_ctx;\n");
    out.append("static __declspec(thread) jclass _mj2x_regc_owner_class = NULL;\n");
    out.append("#else\n");
    out.append("static _Thread_local JNIEnv* _mj2x_regc_env = NULL;\n");
    out.append("static _Thread_local mj2x_regc_context _mj2x_regc_ctx;\n");
    out.append("static _Thread_local jclass _mj2x_regc_owner_class = NULL;\n");
    out.append("#endif\n");
    out.append("static mj2x_regc_context* _mj2x_regc_current_context(void) { return &_mj2x_regc_ctx; "
           "}\n\n");
    out.append("static float _mj2x_regc_f32_from_bits(uint32_t bits) {\n");
    out.append("    float value;\n");
    out.append("    memcpy(&value, &bits, sizeof(value));\n");
    out.append("    return value;\n");
    out.append("}\n\n");
    out.append("static double _mj2x_regc_f64_from_bits(uint64_t bits) {\n");
    out.append("    double value;\n");
    out.append("    memcpy(&value, &bits, sizeof(value));\n");
    out.append("    return value;\n");
    out.append("}\n\n");
    out.append("static void _mj2x_regc_throw_runtime(void* env, const char* class_name, const char* "
           "message) {\n");
    out.append("    JNIEnv* jni = (JNIEnv*)env;\n");
    out.append("    if (jni == NULL || class_name == NULL) return;\n");
    out.append("    jclass ex = (*jni)->FindClass(jni, class_name);\n");
    out.append("    if (ex != NULL) (*jni)->ThrowNew(jni, ex, message != NULL ? message : \"\");\n");
    out.append("}\n\n");
    out.append("static void _mj2x_regc_throw_unsupported(const char* feature) {\n");
    out.append("    jclass ex = _mj2x_regc_cached_class(&")
        .append(unsupportedOperationExceptionClassCacheName.data(), unsupportedOperationExceptionClassCacheName.size())
        .append(");\n");
    out.append("    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, feature);\n");
    out.append("}\n\n");
    out.append("static void _mj2x_regc_throw_null(const char* message) {\n");
    out.append("    jclass ex = _mj2x_regc_cached_class(&")
        .append(nullPointerExceptionClassCacheName.data(), nullPointerExceptionClassCacheName.size())
        .append(");\n");
    out.append("    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, message);\n");
    out.append("}\n\n");
    out.append("static jclass _mj2x_regc_find_class(const char* desc) {\n");
    out.append("    if (desc == NULL) return NULL;\n");
    out.append("    if (desc[0] == 'L') {\n");
    out.append("        char name[1024];\n");
    out.append("        int i = 0;\n");
    out.append("        while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = "
           "desc[i + 1]; i++; }\n");
    out.append("        name[i] = '\\0';\n");
    out.append("        return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, name);\n");
    out.append("    }\n");
    out.append("    return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, desc);\n");
    out.append("}\n\n");
    out.append("static jclass _mj2x_regc_global_class(const char* desc, jclass* slot) {\n");
    out.append("    if (slot == NULL) return NULL;\n");
    out.append("    if (*slot != NULL) return *slot;\n");
    out.append("    jclass local = _mj2x_regc_find_class(desc);\n");
    out.append("    if (local == NULL) return NULL;\n");
    out.append("    jclass global = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, local);\n");
    out.append("    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, local);\n");
    out.append("    if (global == NULL) return NULL;\n");
    out.append("    *slot = global;\n");
    out.append("    return global;\n");
    out.append("}\n\n");
    out.append("static jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache) {\n");
    out.append("    if (cache == NULL) return NULL;\n");
    out.append("    return _mj2x_regc_global_class(cache->desc, &cache->value);\n");
    out.append("}\n\n");
    out.append("static jmethodID _mj2x_regc_cached_static_method(mj2x_regc_static_method_cache* cache) "
           "{\n");
    out.append("    if (cache == NULL) return NULL;\n");
    out.append("    if (cache->method != NULL) return cache->method;\n");
    out.append("    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n");
    out.append("    if (owner == NULL) return NULL;\n");
    out.append("    jmethodID method = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n");
    out.append("    if (method == NULL) return NULL;\n");
    out.append("    cache->method = method;\n");
    out.append("    return method;\n");
    out.append("}\n\n");
    out.append("// Resolve the jmethodID for an instance method. Correctness: the id MUST be\n");
    out.append("// resolved against the receiver's actual class, not the declared owner. When the\n");
    out.append("// owner is abstract/interface (e.g. java/io/InputStream.read), GetMethodID on the\n");
    out.append("// owner yields the abstract entry and CallXMethod triggers AbstractMethodError or\n");
    out.append("// crashes on the JIT inlined path (observed ACCESS_VIOLATION on HotSpot 17).\n");
    out.append("// Caching: reuse the cached id only when the receiver's runtime class equals the\n");
    out.append("// declared owner (covers the common same-type repeated calls); write back only in\n");
    out.append("// that case. Interface/abstract dispatch additionally keeps one per-receiver-class\n");
    out.append("// slot (lastReceiverClass + lastReceiverMethod, global-ref pinned): repeated calls on\n");
    out.append("// the same concrete class reuse the resolved id after an IsSameObject check, while a\n");
    out.append("// different subclass re-resolves instead of reusing a misleading id.\n");
    out.append("static jmethodID _mj2x_regc_cached_instance_method("
            "mj2x_regc_instance_method_cache* cache, jobject receiver) {\n");
    out.append("    if (cache == NULL || receiver == NULL) return NULL;\n");
    out.append("    jclass receiver_class = (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, "
            "receiver);\n");
    out.append("    if (receiver_class == NULL) return NULL;\n");
    out.append("    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n");
    out.append("    /* Fast path: receiver class == declared owner, reuse cached id. */\n");
    out.append("    if (owner != NULL && (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, owner, "
            "receiver_class) && cache->method != NULL) {\n");
    out.append("        (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);\n");
    out.append("        return cache->method;\n");
    out.append("    }\n");
    out.append("    /* Subclass/interface path: reuse the per-receiver-class slot when the class matches. */\n");
    out.append("    if (cache->lastReceiverClass != NULL &&\n");
    out.append("        (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, cache->lastReceiverClass, "
            "receiver_class)) {\n");
    out.append("        (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);\n");
    out.append("        return cache->lastReceiverMethod;\n");
    out.append("    }\n");
    out.append("    jmethodID method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, receiver_class, "
            "cache->name, cache->desc);\n");
    out.append("    if (method == NULL) { (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, "
            "receiver_class); return NULL; }\n");
    out.append("    /* Write back only when receiver class == declared owner; never for subclasses. */\n");
    out.append("    if (owner != NULL && (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, owner, "
            "receiver_class)) {\n");
    out.append("        cache->method = method;\n");
    out.append("    }\n");
    out.append("    else {\n");
    out.append("        /* Pin the receiver class and its resolved id for the next same-class call. */\n");
    out.append("        if (cache->lastReceiverClass != NULL) {\n");
    out.append("            (*_mj2x_regc_env)->DeleteGlobalRef(_mj2x_regc_env, "
            "cache->lastReceiverClass);\n");
    out.append("            cache->lastReceiverClass = NULL;\n");
    out.append("        }\n");
    out.append("        cache->lastReceiverClass = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, "
            "receiver_class);\n");
    out.append("        cache->lastReceiverMethod = method;\n");
    out.append("    }\n");
    out.append("    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);\n");
    out.append("    return method;\n");
    out.append("}\n\n");
    out.append("/* 按声明 owner 解析的实例方法缓存（libraryOwner 调用点专用）：GetMethodID 只\n");
    out.append(" * 解析一次并缓存，之后每次调用 0 JNI（相比 _mj2x_regc_cached_instance_method 的\n");
    out.append(" * 每调用 GetObjectClass+IsSameObject）。CallXxxMethodA 对任意 receiver 做 JNI 虚\n");
    out.append(" * 分派（按实际对象类找覆写），methodID 来自声明类即可，语义正确；对接口 owner\n");
    out.append(" * 同样适用（HotSpot 支持接口 methodID）。解析失败清异常防 pending 挂起。 */\n");
    out.append("static jmethodID _mj2x_regc_cached_owner_method("
            "mj2x_regc_instance_method_cache* cache, jobject receiver) {\n");
    out.append("    if (cache == NULL || receiver == NULL) return NULL;\n");
    out.append("    if (cache->method != NULL) return cache->method;\n");
    out.append("    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n");
    out.append("    if (owner == NULL) return NULL;\n");
    out.append("    cache->method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, owner, cache->name, "
            "cache->desc);\n");
    out.append("    if (cache->method == NULL) {\n");
    out.append("        (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env);\n");
    out.append("    }\n");
    out.append("    return cache->method;\n");
    out.append("}\n\n");
    out.append("static jfieldID _mj2x_regc_cached_static_field(mj2x_regc_static_field_cache* cache) "
           "{\n");
    out.append("    if (cache == NULL) return NULL;\n");
    out.append("    if (cache->field != NULL) return cache->field;\n");
    out.append("    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n");
    out.append("    if (owner == NULL) return NULL;\n");
    out.append("    jfieldID field = (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n");
    out.append("    if (field == NULL) return NULL;\n");
    out.append("    cache->field = field;\n");
    out.append("    return field;\n");
    out.append("}\n\n");
    out.append("static jfieldID _mj2x_regc_cached_instance_field("
           "mj2x_regc_instance_field_cache* cache) {\n");
    out.append("    if (cache == NULL) return NULL;\n");
    out.append("    if (cache->field != NULL) return cache->field;\n");
    out.append("    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n");
    out.append("    if (owner == NULL) return NULL;\n");
    out.append("    jfieldID field = (*_mj2x_regc_env)->GetFieldID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n");
    out.append("    if (field == NULL) return NULL;\n");
    out.append("    cache->field = field;\n");
    out.append("    return field;\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc) {\n");
    out.append("    jclass cls = _mj2x_regc_find_class(type_desc);\n");
    out.append("    if (cls == NULL) return NULL;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache) {\n");
    out.append("    jclass cls = _mj2x_regc_cached_class(cache);\n");
    out.append("    if (cls == NULL) return NULL;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length) {\n");
    out.append("    if (type_desc == NULL || type_desc[0] != '[') return NULL;\n");
    out.append("    switch (type_desc[1]) {\n");
    out.append("    case 'Z': return (mj2x_ref)(*_mj2x_regc_env)->NewBooleanArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'B': return (mj2x_ref)(*_mj2x_regc_env)->NewByteArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'C': return (mj2x_ref)(*_mj2x_regc_env)->NewCharArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'S': return (mj2x_ref)(*_mj2x_regc_env)->NewShortArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'I': return (mj2x_ref)(*_mj2x_regc_env)->NewIntArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'J': return (mj2x_ref)(*_mj2x_regc_env)->NewLongArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'F': return (mj2x_ref)(*_mj2x_regc_env)->NewFloatArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    case 'D': return (mj2x_ref)(*_mj2x_regc_env)->NewDoubleArray(_mj2x_regc_env, "
           "length);\n");
    out.append("    default: return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, "
           "length, _mj2x_regc_find_class(type_desc + 1), NULL);\n");
    out.append("    }\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_ref_array_cached("
           "mj2x_regc_class_cache* component_cache, int32_t length) {\n");
    out.append("    jclass component = _mj2x_regc_cached_class(component_cache);\n");
    out.append("    if (component == NULL) return NULL;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, length, "
           "component, NULL);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, "
           "int32_t* dims) {\n");
    out.append("    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') "
           "{\n");
    out.append("        _mj2x_regc_throw_unsupported(\"RegC multi-array helper received a malformed "
           "descriptor\");\n");
    out.append("        return NULL;\n");
    out.append("    }\n");
    out.append("    if (dim_count == 1) return _mj2x_regc_alloc_array(type_desc, dims[0]);\n");
    out.append("    jclass component = _mj2x_regc_find_class(type_desc + 1);\n");
    out.append("    if (component == NULL) return NULL;\n");
    out.append("    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], "
           "component, NULL);\n");
    out.append("    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("    /* Java multi-array allocation eagerly allocates every requested dimension.\n");
    out.append("       Dimensions beyond dim_count stay NULL, matching MULTIANEWARRAY partial forms. "
           "*/\n");
    out.append("    for (int32_t i = 0; i < dims[0]; ++i) {\n");
    out.append("        mj2x_ref child = _mj2x_regc_alloc_multi_array(type_desc + 1, dim_count - 1, "
           "dims + 1);\n");
    out.append("        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, "
           "(jobject)child);\n");
    out.append("        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("    }\n");
    out.append("    return (mj2x_ref)array;\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_alloc_multi_array_cached(const char* type_desc, "
           "int32_t dim_count, int32_t* dims, mj2x_regc_class_cache** component_caches) {\n");
    out.append("    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') "
           "{\n");
    out.append("        _mj2x_regc_throw_unsupported(\"RegC cached multi-array helper received a "
           "malformed descriptor\");\n");
    out.append("        return NULL;\n");
    out.append("    }\n");
    out.append("    if (dim_count == 1) {\n");
    out.append("        if (type_desc[1] == 'L' || type_desc[1] == '[') {\n");
    out.append("            return _mj2x_regc_alloc_ref_array_cached(component_caches[0], dims[0]);\n");
    out.append("        }\n");
    out.append("        return _mj2x_regc_alloc_array(type_desc, dims[0]);\n");
    out.append("    }\n");
    out.append("    if (component_caches == NULL || component_caches[0] == NULL) return NULL;\n");
    out.append("    jclass component = _mj2x_regc_cached_class(component_caches[0]);\n");
    out.append("    if (component == NULL) return NULL;\n");
    out.append("    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], "
           "component, NULL);\n");
    out.append("    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("    for (int32_t i = 0; i < dims[0]; ++i) {\n");
    out.append("        mj2x_ref child = _mj2x_regc_alloc_multi_array_cached(type_desc + 1, "
           "dim_count - 1, dims + 1, component_caches + 1);\n");
    out.append("        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, "
           "(jobject)child);\n");
    out.append("        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n");
    out.append("    }\n");
    out.append("    return (mj2x_ref)array;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_regc_array_length(mj2x_ref array_ref) {\n");
    out.append("    if (array_ref == NULL) { _mj2x_regc_throw_null(\"array.length\"); return 0; }\n");
    out.append("    return (int32_t)(*_mj2x_regc_env)->GetArrayLength(_mj2x_regc_env, "
           "(jarray)array_ref);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc) {\n");
    out.append("    if (value == NULL) return NULL;\n");
    out.append("    jclass cls = _mj2x_regc_find_class(type_desc);\n");
    out.append("    if (cls == NULL) return NULL;\n");
    out.append("    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return "
           "value;\n");
    out.append("    jclass ex = _mj2x_regc_cached_class(&")
        .append(classCastExceptionClassCacheName.data(), classCastExceptionClassCacheName.size())
        .append(");\n");
    out.append("    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, type_desc);\n");
    out.append("    return NULL;\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache) {\n");
    out.append("    if (value == NULL) return NULL;\n");
    out.append("    jclass cls = _mj2x_regc_cached_class(cache);\n");
    out.append("    if (cls == NULL) return NULL;\n");
    out.append("    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return "
           "value;\n");
    out.append("    jclass ex = _mj2x_regc_cached_class(&")
        .append(classCastExceptionClassCacheName.data(), classCastExceptionClassCacheName.size())
        .append(");\n");
    out.append("    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, cache != NULL ? "
           "cache->desc : \"checkcast\");\n");
    out.append("    return NULL;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc) {\n");
    out.append("    if (value == NULL) return 0;\n");
    out.append("    jclass cls = _mj2x_regc_find_class(type_desc);\n");
    out.append("    if (cls == NULL) return 0;\n");
    out.append("    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache) {\n");
    out.append("    if (value == NULL) return 0;\n");
    out.append("    jclass cls = _mj2x_regc_cached_class(cache);\n");
    out.append("    if (cls == NULL) return 0;\n");
    out.append("    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n");
    out.append("}\n\n");
    out.append("static void _mj2x_regc_monitor_enter(mj2x_ref value) { "
           "(*_mj2x_regc_env)->MonitorEnter(_mj2x_regc_env, (jobject)value); }\n");
    out.append("static void _mj2x_regc_monitor_exit(mj2x_ref value) { "
           "(*_mj2x_regc_env)->MonitorExit(_mj2x_regc_env, (jobject)value); }\n");
    out.append("static int32_t _mj2x_regc_exception_check(void) { return "
           "(*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env) ? 1 : 0; }\n");
    out.append("static mj2x_ref _mj2x_regc_exception_occurred(void) { return "
           "(mj2x_ref)(*_mj2x_regc_env)->ExceptionOccurred(_mj2x_regc_env); }\n");
    out.append("static void _mj2x_regc_exception_clear(void) { "
           "(*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); }\n");
    out.append("static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type) "
           "{\n");
    out.append("    if (catch_type == NULL || catch_type[0] == '\\0') return 1;\n");
    out.append("    if (value == NULL) return 0;\n");
    out.append("    jclass cls = _mj2x_regc_find_class(catch_type);\n");
    out.append("    if (cls == NULL) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); return 0; "
           "}\n");
    out.append("    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n");
    out.append("}\n");
    out.append("static void _mj2x_regc_throw(mj2x_ref value) {\n");
    out.append("    if (value == NULL) {\n");
    out.append("        jclass ex = _mj2x_regc_cached_class(&")
        .append(nullPointerExceptionClassCacheName.data(), nullPointerExceptionClassCacheName.size())
        .append(");\n");
    out.append("        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, \"athrow "
           "null\");\n");
    out.append("        return;\n");
    out.append("    }\n");
    out.append("    (*_mj2x_regc_env)->Throw(_mj2x_regc_env, (jthrowable)value);\n");
    out.append("}\n");
    // Static and instance helpers share code, but the generated C passes an
    // explicit mode bit so a null instance receiver still throws NPE.
    emitRegCFieldAccessHelpers(out);
    out.append("static mj2x_regc_class_cache _mj2x_regc_boolean_array_cache = {\"[Z\", NULL};\n");
    out.append("static int32_t _mj2x_regc_is_boolean_array(mj2x_ref array_ref) { jclass c = "
           "_mj2x_regc_cached_class(&_mj2x_regc_boolean_array_cache); return c != NULL && "
           "(*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)array_ref, c); }\n");
    out.append("static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } if "
           "(_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = JNI_FALSE; "
           "(*_mj2x_regc_env)->GetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, "
           "index, 1, &v); return v == JNI_TRUE ? 1 : 0; } jbyte v = 0; "
           "(*_mj2x_regc_env)->GetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, "
           "&v); return (int8_t)v; }\n");
    out.append("static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jshort v = 0; "
           "(*_mj2x_regc_env)->GetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, "
           "1, &v); return (int16_t)v; }\n");
    out.append("static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jchar v = 0; "
           "(*_mj2x_regc_env)->GetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, "
           "&v); return (uint16_t)v; }\n");
    out.append("static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jint v = 0; "
           "(*_mj2x_regc_env)->GetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, "
           "&v); return v; }\n");
    out.append("static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jlong v = 0; "
           "(*_mj2x_regc_env)->GetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, "
           "&v); return v; }\n");
    out.append("static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0f; } jfloat v = "
           "0; (*_mj2x_regc_env)->GetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, "
           "index, 1, &v); return v; }\n");
    out.append("static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0; } jdouble v = "
           "0; (*_mj2x_regc_env)->GetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, "
           "index, 1, &v); return v; }\n");
    out.append("static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return NULL; } return "
           "(mj2x_ref)(*_mj2x_regc_env)->GetObjectArrayElement(_mj2x_regc_env, "
           "(jobjectArray)array_ref, index); }\n");
    out.append("static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value) "
           "{ if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } if "
           "(_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = value != 0 ? JNI_TRUE : "
           "JNI_FALSE; (*_mj2x_regc_env)->SetBooleanArrayRegion(_mj2x_regc_env, "
           "(jbooleanArray)array_ref, index, 1, &v); return; } jbyte v = (jbyte)value; "
           "(*_mj2x_regc_env)->SetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, "
           "&v); }\n");
    out.append("static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jshort v = (jshort)value; (*_mj2x_regc_env)->SetShortArrayRegion(_mj2x_regc_env, "
           "(jshortArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jchar v = (jchar)value; (*_mj2x_regc_env)->SetCharArrayRegion(_mj2x_regc_env, "
           "(jcharArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jint v = value; (*_mj2x_regc_env)->SetIntArrayRegion(_mj2x_regc_env, "
           "(jintArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jlong v = value; (*_mj2x_regc_env)->SetLongArrayRegion(_mj2x_regc_env, "
           "(jlongArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value) "
           "{ if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jfloat v "
           "= value; (*_mj2x_regc_env)->SetFloatArrayRegion(_mj2x_regc_env, "
           "(jfloatArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jdouble v = value; (*_mj2x_regc_env)->SetDoubleArrayRegion(_mj2x_regc_env, "
           "(jdoubleArray)array_ref, index, 1, &v); }\n");
    out.append("static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "(*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, "
           "index, (jobject)value); }\n\n");
    out.append("static const char* _mj2x_regc_skip_type(const char* p) {\n");
    out.append("    while (*p == '[') p++;\n");
    out.append("    if (*p == 'L') { while (*p != ';' && *p != '\\0') p++; return *p == ';' ? p + 1 : "
           "p; }\n");
    out.append("    return *p == '\\0' ? p : p + 1;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_regc_count_params(const char* desc) {\n");
    out.append("    int32_t count = 0;\n");
    out.append("    const char* p = desc + 1;\n");
    out.append("    while (*p != ')' && *p != '\\0') { p = _mj2x_regc_skip_type(p); count++; }\n");
    out.append("    return count;\n");
    out.append("}\n\n");
    out.append("static jobject _mj2x_regc_collect_args(const char* desc, int32_t arg_count, va_list "
           "ap, jvalue* values) {\n");
    out.append("    int32_t param_count = _mj2x_regc_count_params(desc);\n");
    out.append("    if (param_count > 32) { _mj2x_regc_throw_unsupported(\"method call exceeds 32 "
           "parameters (jvalue values[32])\"); return NULL; }\n");
    out.append("    jobject receiver = NULL;\n");
    out.append("    if (arg_count == param_count + 1) receiver = (jobject)va_arg(ap, mj2x_ref);\n");
    out.append("    const char* p = desc + 1;\n");
    out.append("    int32_t i = 0;\n");
    out.append("    while (*p != ')' && *p != '\\0' && i < 32) {\n");
    out.append("        char tag = *p;\n");
    out.append("        /* Array params ([X) are references (jobject), NOT the element primitive.\n");
    out.append("           Previously the '[' was consumed and tag became the element type, so a\n");
    out.append("           byte[] arg was read as a jbyte via va_arg(ap,int) -- truncating the\n");
    out.append("           pointer and corrupting the constructor call (e.g. ByteArrayInputStream.<init>).\n");
    out.append("           Track array-ness and route to the .l slot. */\n");
    out.append("        int32_t is_array = 0;\n");
    out.append("        while (tag == '[') { is_array = 1; tag = *++p; }\n");
    out.append("        if (is_array || tag == 'L') {\n");
    out.append("            values[i].l = (jobject)va_arg(ap, mj2x_ref);\n");
    out.append("        } else {\n");
    out.append("            switch (tag) {\n");
    out.append("        case 'Z': values[i].z = (jboolean)va_arg(ap, int); break;\n");
    out.append("        case 'B': values[i].b = (jbyte)va_arg(ap, int); break;\n");
    out.append("        case 'C': values[i].c = (jchar)va_arg(ap, int); break;\n");
    out.append("        case 'S': values[i].s = (jshort)va_arg(ap, int); break;\n");
    out.append("        case 'I': values[i].i = (jint)va_arg(ap, int); break;\n");
    out.append("        case 'J': values[i].j = (jlong)va_arg(ap, int64_t); break;\n");
    out.append("        case 'F': values[i].f = (jfloat)va_arg(ap, double); break;\n");
    out.append("        case 'D': values[i].d = (jdouble)va_arg(ap, double); break;\n");
    out.append("        default: values[i].l = (jobject)va_arg(ap, mj2x_ref); break;\n");
    out.append("        }\n");
    out.append("        }\n");
    out.append("        p = _mj2x_regc_skip_type(p);\n");
    out.append("        i++;\n");
    out.append("    }\n");
    out.append("    return receiver;\n");
    out.append("}\n\n");
    emitRegCMethodCallHelpers(out);
    if (traceCallLog) {
        // 多线程下深度计数按线程独立（仅调试面，_Thread_local C11 起可用）。
        out.append("static _Thread_local int _mj2x_regc_dbg_depth = 0;\n");
        out.append("static void _mj2x_regc_dbg_enter(const char* owner, const char* name, const char* "
                   "desc) {\n");
        out.append("    _mj2x_regc_dbg_depth++;\n");
        out.append("    if (_mj2x_regc_dbg_depth > 64) {\n");
        out.append("        fprintf(stderr, \"DEEP[%d] %s.%s%s\\n\", _mj2x_regc_dbg_depth, owner, name, "
                   "desc);\n");
        out.append("    }\n");
        out.append("}\n");
        out.append("static void _mj2x_regc_dbg_leave() { _mj2x_regc_dbg_depth--; }\n");
        out.append("static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* "
                   "cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, "
                   "arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, "
                   "values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null(\"instance "
                   "method call\"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, "
                   "receiver); if (m == NULL) return; fprintf(stderr, \"[CALL:%p] %s.%s%s\\n\", "
                   "(void*)_mj2x_regc_env, cache->owner, cache->name, cache->desc); "
                   "_mj2x_regc_dbg_enter(cache->owner, cache->name, cache->desc); "
                   "(*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); "
                   "_mj2x_regc_dbg_leave(); }\n");
    }
    else {
        out.append("static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* "
                   "cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, "
                   "arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, "
                   "values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null(\"instance "
                   "method call\"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, "
                   "receiver); if (m == NULL) return; (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, "
                   "receiver, m, values); }\n");
    }
    // String LDC intrinsic helper.  This is the only JNI touchpoint of the
    // String intrinsic path: a String literal becomes a jstring via
    // NewStringUTF so it can flow into parseInt/toString without per-call JNI
    // method dispatch.  context is accepted for ABI symmetry with future
    // throwing String intrinsics.
    // 字符串常量按内容缓存：每次 NewStringUTF 会产生新 jstring 且每次 1 个 JNI
    // + 1 个 local ref，热路径（如 Crypto 探针每迭代 4 处常量）下开销显著。
    // global ref 跨帧安全（函数返回后 local ref 失效，静态缓存必须持 global）。
    out.append("static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* "
           "utf8) {\n");
    out.append("    (void)context;\n");
    out.append("    if (utf8 == NULL) return NULL;\n");
    out.append("    static struct { const char* key; jstring value; } literal_cache[32];\n");
    out.append("    static int literal_cache_count = 0;\n");
    out.append("    for (int i = 0; i < literal_cache_count; ++i) {\n");
    out.append("        if (literal_cache[i].key == utf8 ||\n");
    out.append("            strcmp(literal_cache[i].key, utf8) == 0) {\n");
    out.append("            return literal_cache[i].value;\n");
    out.append("        }\n");
    out.append("    }\n");
    out.append("    jstring s = (jstring)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, utf8);\n");
    out.append("    if (s != NULL) {\n");
    out.append("        s = (jstring)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, s);\n");
    out.append("    }\n");
    out.append("    if (s != NULL && literal_cache_count < 32) {\n");
    out.append("        /* 索引按位保护：多线程首次并发的瞬时竞态最多覆盖已有槽，不越界 */\n");
    out.append("        literal_cache[literal_cache_count & 31].key = utf8;\n");
    out.append("        literal_cache[literal_cache_count & 31].value = s;\n");
    out.append("        ++literal_cache_count;\n");
    out.append("    }\n");
    out.append("    return (mj2x_ref)s;\n");
    out.append("}\n\n");
    // LDC Class constant (Foo.class) materialization: FindClass returns a jclass.
    // Class objects are JVM singletons; local-ref lifetime is covered by the
    // R6-D reference management frame. context is for ABI symmetry.
    out.append("static mj2x_ref _mj2x_regc_class_literal(mj2x_regc_context* context, const char* "
           "internal_name) {\n");
    out.append("    (void)context;\n");
    out.append("    if (internal_name == NULL) return NULL;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->FindClass(_mj2x_regc_env, internal_name);\n");
    out.append("}\n\n");
    // Integer.parseInt / Long.parseLong intrinsic helpers.  Reads the jstring's
    // modified-UTF-8 bytes and parses digits locally so radix overloads do not
    // dispatch through CallStatic*Method.
    out.append("static int32_t _mj2x_intrinsic_digit_value(unsigned char ch) {\n");
    out.append("    if (ch >= '0' && ch <= '9') return (int32_t)(ch - '0');\n");
    out.append("    if (ch >= 'a' && ch <= 'z') return (int32_t)(ch - 'a') + 10;\n");
    out.append("    if (ch >= 'A' && ch <= 'Z') return (int32_t)(ch - 'A') + 10;\n");
    out.append("    return -1;\n");
    out.append("}\n\n");
    out.append("static void _mj2x_intrinsic_throw_number_format(mj2x_regc_context* context) {\n");
    out.append("    if (context != NULL && context->throwException != NULL) {\n");
    out.append("        context->throwException(context->env, \"java/lang/NumberFormatException\", "
           "\"For input string\");\n");
    out.append("    }\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n");
    out.append("static int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n");
    out.append("static int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref "
           "jstr_ref) {\n");
    out.append("    return _mj2x_intrinsic_parse_int_radix(context, jstr_ref, 10);\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n");
    out.append("    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); "
           "return 0; }\n");
    out.append("    if (jstr_ref == NULL) {\n");
    out.append("        if (context != NULL && context->throwException != NULL) {\n");
    out.append("            context->throwException(context->env, \"java/lang/NullPointerException\", "
           "\"parseInt\");\n");
    out.append("        }\n");
    out.append("        return 0;\n");
    out.append("    }\n");
    out.append("    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n");
    out.append("    if (utf == NULL) return 0;\n");
    out.append("    const unsigned char* p = (const unsigned char*)utf;\n");
    out.append("    int negative = 0;\n");
    out.append("    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }\n");
    out.append("    uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;\n");
    out.append("    uint64_t value = 0;\n");
    out.append("    int digit_count = 0;\n");
    out.append("    int failed = 0;\n");
    out.append("    while (*p != '\\0') {\n");
    out.append("        int32_t digit = _mj2x_intrinsic_digit_value(*p++);\n");
    out.append("        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / "
           "(uint64_t)radix) { failed = 1; break; }\n");
    out.append("        value = value * (uint64_t)radix + (uint64_t)digit;\n");
    out.append("        digit_count++;\n");
    out.append("    }\n");
    out.append("    if (digit_count == 0) failed = 1;\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "utf);\n");
    out.append("    if (failed) {\n");
    out.append("        _mj2x_intrinsic_throw_number_format(context);\n");
    out.append("        return 0;\n");
    out.append("    }\n");
    out.append("    if (negative) {\n");
    out.append("        if (value == 2147483648ULL) return (int32_t)INT32_MIN;\n");
    out.append("        return -(int32_t)value;\n");
    out.append("    }\n");
    out.append("    return (int32_t)value;\n");
    out.append("}\n\n");
    out.append("static int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref "
           "jstr_ref) {\n");
    out.append("    return _mj2x_intrinsic_parse_long_radix(context, jstr_ref, 10);\n");
    out.append("}\n\n");
    out.append("static int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n");
    out.append("    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); "
           "return 0; }\n");
    out.append("    if (jstr_ref == NULL) {\n");
    out.append("        if (context != NULL && context->throwException != NULL) {\n");
    out.append("            context->throwException(context->env, \"java/lang/NullPointerException\", "
           "\"parseLong\");\n");
    out.append("        }\n");
    out.append("        return 0;\n");
    out.append("    }\n");
    out.append("    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n");
    out.append("    if (utf == NULL) return 0;\n");
    out.append("    const unsigned char* p = (const unsigned char*)utf;\n");
    out.append("    int negative = 0;\n");
    out.append("    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }\n");
    out.append("    uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;\n");
    out.append("    uint64_t value = 0;\n");
    out.append("    int digit_count = 0;\n");
    out.append("    int failed = 0;\n");
    out.append("    while (*p != '\\0') {\n");
    out.append("        int32_t digit = _mj2x_intrinsic_digit_value(*p++);\n");
    out.append("        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / "
           "(uint64_t)radix) { failed = 1; break; }\n");
    out.append("        value = value * (uint64_t)radix + (uint64_t)digit;\n");
    out.append("        digit_count++;\n");
    out.append("    }\n");
    out.append("    if (digit_count == 0) failed = 1;\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "utf);\n");
    out.append("    if (failed) {\n");
    out.append("        _mj2x_intrinsic_throw_number_format(context);\n");
    out.append("        return 0;\n");
    out.append("    }\n");
    out.append("    if (negative) {\n");
    out.append("        if (value == 9223372036854775808ULL) return (int64_t)INT64_MIN;\n");
    out.append("        return -(int64_t)value;\n");
    out.append("    }\n");
    out.append("    return (int64_t)value;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_short(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n");
    out.append("    return _mj2x_intrinsic_parse_short_radix(context, jstr_ref, 10);\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n");
    out.append("    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);\n");
    out.append("    if (_mj2x_regc_exception_check()) return 0;\n");
    out.append("    if (value < INT16_MIN || value > INT16_MAX) { "
           "_mj2x_intrinsic_throw_number_format(context); return 0; }\n");
    out.append("    return (int32_t)(int16_t)value;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_byte(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n");
    out.append("    return _mj2x_intrinsic_parse_byte_radix(context, jstr_ref, 10);\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n");
    out.append("    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);\n");
    out.append("    if (_mj2x_regc_exception_check()) return 0;\n");
    out.append("    if (value < INT8_MIN || value > INT8_MAX) { "
           "_mj2x_intrinsic_throw_number_format(context); return 0; }\n");
    out.append("    return (int32_t)(int8_t)value;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_parse_boolean(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n");
    out.append("    (void)context;\n");
    out.append("    if (jstr_ref == NULL) return 0;\n");
    out.append("    if ((*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)jstr_ref) != 4) "
           "return 0;\n");
    out.append("    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n");
    out.append("    if (chars == NULL) return 0;\n");
    out.append("    int32_t result = ((chars[0] | 0x20) == 't' && (chars[1] | 0x20) == 'r' && "
           "(chars[2] | 0x20) == 'u' && (chars[3] | 0x20) == 'e') ? 1 : 0;\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "chars);\n");
    out.append("    return result;\n");
    out.append("}\n\n");
    out.append("static float _mj2x_intrinsic_parse_float(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n");
    out.append("    (void)context;\n");
    out.append("    jmethodID method = _mj2x_regc_cached_static_method(&")
        .append(floatParseFloatMethodCacheName.data(), floatParseFloatMethodCacheName.size())
        .append(");\n");
    out.append("    if (method == NULL || ")
        .append(floatParseFloatMethodCacheName.data(), floatParseFloatMethodCacheName.size())
        .append(".ownerClass == NULL) return 0.0f;\n");
    out.append("    jvalue args[1]; args[0].l = (jobject)jstr_ref;\n");
    out.append("    return (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, ")
        .append(floatParseFloatMethodCacheName.data(), floatParseFloatMethodCacheName.size())
        .append(".ownerClass, method, args);\n");
    out.append("}\n\n");
    out.append("static double _mj2x_intrinsic_parse_double(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n");
    out.append("    (void)context;\n");
    out.append("    jmethodID method = _mj2x_regc_cached_static_method(&")
        .append(doubleParseDoubleMethodCacheName.data(), doubleParseDoubleMethodCacheName.size())
        .append(");\n");
    out.append("    if (method == NULL || ")
        .append(doubleParseDoubleMethodCacheName.data(), doubleParseDoubleMethodCacheName.size())
        .append(".ownerClass == NULL) return 0.0;\n");
    out.append("    jvalue args[1]; args[0].l = (jobject)jstr_ref;\n");
    out.append("    return (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, ")
        .append(doubleParseDoubleMethodCacheName.data(), doubleParseDoubleMethodCacheName.size())
        .append(".ownerClass, method, args);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_intrinsic_float_to_string(mj2x_regc_context* context, "
           "float value) {\n");
    out.append("    (void)context;\n");
    out.append("    jmethodID value_of = _mj2x_regc_cached_static_method(&")
        .append(stringValueOfFloatMethodCacheName.data(), stringValueOfFloatMethodCacheName.size())
        .append(");\n");
    out.append("    if (value_of == NULL || ")
        .append(stringValueOfFloatMethodCacheName.data(), stringValueOfFloatMethodCacheName.size())
        .append(".ownerClass == NULL) return NULL;\n");
    out.append("    jvalue args[1];\n");
    out.append("    args[0].f = (jfloat)value;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, ")
        .append(stringValueOfFloatMethodCacheName.data(), stringValueOfFloatMethodCacheName.size())
        .append(".ownerClass, value_of, args);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_intrinsic_double_to_string(mj2x_regc_context* context, "
           "double value) {\n");
    out.append("    (void)context;\n");
    out.append("    jmethodID value_of = _mj2x_regc_cached_static_method(&")
        .append(stringValueOfDoubleMethodCacheName.data(), stringValueOfDoubleMethodCacheName.size())
        .append(");\n");
    out.append("    if (value_of == NULL || ")
        .append(stringValueOfDoubleMethodCacheName.data(), stringValueOfDoubleMethodCacheName.size())
        .append(".ownerClass == NULL) return NULL;\n");
    out.append("    jvalue args[1];\n");
    out.append("    args[0].d = (jdouble)value;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, ")
        .append(stringValueOfDoubleMethodCacheName.data(), stringValueOfDoubleMethodCacheName.size())
        .append(".ownerClass, value_of, args);\n");
    out.append("}\n\n");
    out.append("static mj2x_ref _mj2x_intrinsic_object_to_string(mj2x_regc_context* context, "
           "mj2x_ref value) {\n");
    out.append("    (void)context;\n");
    out.append("    if (value == NULL) return (mj2x_ref)(*_mj2x_regc_env)->NewStringUTF("
           "_mj2x_regc_env, \"null\");\n");
    out.append("    jmethodID to_string = _mj2x_regc_cached_instance_method(&")
        .append(objectToStringMethodCacheName.data(), objectToStringMethodCacheName.size())
        .append(", (jobject)value);\n");
    out.append("    if (to_string == NULL) return NULL;\n");
    out.append("    return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, "
           "(jobject)value, to_string);\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_is_string(mj2x_ref value) {\n");
    out.append("    if (value == NULL) return 0;\n");
    out.append("    jclass cls = _mj2x_regc_cached_class(&")
        .append(stringClassCacheName.data(), stringClassCacheName.size())
        .append(");\n");
    out.append("    if (cls == NULL) return 0;\n");
    out.append("    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_equals(mj2x_ref lhs, mj2x_ref rhs) {\n");
    out.append("    if (lhs == NULL) { _mj2x_regc_throw_null(\"String.equals\"); return 0; }\n");
    out.append("    if (lhs == rhs) return 1;\n");
    out.append("    if (rhs == NULL || !_mj2x_intrinsic_is_string(rhs)) return 0;\n");
    out.append("    jsize lhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)lhs);\n");
    out.append("    jsize rhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)rhs);\n");
    out.append("    if (lhs_len != rhs_len) return 0;\n");
    out.append("    if (lhs_len == 0) return 1;\n");
    out.append("    const jchar* lhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)lhs, NULL);\n");
    out.append("    if (lhs_chars == NULL) return 0;\n");
    out.append("    const jchar* rhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)rhs, NULL);\n");
    out.append("    if (rhs_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)lhs, lhs_chars); return 0; }\n");
    out.append("    int32_t same = 1;\n");
    out.append("    for (jsize i = 0; i < lhs_len; ++i) { if (lhs_chars[i] != rhs_chars[i]) { same = "
           "0; break; } }\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)lhs, lhs_chars);\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)rhs, rhs_chars);\n");
    out.append("    return same;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_length(mj2x_ref value) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.length\"); return 0; }\n");
    out.append("    return (int32_t)(*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n");
    out.append("}\n\n");
    out.append("static uint16_t _mj2x_intrinsic_string_char_at(mj2x_ref value, int32_t index) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.charAt\"); return 0; }\n");
    out.append("    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);\n");
    out.append("    if (index < 0 || index >= len) {\n");
    out.append("        jclass ex = _mj2x_regc_cached_class(&")
        .append(stringIndexOutOfBoundsClassCacheName.data(), stringIndexOutOfBoundsClassCacheName.size())
        .append(");\n");
    out.append("        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, "
           "\"String.charAt\");\n");
    out.append("        return 0;\n");
    out.append("    }\n");
    out.append("    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n");
    out.append("    if (chars == NULL) return 0;\n");
    out.append("    uint16_t ch = (uint16_t)chars[index];\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);\n");
    out.append("    return ch;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_hash_code(mj2x_ref value) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.hashCode\"); return 0; }\n");
    out.append("    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);\n");
    out.append("    if (len == 0) return 0;\n");
    out.append("    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n");
    out.append("    if (chars == NULL) return 0;\n");
    out.append("    uint32_t hash = 0;\n");
    out.append("    for (jsize i = 0; i < len; ++i) hash = hash * 31u + (uint32_t)chars[i];\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);\n");
    out.append("    return (int32_t)hash;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_is_empty(mj2x_ref value) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.isEmpty\"); return 0; }\n");
    out.append("    return (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value) == 0;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_starts_with(mj2x_ref value, mj2x_ref prefix) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.startsWith\"); return 0; }\n");
    out.append("    if (prefix == NULL) { _mj2x_regc_throw_null(\"String.startsWith prefix\"); return "
           "0; }\n");
    out.append("    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n");
    out.append("    jsize prefix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)prefix);\n");
    out.append("    if (prefix_len > value_len) return 0;\n");
    out.append("    if (prefix_len == 0) return 1;\n");
    out.append("    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n");
    out.append("    if (value_chars == NULL) return 0;\n");
    out.append("    const jchar* prefix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)prefix, NULL);\n");
    out.append("    if (prefix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return 0; }\n");
    out.append("    int32_t same = 1;\n");
    out.append("    for (jsize i = 0; i < prefix_len; ++i) { if (value_chars[i] != prefix_chars[i]) { "
           "same = 0; break; } }\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)prefix, "
           "prefix_chars);\n");
    out.append("    return same;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_ends_with(mj2x_ref value, mj2x_ref suffix) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.endsWith\"); return 0; }\n");
    out.append("    if (suffix == NULL) { _mj2x_regc_throw_null(\"String.endsWith suffix\"); return 0; "
           "}\n");
    out.append("    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n");
    out.append("    jsize suffix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)suffix);\n");
    out.append("    if (suffix_len > value_len) return 0;\n");
    out.append("    if (suffix_len == 0) return 1;\n");
    out.append("    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n");
    out.append("    if (value_chars == NULL) return 0;\n");
    out.append("    const jchar* suffix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)suffix, NULL);\n");
    out.append("    if (suffix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return 0; }\n");
    out.append("    int32_t same = 1;\n");
    out.append("    jsize start = value_len - suffix_len;\n");
    out.append("    for (jsize i = 0; i < suffix_len; ++i) { if (value_chars[start + i] != "
           "suffix_chars[i]) { same = 0; break; } }\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)suffix, "
           "suffix_chars);\n");
    out.append("    return same;\n");
    out.append("}\n\n");
    out.append("static int32_t _mj2x_intrinsic_string_index_of(mj2x_ref value, mj2x_ref needle) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.indexOf\"); return -1; }\n");
    out.append("    if (needle == NULL) { _mj2x_regc_throw_null(\"String.indexOf needle\"); return -1; "
           "}\n");
    out.append("    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n");
    out.append("    jsize needle_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)needle);\n");
    out.append("    if (needle_len == 0) return 0;\n");
    out.append("    if (needle_len > value_len) return -1;\n");
    out.append("    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n");
    out.append("    if (value_chars == NULL) return -1;\n");
    out.append("    const jchar* needle_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)needle, NULL);\n");
    out.append("    if (needle_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return -1; }\n");
    out.append("    int32_t found = -1;\n");
    out.append("    for (jsize i = 0; i <= value_len - needle_len; ++i) {\n");
    out.append("        jsize j = 0;\n");
    out.append("        while (j < needle_len && value_chars[i + j] == needle_chars[j]) j++;\n");
    out.append("        if (j == needle_len) { found = (int32_t)i; break; }\n");
    out.append("    }\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n");
    out.append("    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)needle, "
           "needle_chars);\n");
    out.append("    return found;\n");
    out.append("}\n\n");
    out.append("static jmethodID _mj2x_intrinsic_string_contains_method = NULL;\n");
    out.append("static int32_t _mj2x_intrinsic_string_contains(mj2x_ref value, mj2x_ref seq) {\n");
    out.append("    if (value == NULL) { _mj2x_regc_throw_null(\"String.contains\"); return 0; }\n");
    out.append("    if (seq == NULL) { _mj2x_regc_throw_null(\"String.contains sequence\"); return 0; "
           "}\n");
    out.append("    if (_mj2x_intrinsic_is_string(seq)) {\n");
    out.append("        return _mj2x_intrinsic_string_index_of(value, seq) >= 0;\n");
    out.append("    }\n");
    out.append("    jclass cls = _mj2x_regc_cached_class(&")
        .append(stringClassCacheName.data(), stringClassCacheName.size())
        .append(");\n");
    out.append("    if (cls == NULL) return 0;\n");
    out.append("    if (_mj2x_intrinsic_string_contains_method == NULL) {\n");
    out.append("        _mj2x_intrinsic_string_contains_method = "
           "(*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, cls, \"contains\", "
           "\"(Ljava/lang/CharSequence;)Z\");\n");
    out.append("        if (_mj2x_intrinsic_string_contains_method == NULL) return 0;\n");
    out.append("    }\n");
    out.append("    return (*_mj2x_regc_env)->CallBooleanMethod(_mj2x_regc_env, (jobject)value, "
           "_mj2x_intrinsic_string_contains_method, (jobject)seq) == JNI_TRUE;\n");
    out.append("}\n\n");
    out.append("// invokedynamic (stage 3) box/unbox helpers: primitive arguments are boxed\n");
    out.append("// into Object[]; invokeWithArguments results are unboxed back into scalar\n");
    out.append("// registers. class/methodID are cached globally to avoid repeated lookups.\n");
    const char* boxDefs[] = {
        "static jobject _mj2x_regc_box_i32(int32_t value) { static jclass c = NULL; static "
        "jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "
        "\"java/lang/Integer\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef("
        "_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID("
        "_mj2x_regc_env, c, \"valueOf\", \"(I)Ljava/lang/Integer;\"); if (c == NULL || m == NULL) "
        "return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }",
        "static jobject _mj2x_regc_box_i64(int64_t value) { static jclass c = NULL; static "
        "jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "
        "\"java/lang/Long\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef("
        "_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID("
        "_mj2x_regc_env, c, \"valueOf\", \"(J)Ljava/lang/Long;\"); if (c == NULL || m == NULL) "
        "return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }",
        "static jobject _mj2x_regc_box_f32(float value) { static jclass c = NULL; static "
        "jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "
        "\"java/lang/Float\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef("
        "_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID("
        "_mj2x_regc_env, c, \"valueOf\", \"(F)Ljava/lang/Float;\"); if (c == NULL || m == NULL) "
        "return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }",
        "static jobject _mj2x_regc_box_f64(double value) { static jclass c = NULL; static "
        "jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "
        "\"java/lang/Double\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef("
        "_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID("
        "_mj2x_regc_env, c, \"valueOf\", \"(D)Ljava/lang/Double;\"); if (c == NULL || m == NULL) "
        "return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }",
        "static int32_t _mj2x_regc_unbox_i32(jobject value) { if (value == NULL) return 0; static "
        "jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)"
        "->FindClass(_mj2x_regc_env, \"java/lang/Integer\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)"
        "->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)"
        "->GetMethodID(_mj2x_regc_env, c, \"intValue\", \"()I\"); if (c == NULL || m == NULL) return 0; "
        "return (*_mj2x_regc_env)->CallIntMethod(_mj2x_regc_env, value, m); }",
        "static int64_t _mj2x_regc_unbox_i64(jobject value) { if (value == NULL) return 0; static "
        "jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)"
        "->FindClass(_mj2x_regc_env, \"java/lang/Long\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)"
        "->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)"
        "->GetMethodID(_mj2x_regc_env, c, \"longValue\", \"()J\"); if (c == NULL || m == NULL) return 0; "
        "return (*_mj2x_regc_env)->CallLongMethod(_mj2x_regc_env, value, m); }",
        "static float _mj2x_regc_unbox_f32(jobject value) { if (value == NULL) return 0.0f; static "
        "jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)"
        "->FindClass(_mj2x_regc_env, \"java/lang/Float\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)"
        "->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)"
        "->GetMethodID(_mj2x_regc_env, c, \"floatValue\", \"()F\"); if (c == NULL || m == NULL) "
        "return 0.0f; return (*_mj2x_regc_env)->CallFloatMethod(_mj2x_regc_env, value, m); }",
        "static double _mj2x_regc_unbox_f64(jobject value) { if (value == NULL) return 0.0; static "
        "jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)"
        "->FindClass(_mj2x_regc_env, \"java/lang/Double\"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)"
        "->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)"
        "->GetMethodID(_mj2x_regc_env, c, \"doubleValue\", \"()D\"); if (c == NULL || m == NULL) "
        "return 0.0; return (*_mj2x_regc_env)->CallDoubleMethod(_mj2x_regc_env, value, m); }",
    };
    for (const char* def : boxDefs) {
        out.append(def);
        out.append("\n");
    }
    out.append("\n");
}
}   // namespace mj2x

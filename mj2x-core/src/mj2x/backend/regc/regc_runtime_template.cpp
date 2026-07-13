/**
 * @file regc_runtime_template.cpp
 * @brief RegC 运行时 C-source 定义发射（从 regc_backend.cpp 抽出，字节不变）。
 *
 * @details 见 regc_runtime_template.hpp。三个函数原先在 regc_backend.cpp 匿名
 * namespace，现搬到此处命名空间 mj2x 下，供 RegCBackend::finalize 调用。
 */
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {

// emitRegCFieldAccessHelpers / emitRegCMethodCallHelpers 用 CSourceStream 包装
// raw-string 发射；对齐 regc_backend.cpp 的 using 约定，避免逐处加 codegen:: 前缀。
using codegen::CSourceStream;

void emitRegCFieldAccessHelpers(std::ostringstream& out)
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
    out << src.str();
}

void emitRegCMethodCallHelpers(std::ostringstream& out)
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
static int32_t _mj2x_regc_call_cached_instance_i32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallIntMethodA(_mj2x_regc_env, receiver, m, values); }
static int64_t _mj2x_regc_call_cached_instance_i64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallLongMethodA(_mj2x_regc_env, receiver, m, values); }
static float _mj2x_regc_call_cached_instance_f32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0f; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return 0.0f; return (*_mj2x_regc_env)->CallFloatMethodA(_mj2x_regc_env, receiver, m, values); }
static double _mj2x_regc_call_cached_instance_f64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return 0.0; return (*_mj2x_regc_env)->CallDoubleMethodA(_mj2x_regc_env, receiver, m, values); }
static mj2x_ref _mj2x_regc_call_cached_instance_ref(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return NULL; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethodA(_mj2x_regc_env, receiver, m, values); }
static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache); if (m == NULL) return; (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); }

)mj2x");
    out << src.str();
}

void emitRuntimeHelpers(std::ostringstream& out, const std::string& cacheDeclarations,
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
                        const std::string& unsupportedOperationExceptionClassCacheName)
{
    if (!cacheDeclarations.empty()) {
        out << cacheDeclarations;
    }
    out << "static jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache);\n";
    out << "\n";
    out << "static JNIEnv* _mj2x_regc_env = NULL;\n\n";
    out << "static mj2x_regc_context _mj2x_regc_ctx;\n";
    out << "static mj2x_regc_context* _mj2x_regc_current_context(void) { return &_mj2x_regc_ctx; "
           "}\n\n";
    out << "static void _mj2x_regc_throw_runtime(void* env, const char* class_name, const char* "
           "message) {\n";
    out << "    JNIEnv* jni = (JNIEnv*)env;\n";
    out << "    if (jni == NULL || class_name == NULL) return;\n";
    out << "    jclass ex = (*jni)->FindClass(jni, class_name);\n";
    out << "    if (ex != NULL) (*jni)->ThrowNew(jni, ex, message != NULL ? message : \"\");\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_throw_unsupported(const char* feature) {\n";
    out << "    jclass ex = _mj2x_regc_cached_class(&"
        << unsupportedOperationExceptionClassCacheName << ");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, feature);\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_throw_null(const char* message) {\n";
    out << "    jclass ex = _mj2x_regc_cached_class(&" << nullPointerExceptionClassCacheName
        << ");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, message);\n";
    out << "}\n\n";
    out << "static jclass _mj2x_regc_find_class(const char* desc) {\n";
    out << "    if (desc == NULL) return NULL;\n";
    out << "    if (desc[0] == 'L') {\n";
    out << "        char name[1024];\n";
    out << "        int i = 0;\n";
    out << "        while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = "
           "desc[i + 1]; i++; }\n";
    out << "        name[i] = '\\0';\n";
    out << "        return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, name);\n";
    out << "    }\n";
    out << "    return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, desc);\n";
    out << "}\n\n";
    out << "static jclass _mj2x_regc_global_class(const char* desc, jclass* slot) {\n";
    out << "    if (slot == NULL) return NULL;\n";
    out << "    if (*slot != NULL) return *slot;\n";
    out << "    jclass local = _mj2x_regc_find_class(desc);\n";
    out << "    if (local == NULL) return NULL;\n";
    out << "    jclass global = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, local);\n";
    out << "    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, local);\n";
    out << "    if (global == NULL) return NULL;\n";
    out << "    *slot = global;\n";
    out << "    return global;\n";
    out << "}\n\n";
    out << "static jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache) {\n";
    out << "    if (cache == NULL) return NULL;\n";
    out << "    return _mj2x_regc_global_class(cache->desc, &cache->value);\n";
    out << "}\n\n";
    out << "static jmethodID _mj2x_regc_cached_static_method(mj2x_regc_static_method_cache* cache) "
           "{\n";
    out << "    if (cache == NULL) return NULL;\n";
    out << "    if (cache->method != NULL) return cache->method;\n";
    out << "    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n";
    out << "    if (owner == NULL) return NULL;\n";
    out << "    jmethodID method = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n";
    out << "    if (method == NULL) return NULL;\n";
    out << "    cache->method = method;\n";
    out << "    return method;\n";
    out << "}\n\n";
    out << "static jmethodID _mj2x_regc_cached_instance_method("
           "mj2x_regc_instance_method_cache* cache) {\n";
    out << "    if (cache == NULL) return NULL;\n";
    out << "    if (cache->method != NULL) return cache->method;\n";
    out << "    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n";
    out << "    if (owner == NULL) return NULL;\n";
    out << "    jmethodID method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n";
    out << "    if (method == NULL) return NULL;\n";
    out << "    cache->method = method;\n";
    out << "    return method;\n";
    out << "}\n\n";
    out << "static jfieldID _mj2x_regc_cached_static_field(mj2x_regc_static_field_cache* cache) "
           "{\n";
    out << "    if (cache == NULL) return NULL;\n";
    out << "    if (cache->field != NULL) return cache->field;\n";
    out << "    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n";
    out << "    if (owner == NULL) return NULL;\n";
    out << "    jfieldID field = (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n";
    out << "    if (field == NULL) return NULL;\n";
    out << "    cache->field = field;\n";
    out << "    return field;\n";
    out << "}\n\n";
    out << "static jfieldID _mj2x_regc_cached_instance_field("
           "mj2x_regc_instance_field_cache* cache) {\n";
    out << "    if (cache == NULL) return NULL;\n";
    out << "    if (cache->field != NULL) return cache->field;\n";
    out << "    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);\n";
    out << "    if (owner == NULL) return NULL;\n";
    out << "    jfieldID field = (*_mj2x_regc_env)->GetFieldID(_mj2x_regc_env, owner, "
           "cache->name, cache->desc);\n";
    out << "    if (field == NULL) return NULL;\n";
    out << "    cache->field = field;\n";
    out << "    return field;\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc) {\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache) {\n";
    out << "    jclass cls = _mj2x_regc_cached_class(cache);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length) {\n";
    out << "    if (type_desc == NULL || type_desc[0] != '[') return NULL;\n";
    out << "    switch (type_desc[1]) {\n";
    out << "    case 'Z': return (mj2x_ref)(*_mj2x_regc_env)->NewBooleanArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'B': return (mj2x_ref)(*_mj2x_regc_env)->NewByteArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'C': return (mj2x_ref)(*_mj2x_regc_env)->NewCharArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'S': return (mj2x_ref)(*_mj2x_regc_env)->NewShortArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'I': return (mj2x_ref)(*_mj2x_regc_env)->NewIntArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'J': return (mj2x_ref)(*_mj2x_regc_env)->NewLongArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'F': return (mj2x_ref)(*_mj2x_regc_env)->NewFloatArray(_mj2x_regc_env, "
           "length);\n";
    out << "    case 'D': return (mj2x_ref)(*_mj2x_regc_env)->NewDoubleArray(_mj2x_regc_env, "
           "length);\n";
    out << "    default: return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, "
           "length, _mj2x_regc_find_class(type_desc + 1), NULL);\n";
    out << "    }\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_ref_array_cached("
           "mj2x_regc_class_cache* component_cache, int32_t length) {\n";
    out << "    jclass component = _mj2x_regc_cached_class(component_cache);\n";
    out << "    if (component == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, length, "
           "component, NULL);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, "
           "int32_t* dims) {\n";
    out << "    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') "
           "{\n";
    out << "        _mj2x_regc_throw_unsupported(\"RegC multi-array helper received a malformed "
           "descriptor\");\n";
    out << "        return NULL;\n";
    out << "    }\n";
    out << "    if (dim_count == 1) return _mj2x_regc_alloc_array(type_desc, dims[0]);\n";
    out << "    jclass component = _mj2x_regc_find_class(type_desc + 1);\n";
    out << "    if (component == NULL) return NULL;\n";
    out << "    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], "
           "component, NULL);\n";
    out << "    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "    /* Java multi-array allocation eagerly allocates every requested dimension.\n";
    out << "       Dimensions beyond dim_count stay NULL, matching MULTIANEWARRAY partial forms. "
           "*/\n";
    out << "    for (int32_t i = 0; i < dims[0]; ++i) {\n";
    out << "        mj2x_ref child = _mj2x_regc_alloc_multi_array(type_desc + 1, dim_count - 1, "
           "dims + 1);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, "
           "(jobject)child);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "    }\n";
    out << "    return (mj2x_ref)array;\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_multi_array_cached(const char* type_desc, "
           "int32_t dim_count, int32_t* dims, mj2x_regc_class_cache** component_caches) {\n";
    out << "    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') "
           "{\n";
    out << "        _mj2x_regc_throw_unsupported(\"RegC cached multi-array helper received a "
           "malformed descriptor\");\n";
    out << "        return NULL;\n";
    out << "    }\n";
    out << "    if (dim_count == 1) {\n";
    out << "        if (type_desc[1] == 'L' || type_desc[1] == '[') {\n";
    out << "            return _mj2x_regc_alloc_ref_array_cached(component_caches[0], dims[0]);\n";
    out << "        }\n";
    out << "        return _mj2x_regc_alloc_array(type_desc, dims[0]);\n";
    out << "    }\n";
    out << "    if (component_caches == NULL || component_caches[0] == NULL) return NULL;\n";
    out << "    jclass component = _mj2x_regc_cached_class(component_caches[0]);\n";
    out << "    if (component == NULL) return NULL;\n";
    out << "    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], "
           "component, NULL);\n";
    out << "    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "    for (int32_t i = 0; i < dims[0]; ++i) {\n";
    out << "        mj2x_ref child = _mj2x_regc_alloc_multi_array_cached(type_desc + 1, "
           "dim_count - 1, dims + 1, component_caches + 1);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, "
           "(jobject)child);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return "
           "(mj2x_ref)array;\n";
    out << "    }\n";
    out << "    return (mj2x_ref)array;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref) {\n";
    out << "    if (array_ref == NULL) { _mj2x_regc_throw_null(\"array.length\"); return 0; }\n";
    out << "    return (int32_t)(*_mj2x_regc_env)->GetArrayLength(_mj2x_regc_env, "
           "(jarray)array_ref);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc) {\n";
    out << "    if (value == NULL) return NULL;\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return "
           "value;\n";
    out << "    jclass ex = _mj2x_regc_cached_class(&" << classCastExceptionClassCacheName
        << ");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, type_desc);\n";
    out << "    return NULL;\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache) {\n";
    out << "    if (value == NULL) return NULL;\n";
    out << "    jclass cls = _mj2x_regc_cached_class(cache);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return "
           "value;\n";
    out << "    jclass ex = _mj2x_regc_cached_class(&" << classCastExceptionClassCacheName
        << ");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, cache != NULL ? "
           "cache->desc : \"checkcast\");\n";
    out << "    return NULL;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc) {\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return 0;\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache) {\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_cached_class(cache);\n";
    out << "    if (cls == NULL) return 0;\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_monitor_enter(mj2x_ref value) { "
           "(*_mj2x_regc_env)->MonitorEnter(_mj2x_regc_env, (jobject)value); }\n";
    out << "static void _mj2x_regc_monitor_exit(mj2x_ref value) { "
           "(*_mj2x_regc_env)->MonitorExit(_mj2x_regc_env, (jobject)value); }\n";
    out << "static int32_t _mj2x_regc_exception_check(void) { return "
           "(*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env) ? 1 : 0; }\n";
    out << "static mj2x_ref _mj2x_regc_exception_occurred(void) { return "
           "(mj2x_ref)(*_mj2x_regc_env)->ExceptionOccurred(_mj2x_regc_env); }\n";
    out << "static void _mj2x_regc_exception_clear(void) { "
           "(*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); }\n";
    out << "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type) "
           "{\n";
    out << "    if (catch_type == NULL || catch_type[0] == '\\0') return 1;\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_find_class(catch_type);\n";
    out << "    if (cls == NULL) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); return 0; "
           "}\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n";
    out << "}\n";
    out << "static void _mj2x_regc_throw(mj2x_ref value) {\n";
    out << "    if (value == NULL) {\n";
    out << "        jclass ex = _mj2x_regc_cached_class(&" << nullPointerExceptionClassCacheName
        << ");\n";
    out << "        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, \"athrow "
           "null\");\n";
    out << "        return;\n";
    out << "    }\n";
    out << "    (*_mj2x_regc_env)->Throw(_mj2x_regc_env, (jthrowable)value);\n";
    out << "}\n";
    // Static and instance helpers share code, but the generated C passes an
    // explicit mode bit so a null instance receiver still throws NPE.
    emitRegCFieldAccessHelpers(out);
    out << "static mj2x_regc_class_cache _mj2x_regc_boolean_array_cache = {\"[Z\", NULL};\n";
    out << "static int32_t _mj2x_regc_is_boolean_array(mj2x_ref array_ref) { jclass c = "
           "_mj2x_regc_cached_class(&_mj2x_regc_boolean_array_cache); return c != NULL && "
           "(*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)array_ref, c); }\n";
    out << "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } if "
           "(_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = JNI_FALSE; "
           "(*_mj2x_regc_env)->GetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, "
           "index, 1, &v); return v == JNI_TRUE ? 1 : 0; } jbyte v = 0; "
           "(*_mj2x_regc_env)->GetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, "
           "&v); return (int8_t)v; }\n";
    out << "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jshort v = 0; "
           "(*_mj2x_regc_env)->GetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, "
           "1, &v); return (int16_t)v; }\n";
    out << "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jchar v = 0; "
           "(*_mj2x_regc_env)->GetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, "
           "&v); return (uint16_t)v; }\n";
    out << "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jint v = 0; "
           "(*_mj2x_regc_env)->GetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, "
           "&v); return v; }\n";
    out << "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jlong v = 0; "
           "(*_mj2x_regc_env)->GetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, "
           "&v); return v; }\n";
    out << "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0f; } jfloat v = "
           "0; (*_mj2x_regc_env)->GetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, "
           "index, 1, &v); return v; }\n";
    out << "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0; } jdouble v = "
           "0; (*_mj2x_regc_env)->GetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, "
           "index, 1, &v); return v; }\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index) { if "
           "(array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return NULL; } return "
           "(mj2x_ref)(*_mj2x_regc_env)->GetObjectArrayElement(_mj2x_regc_env, "
           "(jobjectArray)array_ref, index); }\n";
    out << "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value) "
           "{ if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } if "
           "(_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = value != 0 ? JNI_TRUE : "
           "JNI_FALSE; (*_mj2x_regc_env)->SetBooleanArrayRegion(_mj2x_regc_env, "
           "(jbooleanArray)array_ref, index, 1, &v); return; } jbyte v = (jbyte)value; "
           "(*_mj2x_regc_env)->SetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, "
           "&v); }\n";
    out << "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jshort v = (jshort)value; (*_mj2x_regc_env)->SetShortArrayRegion(_mj2x_regc_env, "
           "(jshortArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jchar v = (jchar)value; (*_mj2x_regc_env)->SetCharArrayRegion(_mj2x_regc_env, "
           "(jcharArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jint v = value; (*_mj2x_regc_env)->SetIntArrayRegion(_mj2x_regc_env, "
           "(jintArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jlong v = value; (*_mj2x_regc_env)->SetLongArrayRegion(_mj2x_regc_env, "
           "(jlongArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value) "
           "{ if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jfloat v "
           "= value; (*_mj2x_regc_env)->SetFloatArrayRegion(_mj2x_regc_env, "
           "(jfloatArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "jdouble v = value; (*_mj2x_regc_env)->SetDoubleArrayRegion(_mj2x_regc_env, "
           "(jdoubleArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref "
           "value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } "
           "(*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, "
           "index, (jobject)value); }\n\n";
    out << "static const char* _mj2x_regc_skip_type(const char* p) {\n";
    out << "    while (*p == '[') p++;\n";
    out << "    if (*p == 'L') { while (*p != ';' && *p != '\\0') p++; return *p == ';' ? p + 1 : "
           "p; }\n";
    out << "    return *p == '\\0' ? p : p + 1;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_count_params(const char* desc) {\n";
    out << "    int32_t count = 0;\n";
    out << "    const char* p = desc + 1;\n";
    out << "    while (*p != ')' && *p != '\\0') { p = _mj2x_regc_skip_type(p); count++; }\n";
    out << "    return count;\n";
    out << "}\n\n";
    out << "static jobject _mj2x_regc_collect_args(const char* desc, int32_t arg_count, va_list "
           "ap, jvalue* values) {\n";
    out << "    int32_t param_count = _mj2x_regc_count_params(desc);\n";
    out << "    jobject receiver = NULL;\n";
    out << "    if (arg_count == param_count + 1) receiver = (jobject)va_arg(ap, mj2x_ref);\n";
    out << "    const char* p = desc + 1;\n";
    out << "    int32_t i = 0;\n";
    out << "    while (*p != ')' && *p != '\\0' && i < 32) {\n";
    out << "        char tag = *p;\n";
    out << "        while (tag == '[') tag = *++p;\n";
    out << "        switch (tag) {\n";
    out << "        case 'Z': values[i].z = (jboolean)va_arg(ap, int); break;\n";
    out << "        case 'B': values[i].b = (jbyte)va_arg(ap, int); break;\n";
    out << "        case 'C': values[i].c = (jchar)va_arg(ap, int); break;\n";
    out << "        case 'S': values[i].s = (jshort)va_arg(ap, int); break;\n";
    out << "        case 'I': values[i].i = (jint)va_arg(ap, int); break;\n";
    out << "        case 'J': values[i].j = (jlong)va_arg(ap, int64_t); break;\n";
    out << "        case 'F': values[i].f = (jfloat)va_arg(ap, double); break;\n";
    out << "        case 'D': values[i].d = (jdouble)va_arg(ap, double); break;\n";
    out << "        default: values[i].l = (jobject)va_arg(ap, mj2x_ref); break;\n";
    out << "        }\n";
    out << "        p = _mj2x_regc_skip_type(p);\n";
    out << "        i++;\n";
    out << "    }\n";
    out << "    return receiver;\n";
    out << "}\n\n";
    emitRegCMethodCallHelpers(out);
    // String LDC intrinsic helper.  This is the only JNI touchpoint of the
    // String intrinsic path: a String literal becomes a jstring via
    // NewStringUTF so it can flow into parseInt/toString without per-call JNI
    // method dispatch.  context is accepted for ABI symmetry with future
    // throwing String intrinsics.
    out << "static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* "
           "utf8) {\n";
    out << "    (void)context;\n";
    out << "    if (utf8 == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, utf8);\n";
    out << "}\n\n";
    // Integer.parseInt / Long.parseLong intrinsic helpers.  Reads the jstring's
    // modified-UTF-8 bytes and parses digits locally so radix overloads do not
    // dispatch through CallStatic*Method.
    out << "static int32_t _mj2x_intrinsic_digit_value(unsigned char ch) {\n";
    out << "    if (ch >= '0' && ch <= '9') return (int32_t)(ch - '0');\n";
    out << "    if (ch >= 'a' && ch <= 'z') return (int32_t)(ch - 'a') + 10;\n";
    out << "    if (ch >= 'A' && ch <= 'Z') return (int32_t)(ch - 'A') + 10;\n";
    out << "    return -1;\n";
    out << "}\n\n";
    out << "static void _mj2x_intrinsic_throw_number_format(mj2x_regc_context* context) {\n";
    out << "    if (context != NULL && context->throwException != NULL) {\n";
    out << "        context->throwException(context->env, \"java/lang/NumberFormatException\", "
           "\"For input string\");\n";
    out << "    }\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n";
    out << "static int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n";
    out << "static int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix);\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref "
           "jstr_ref) {\n";
    out << "    return _mj2x_intrinsic_parse_int_radix(context, jstr_ref, 10);\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n";
    out << "    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); "
           "return 0; }\n";
    out << "    if (jstr_ref == NULL) {\n";
    out << "        if (context != NULL && context->throwException != NULL) {\n";
    out << "            context->throwException(context->env, \"java/lang/NullPointerException\", "
           "\"parseInt\");\n";
    out << "        }\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n";
    out << "    if (utf == NULL) return 0;\n";
    out << "    const unsigned char* p = (const unsigned char*)utf;\n";
    out << "    int negative = 0;\n";
    out << "    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }\n";
    out << "    uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;\n";
    out << "    uint64_t value = 0;\n";
    out << "    int digit_count = 0;\n";
    out << "    int failed = 0;\n";
    out << "    while (*p != '\\0') {\n";
    out << "        int32_t digit = _mj2x_intrinsic_digit_value(*p++);\n";
    out << "        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / "
           "(uint64_t)radix) { failed = 1; break; }\n";
    out << "        value = value * (uint64_t)radix + (uint64_t)digit;\n";
    out << "        digit_count++;\n";
    out << "    }\n";
    out << "    if (digit_count == 0) failed = 1;\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "utf);\n";
    out << "    if (failed) {\n";
    out << "        _mj2x_intrinsic_throw_number_format(context);\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    if (negative) {\n";
    out << "        if (value == 2147483648ULL) return (int32_t)INT32_MIN;\n";
    out << "        return -(int32_t)value;\n";
    out << "    }\n";
    out << "    return (int32_t)value;\n";
    out << "}\n\n";
    out << "static int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref "
           "jstr_ref) {\n";
    out << "    return _mj2x_intrinsic_parse_long_radix(context, jstr_ref, 10);\n";
    out << "}\n\n";
    out << "static int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n";
    out << "    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); "
           "return 0; }\n";
    out << "    if (jstr_ref == NULL) {\n";
    out << "        if (context != NULL && context->throwException != NULL) {\n";
    out << "            context->throwException(context->env, \"java/lang/NullPointerException\", "
           "\"parseLong\");\n";
    out << "        }\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n";
    out << "    if (utf == NULL) return 0;\n";
    out << "    const unsigned char* p = (const unsigned char*)utf;\n";
    out << "    int negative = 0;\n";
    out << "    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }\n";
    out << "    uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;\n";
    out << "    uint64_t value = 0;\n";
    out << "    int digit_count = 0;\n";
    out << "    int failed = 0;\n";
    out << "    while (*p != '\\0') {\n";
    out << "        int32_t digit = _mj2x_intrinsic_digit_value(*p++);\n";
    out << "        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / "
           "(uint64_t)radix) { failed = 1; break; }\n";
    out << "        value = value * (uint64_t)radix + (uint64_t)digit;\n";
    out << "        digit_count++;\n";
    out << "    }\n";
    out << "    if (digit_count == 0) failed = 1;\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "utf);\n";
    out << "    if (failed) {\n";
    out << "        _mj2x_intrinsic_throw_number_format(context);\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    if (negative) {\n";
    out << "        if (value == 9223372036854775808ULL) return (int64_t)INT64_MIN;\n";
    out << "        return -(int64_t)value;\n";
    out << "    }\n";
    out << "    return (int64_t)value;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_short(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n";
    out << "    return _mj2x_intrinsic_parse_short_radix(context, jstr_ref, 10);\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n";
    out << "    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);\n";
    out << "    if (_mj2x_regc_exception_check()) return 0;\n";
    out << "    if (value < INT16_MIN || value > INT16_MAX) { "
           "_mj2x_intrinsic_throw_number_format(context); return 0; }\n";
    out << "    return (int32_t)(int16_t)value;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_byte(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n";
    out << "    return _mj2x_intrinsic_parse_byte_radix(context, jstr_ref, 10);\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref, int32_t radix) {\n";
    out << "    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);\n";
    out << "    if (_mj2x_regc_exception_check()) return 0;\n";
    out << "    if (value < INT8_MIN || value > INT8_MAX) { "
           "_mj2x_intrinsic_throw_number_format(context); return 0; }\n";
    out << "    return (int32_t)(int8_t)value;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_parse_boolean(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n";
    out << "    (void)context;\n";
    out << "    if (jstr_ref == NULL) return 0;\n";
    out << "    if ((*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)jstr_ref) != 4) "
           "return 0;\n";
    out << "    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)jstr_ref, NULL);\n";
    out << "    if (chars == NULL) return 0;\n";
    out << "    int32_t result = ((chars[0] | 0x20) == 't' && (chars[1] | 0x20) == 'r' && "
           "(chars[2] | 0x20) == 'u' && (chars[3] | 0x20) == 'e') ? 1 : 0;\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)jstr_ref, "
           "chars);\n";
    out << "    return result;\n";
    out << "}\n\n";
    out << "static float _mj2x_intrinsic_parse_float(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n";
    out << "    (void)context;\n";
    out << "    jmethodID method = _mj2x_regc_cached_static_method(&"
        << floatParseFloatMethodCacheName << ");\n";
    out << "    if (method == NULL || " << floatParseFloatMethodCacheName
        << ".ownerClass == NULL) return 0.0f;\n";
    out << "    jvalue args[1]; args[0].l = (jobject)jstr_ref;\n";
    out << "    return (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, "
        << floatParseFloatMethodCacheName << ".ownerClass, method, args);\n";
    out << "}\n\n";
    out << "static double _mj2x_intrinsic_parse_double(mj2x_regc_context* context, "
           "mj2x_ref jstr_ref) {\n";
    out << "    (void)context;\n";
    out << "    jmethodID method = _mj2x_regc_cached_static_method(&"
        << doubleParseDoubleMethodCacheName << ");\n";
    out << "    if (method == NULL || " << doubleParseDoubleMethodCacheName
        << ".ownerClass == NULL) return 0.0;\n";
    out << "    jvalue args[1]; args[0].l = (jobject)jstr_ref;\n";
    out << "    return (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, "
        << doubleParseDoubleMethodCacheName << ".ownerClass, method, args);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_intrinsic_float_to_string(mj2x_regc_context* context, "
           "float value) {\n";
    out << "    (void)context;\n";
    out << "    jmethodID value_of = _mj2x_regc_cached_static_method(&"
        << stringValueOfFloatMethodCacheName << ");\n";
    out << "    if (value_of == NULL || " << stringValueOfFloatMethodCacheName
        << ".ownerClass == NULL) return NULL;\n";
    out << "    jvalue args[1];\n";
    out << "    args[0].f = (jfloat)value;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, "
        << stringValueOfFloatMethodCacheName << ".ownerClass, value_of, args);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_intrinsic_double_to_string(mj2x_regc_context* context, "
           "double value) {\n";
    out << "    (void)context;\n";
    out << "    jmethodID value_of = _mj2x_regc_cached_static_method(&"
        << stringValueOfDoubleMethodCacheName << ");\n";
    out << "    if (value_of == NULL || " << stringValueOfDoubleMethodCacheName
        << ".ownerClass == NULL) return NULL;\n";
    out << "    jvalue args[1];\n";
    out << "    args[0].d = (jdouble)value;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, "
        << stringValueOfDoubleMethodCacheName << ".ownerClass, value_of, args);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_intrinsic_object_to_string(mj2x_regc_context* context, "
           "mj2x_ref value) {\n";
    out << "    (void)context;\n";
    out << "    if (value == NULL) return (mj2x_ref)(*_mj2x_regc_env)->NewStringUTF("
           "_mj2x_regc_env, \"null\");\n";
    out << "    jmethodID to_string = _mj2x_regc_cached_instance_method(&"
        << objectToStringMethodCacheName << ");\n";
    out << "    if (to_string == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, "
           "(jobject)value, to_string);\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_is_string(mj2x_ref value) {\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_cached_class(&" << stringClassCacheName << ");\n";
    out << "    if (cls == NULL) return 0;\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : "
           "0;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_equals(mj2x_ref lhs, mj2x_ref rhs) {\n";
    out << "    if (lhs == NULL) { _mj2x_regc_throw_null(\"String.equals\"); return 0; }\n";
    out << "    if (lhs == rhs) return 1;\n";
    out << "    if (rhs == NULL || !_mj2x_intrinsic_is_string(rhs)) return 0;\n";
    out << "    jsize lhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)lhs);\n";
    out << "    jsize rhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)rhs);\n";
    out << "    if (lhs_len != rhs_len) return 0;\n";
    out << "    if (lhs_len == 0) return 1;\n";
    out << "    const jchar* lhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)lhs, NULL);\n";
    out << "    if (lhs_chars == NULL) return 0;\n";
    out << "    const jchar* rhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)rhs, NULL);\n";
    out << "    if (rhs_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)lhs, lhs_chars); return 0; }\n";
    out << "    int32_t same = 1;\n";
    out << "    for (jsize i = 0; i < lhs_len; ++i) { if (lhs_chars[i] != rhs_chars[i]) { same = "
           "0; break; } }\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)lhs, lhs_chars);\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)rhs, rhs_chars);\n";
    out << "    return same;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_length(mj2x_ref value) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.length\"); return 0; }\n";
    out << "    return (int32_t)(*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n";
    out << "}\n\n";
    out << "static uint16_t _mj2x_intrinsic_string_char_at(mj2x_ref value, int32_t index) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.charAt\"); return 0; }\n";
    out << "    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);\n";
    out << "    if (index < 0 || index >= len) {\n";
    out << "        jclass ex = _mj2x_regc_cached_class(&" << stringIndexOutOfBoundsClassCacheName
        << ");\n";
    out << "        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, "
           "\"String.charAt\");\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n";
    out << "    if (chars == NULL) return 0;\n";
    out << "    uint16_t ch = (uint16_t)chars[index];\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);\n";
    out << "    return ch;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_hash_code(mj2x_ref value) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.hashCode\"); return 0; }\n";
    out << "    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);\n";
    out << "    if (len == 0) return 0;\n";
    out << "    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n";
    out << "    if (chars == NULL) return 0;\n";
    out << "    uint32_t hash = 0;\n";
    out << "    for (jsize i = 0; i < len; ++i) hash = hash * 31u + (uint32_t)chars[i];\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);\n";
    out << "    return (int32_t)hash;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_is_empty(mj2x_ref value) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.isEmpty\"); return 0; }\n";
    out << "    return (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value) == 0;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_starts_with(mj2x_ref value, mj2x_ref prefix) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.startsWith\"); return 0; }\n";
    out << "    if (prefix == NULL) { _mj2x_regc_throw_null(\"String.startsWith prefix\"); return "
           "0; }\n";
    out << "    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n";
    out << "    jsize prefix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)prefix);\n";
    out << "    if (prefix_len > value_len) return 0;\n";
    out << "    if (prefix_len == 0) return 1;\n";
    out << "    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n";
    out << "    if (value_chars == NULL) return 0;\n";
    out << "    const jchar* prefix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)prefix, NULL);\n";
    out << "    if (prefix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return 0; }\n";
    out << "    int32_t same = 1;\n";
    out << "    for (jsize i = 0; i < prefix_len; ++i) { if (value_chars[i] != prefix_chars[i]) { "
           "same = 0; break; } }\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)prefix, "
           "prefix_chars);\n";
    out << "    return same;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_ends_with(mj2x_ref value, mj2x_ref suffix) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.endsWith\"); return 0; }\n";
    out << "    if (suffix == NULL) { _mj2x_regc_throw_null(\"String.endsWith suffix\"); return 0; "
           "}\n";
    out << "    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n";
    out << "    jsize suffix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)suffix);\n";
    out << "    if (suffix_len > value_len) return 0;\n";
    out << "    if (suffix_len == 0) return 1;\n";
    out << "    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n";
    out << "    if (value_chars == NULL) return 0;\n";
    out << "    const jchar* suffix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)suffix, NULL);\n";
    out << "    if (suffix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return 0; }\n";
    out << "    int32_t same = 1;\n";
    out << "    jsize start = value_len - suffix_len;\n";
    out << "    for (jsize i = 0; i < suffix_len; ++i) { if (value_chars[start + i] != "
           "suffix_chars[i]) { same = 0; break; } }\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)suffix, "
           "suffix_chars);\n";
    out << "    return same;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_intrinsic_string_index_of(mj2x_ref value, mj2x_ref needle) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.indexOf\"); return -1; }\n";
    out << "    if (needle == NULL) { _mj2x_regc_throw_null(\"String.indexOf needle\"); return -1; "
           "}\n";
    out << "    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)value);\n";
    out << "    jsize needle_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, "
           "(jstring)needle);\n";
    out << "    if (needle_len == 0) return 0;\n";
    out << "    if (needle_len > value_len) return -1;\n";
    out << "    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)value, NULL);\n";
    out << "    if (value_chars == NULL) return -1;\n";
    out << "    const jchar* needle_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, "
           "(jstring)needle, NULL);\n";
    out << "    if (needle_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, "
           "(jstring)value, value_chars); return -1; }\n";
    out << "    int32_t found = -1;\n";
    out << "    for (jsize i = 0; i <= value_len - needle_len; ++i) {\n";
    out << "        jsize j = 0;\n";
    out << "        while (j < needle_len && value_chars[i + j] == needle_chars[j]) j++;\n";
    out << "        if (j == needle_len) { found = (int32_t)i; break; }\n";
    out << "    }\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, "
           "value_chars);\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)needle, "
           "needle_chars);\n";
    out << "    return found;\n";
    out << "}\n\n";
    out << "static jmethodID _mj2x_intrinsic_string_contains_method = NULL;\n";
    out << "static int32_t _mj2x_intrinsic_string_contains(mj2x_ref value, mj2x_ref seq) {\n";
    out << "    if (value == NULL) { _mj2x_regc_throw_null(\"String.contains\"); return 0; }\n";
    out << "    if (seq == NULL) { _mj2x_regc_throw_null(\"String.contains sequence\"); return 0; "
           "}\n";
    out << "    if (_mj2x_intrinsic_is_string(seq)) {\n";
    out << "        return _mj2x_intrinsic_string_index_of(value, seq) >= 0;\n";
    out << "    }\n";
    out << "    jclass cls = _mj2x_regc_cached_class(&" << stringClassCacheName << ");\n";
    out << "    if (cls == NULL) return 0;\n";
    out << "    if (_mj2x_intrinsic_string_contains_method == NULL) {\n";
    out << "        _mj2x_intrinsic_string_contains_method = "
           "(*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, cls, \"contains\", "
           "\"(Ljava/lang/CharSequence;)Z\");\n";
    out << "        if (_mj2x_intrinsic_string_contains_method == NULL) return 0;\n";
    out << "    }\n";
    out << "    return (*_mj2x_regc_env)->CallBooleanMethod(_mj2x_regc_env, (jobject)value, "
           "_mj2x_intrinsic_string_contains_method, (jobject)seq) == JNI_TRUE;\n";
    out << "}\n\n";
}
}   // namespace mj2x

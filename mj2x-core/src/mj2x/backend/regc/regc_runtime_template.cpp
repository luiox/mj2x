/// @file regc_runtime_template.cpp
/// @brief RegC 运行时 C-source 定义发射(从 regc_backend.cpp 抽出)。
///
/// @details 见 regc_runtime_template.hpp。emitRegCFieldAccessHelpers /
/// emitRegCMethodCallHelpers 保持 raw-string 发射;emitRuntimeHelpers 主体已
/// 迁移为 resources/codegen/regc_runtime_helpers.c.in 文本模板(#894),
/// 此处仅做占位符渲染。
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_render.hpp"
#include "mj2x/codegen/c_source.hpp"

#include <string>
#include <unordered_map>

#include "libca/log/log_macros.hpp"
#include "libresources/resources.hpp"

// 模板经 resources.embed 嵌入 mj2x-core(挂载 codegen),include 即自注册。
#include "resources_mj2x_core.generated.hpp"

namespace mj2x {

// emitRegCFieldAccessHelpers / emitRegCMethodCallHelpers 用 CSourceStream 包装
// raw-string 发射;对齐 regc_backend.cpp 的 using 约定,避免逐处加 codegen:: 前缀。
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
    // 模板主体见 resources/codegen/regc_runtime_helpers.c.in(#894)。
    const auto* bundle = resources::bundle("codegen");
    if (bundle == nullptr) {
        CA_LOGT_ERROR("codegen", "codegen template mount is unavailable\n");
        return;
    }
    const auto tpl = bundle->get("/regc_runtime_helpers.c.in");
    if (tpl.is_err()) {
        CA_LOGT_ERROR("codegen", "codegen template is missing: regc_runtime_helpers.c.in\n");
        return;
    }

    std::string fieldHelpers;
    {
        ca::str::Utf8StringBuilder buf;
        emitRegCFieldAccessHelpers(buf);
        fieldHelpers = buf.build().to_std_string();
    }
    std::string methodHelpers;
    {
        ca::str::Utf8StringBuilder buf;
        emitRegCMethodCallHelpers(buf);
        methodHelpers = buf.build().to_std_string();
    }

    std::unordered_map<std::string, std::string> values;
    values["FORWARD_DECLARATIONS"] =
        (fwd != nullptr && !fwd->empty()) ? fwd->render() + "\n" : "";
    values["CACHE_DECLARATIONS"]                     = cacheDeclarations;
    values["FIELD_ACCESS_HELPERS"]                   = fieldHelpers;
    values["METHOD_CALL_HELPERS"]                    = methodHelpers;
    values["STRING_CLASS_CACHE"]                     = stringClassCacheName;
    values["OBJECT_TO_STRING_METHOD_CACHE"]          = objectToStringMethodCacheName;
    values["STRING_VALUE_OF_FLOAT_METHOD_CACHE"]     = stringValueOfFloatMethodCacheName;
    values["STRING_VALUE_OF_DOUBLE_METHOD_CACHE"]    = stringValueOfDoubleMethodCacheName;
    values["FLOAT_PARSE_FLOAT_METHOD_CACHE"]         = floatParseFloatMethodCacheName;
    values["DOUBLE_PARSE_DOUBLE_METHOD_CACHE"]       = doubleParseDoubleMethodCacheName;
    values["NULL_POINTER_EXCEPTION_CLASS_CACHE"]     = nullPointerExceptionClassCacheName;
    values["CLASS_CAST_EXCEPTION_CLASS_CACHE"]       = classCastExceptionClassCacheName;
    values["OUT_OF_MEMORY_ERROR_CLASS_CACHE"]        = outOfMemoryErrorClassCacheName;
    values["STRING_INDEX_OUT_OF_BOUNDS_CLASS_CACHE"] = stringIndexOutOfBoundsClassCacheName;
    values["UNSUPPORTED_OPERATION_CLASS_CACHE"]      = unsupportedOperationExceptionClassCacheName;
    // traceCallLog 条件块(#894):trace 变体注入 dbg 深度追踪 + 带日志的
    // call_cached_instance_void;否则发射无日志变体。文本与原实现逐字节一致。
    values["TRACE_VOID_BLOCK"] =
        traceCallLog
            ? R"mj2x(static _Thread_local int _mj2x_regc_dbg_depth = 0;
static void _mj2x_regc_dbg_enter(const char* owner, const char* name, const char* desc) {
    _mj2x_regc_dbg_depth++;
    if (_mj2x_regc_dbg_depth > 64) {
        fprintf(stderr, "DEEP[%d] %s.%s%s\n", _mj2x_regc_dbg_depth, owner, name, desc);
    }
}
static void _mj2x_regc_dbg_leave() { _mj2x_regc_dbg_depth--; }
static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return; fprintf(stderr, "[CALL:%p] %s.%s%s\n", (void*)_mj2x_regc_env, cache->owner, cache->name, cache->desc); _mj2x_regc_dbg_enter(cache->owner, cache->name, cache->desc); (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); _mj2x_regc_dbg_leave(); }
)mj2x"
            : R"mj2x(static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return; (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); }
)mj2x";

    std::string rendered;
    std::string err;
    if (!codegen::renderCTemplate(
            std::string_view{reinterpret_cast<const char*>(tpl.unwrap().data()),
                             tpl.unwrap().size()},
            values, {}, rendered, err)) {
        CA_LOGT_ERROR("codegen", "cannot render regc runtime helpers: {}\n", err.c_str());
        return;
    }
    out.append(rendered.data(), rendered.size());
}

}   // namespace mj2x

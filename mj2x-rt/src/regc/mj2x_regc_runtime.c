/// @file mj2x_regc_runtime.c
/// @brief RegC 运行时 helper 实现(#894 B 类真下沉)。
///
/// @details 原 emitRuntimeHelpers 模板主体的物理落地:编译进 mj2x-rt,
/// 生成工程经链接消费,不再逐 TU 内联发射。TLS 状态(env/ctx/owner_class)
/// 定义于此,生成代码经 mj2x_regc_runtime.h 的 extern 声明直接访问;
/// 角色单例缓存收敛为运行时固定实例。
#include "regc/mj2x_regc_runtime.h"

/* 进程级缓存并发契约（2026-08 定案）：
 * - jclass 全局槽（class_cache/ownerClass）：CAS 发布，输家删除自己新建的
 *   global ref 并改用赢家值（消除 check-then-write 双建泄漏与撕裂读）。
 * - jmethodID/jfieldID 槽：解析结果幂等（同站点必然解析出同一 id），指针宽
 *   写入在实际平台上原子；保留 check-then-write，不做同步。
 * - lastReceiverClass：只增不删——CAS 换入新类后旧 global ref 不 Delete。
 *   旧方案 Delete→New 窗口下其他线程可能对已释放句柄调 IsSameObject
 *   （use-after-free）；泄漏上界 = 每调用点出现过的 distinct receiver 类数，
 *   可接受。实现对齐 StackC 的 _mj2x_cache_cas（JNIC 3.7.0 同构）。 */
#if defined(_MSC_VER)
#include <intrin.h>
static __inline void* _mj2x_regc_ptr_cas(void* volatile* slot, void* expected, void* desired)
{
    return _InterlockedCompareExchangePointer(slot, desired, expected);
}
#else
static __inline void* _mj2x_regc_ptr_cas(void* volatile* slot, void* expected, void* desired)
{
    void* previous = expected;
    __atomic_compare_exchange_n(slot, &previous, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return previous;
}
#endif

jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache);

// Per-thread env/ctx: concurrent native calls from multiple threads must not
// share JNIEnv state. C++ uses thread_local; MSVC C uses __declspec(thread);
// other C11 uses _Thread_local. Keep comments ASCII for MSVC codepage safety.
#if defined(__cplusplus)
thread_local JNIEnv* _mj2x_regc_env = NULL;
thread_local mj2x_regc_context _mj2x_regc_ctx;
thread_local jclass _mj2x_regc_owner_class = NULL;
#elif defined(_MSC_VER)
__declspec(thread) JNIEnv* _mj2x_regc_env = NULL;
__declspec(thread) mj2x_regc_context _mj2x_regc_ctx;
__declspec(thread) jclass _mj2x_regc_owner_class = NULL;
#else
_Thread_local JNIEnv* _mj2x_regc_env = NULL;
_Thread_local mj2x_regc_context _mj2x_regc_ctx;
_Thread_local jclass _mj2x_regc_owner_class = NULL;
#endif

/* 角色单例缓存(#894 B 类下沉):原为每程序 TU 内注入的命名缓存,
 * 现收敛为运行时固定实例——语义角色唯一,跨生成 TU 共享命中。 */
mj2x_regc_class_cache _mj2x_rt_string_class_cache = {"java/lang/String", NULL};
mj2x_regc_instance_method_cache _mj2x_rt_object_to_string_method_cache = {
    "java/lang/Object", "toString", "()Ljava/lang/String;", NULL, NULL};
mj2x_regc_static_method_cache _mj2x_rt_string_value_of_float_cache = {
    "java/lang/String", "valueOf", "(F)Ljava/lang/String;", NULL, NULL};
mj2x_regc_static_method_cache _mj2x_rt_string_value_of_double_cache = {
    "java/lang/String", "valueOf", "(D)Ljava/lang/String;", NULL, NULL};
mj2x_regc_static_method_cache _mj2x_rt_float_parse_float_cache = {
    "java/lang/Float", "parseFloat", "(Ljava/lang/String;)F", NULL, NULL};
mj2x_regc_static_method_cache _mj2x_rt_double_parse_double_cache = {
    "java/lang/Double", "parseDouble", "(Ljava/lang/String;)D", NULL, NULL};
mj2x_regc_class_cache _mj2x_rt_npe_class_cache = {"java/lang/NullPointerException", NULL};
mj2x_regc_class_cache _mj2x_rt_cce_class_cache = {"java/lang/ClassCastException", NULL};
mj2x_regc_class_cache _mj2x_rt_oom_class_cache = {"java/lang/OutOfMemoryError", NULL};
mj2x_regc_class_cache _mj2x_rt_uoe_class_cache = {"java/lang/UnsupportedOperationException", NULL};
mj2x_regc_context* _mj2x_regc_current_context(void) { return &_mj2x_regc_ctx; }

float _mj2x_regc_f32_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

double _mj2x_regc_f64_from_bits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void _mj2x_regc_throw_runtime(void* env, const char* class_name, const char* message) {
    JNIEnv* jni = (JNIEnv*)env;
    if (jni == NULL || class_name == NULL) return;
    jclass ex = (*jni)->FindClass(jni, class_name);
    if (ex != NULL) (*jni)->ThrowNew(jni, ex, message != NULL ? message : "");
}

void _mj2x_regc_throw_unsupported(const char* feature) {
    jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_uoe_class_cache);
    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, feature);
}

void _mj2x_regc_throw_null(const char* message) {
    jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_npe_class_cache);
    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, message);
}

jclass _mj2x_regc_find_class(const char* desc) {
    if (desc == NULL) return NULL;
    /* 'L' 前缀仅在完整字段描述符形态（L...;）时剥壳转内部名：裸内部名可能以 'L'
     * 开头（default 包合成类 Lambda$N，#1182 评审收口 P1），按首字符无条件剥壳
     * 会把名字截成 "ambda$N" → NoClassDefFoundError。 */
    const size_t desc_len = strlen(desc);
    if (desc[0] == 'L' && desc_len >= 3 && desc[desc_len - 1] == ';') {
        char name[1024];
        int i = 0;
        while (desc[i + 1] != ';' && desc[i + 1] != '\0' && i < 1023) { name[i] = desc[i + 1]; i++; }
        name[i] = '\0';
        return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, name);
    }
    return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, desc);
}

/// 前置声明：按 internal name 缓存查找 jclass（定义见 class_literal 区）。
static jclass _mj2x_regc_cached_class_by_name(const char* internal_name);

jclass _mj2x_regc_global_class(const char* desc, jclass* slot) {
    if (slot == NULL) return NULL;
    if (*slot != NULL) return *slot;
    jclass local = _mj2x_regc_find_class(desc);
    if (local == NULL) return NULL;
    jclass global = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, local);
    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, local);
    if (global == NULL) return NULL;
    /* CAS 发布：并发赢家先落槽，输家删自己的新 ref 改用赢家值（无双建泄漏）。 */
    jclass winner = (jclass)_mj2x_regc_ptr_cas((void* volatile*)slot, NULL, global);
    if (winner != NULL) {
        (*_mj2x_regc_env)->DeleteGlobalRef(_mj2x_regc_env, global);
        return winner;
    }
    return global;
}

jclass _mj2x_regc_cached_class(mj2x_regc_class_cache* cache) {
    if (cache == NULL) return NULL;
    return _mj2x_regc_global_class(cache->desc, &cache->value);
}

jmethodID _mj2x_regc_cached_static_method(mj2x_regc_static_method_cache* cache) {
    if (cache == NULL) return NULL;
    if (cache->method != NULL) return cache->method;
    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jmethodID method = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, owner, cache->name, cache->desc);
    if (method == NULL) return NULL;
    cache->method = method;
    return method;
}

// Resolve the jmethodID for an instance method. Correctness: the id MUST be
// resolved against the receiver's actual class, not the declared owner. When the
// owner is abstract/interface (e.g. java/io/InputStream.read), GetMethodID on the
// owner yields the abstract entry and CallXMethod triggers AbstractMethodError or
// crashes on the JIT inlined path (observed ACCESS_VIOLATION on HotSpot 17).
// Caching: reuse the cached id only when the receiver's runtime class equals the
// declared owner (covers the common same-type repeated calls); write back only in
// that case. Interface/abstract dispatch additionally keeps one per-receiver-class
// slot (lastReceiverClass + lastReceiverMethod, global-ref pinned): repeated calls on
// the same concrete class reuse the resolved id after an IsSameObject check, while a
// different subclass re-resolves instead of reusing a misleading id.
jmethodID _mj2x_regc_cached_instance_method(mj2x_regc_instance_method_cache* cache, jobject receiver) {
    if (cache == NULL || receiver == NULL) return NULL;
    jclass receiver_class = (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, receiver);
    if (receiver_class == NULL) return NULL;
    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);
    /* Fast path: receiver class == declared owner, reuse cached id. */
    if (owner != NULL && (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, owner, receiver_class) && cache->method != NULL) {
        (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);
        return cache->method;
    }
    /* Subclass/interface path: reuse the per-receiver-class slot when the class matches. */
    if (cache->lastReceiverClass != NULL &&
        (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, cache->lastReceiverClass, receiver_class)) {
        (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);
        return cache->lastReceiverMethod;
    }
    jmethodID method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, receiver_class, cache->name, cache->desc);
    if (method == NULL) { (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class); return NULL; }
    /* Write back only when receiver class == declared owner; never for subclasses. */
    if (owner != NULL && (*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, owner, receiver_class)) {
        cache->method = method;
    }
    else {
        /* Pin the receiver class and its resolved id for the next same-class call.
         * 只增不删：CAS 把旧类换出后不 Delete 旧 global ref——旧方案
         * Delete→NULL→New 窗口下其他线程可能正对旧句柄做 IsSameObject
         * （use-after-free，含双重 Delete）。泄漏上界 = 本调用点出现过的
         * distinct receiver 类数（仅接口/抽象分派的子类交替场景触发）。
         * method 仅在换占成功后写入；class→method 两写之间读者可能短暂
         * 见到「新类 + 旧类 method」的错配（与旧代码同窗，受 IsSameObject
         * 精确类匹配约束，仅同名 desc 解析到不同覆写时才可观察）。 */
        jclass observed = cache->lastReceiverClass;
        jclass pinned   = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, receiver_class);
        if (pinned != NULL) {
            jclass previous =
                (jclass)_mj2x_regc_ptr_cas((void* volatile*)&cache->lastReceiverClass,
                                           observed, pinned);
            if (previous == observed) {
                cache->lastReceiverMethod = method;
            }
            else {
                /* 槽被并发线程换占：撤自己的新 ref，本次调用仍用现场解析值。 */
                (*_mj2x_regc_env)->DeleteGlobalRef(_mj2x_regc_env, pinned);
            }
        }
    }
    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, receiver_class);
    return method;
}

/* 按声明 owner 解析的实例方法缓存（libraryOwner 调用点专用）：GetMethodID 只
 * 解析一次并缓存，之后每次调用 0 JNI（相比 _mj2x_regc_cached_instance_method 的
 * 每调用 GetObjectClass+IsSameObject）。CallXxxMethodA 对任意 receiver 做 JNI 虚
 * 分派（按实际对象类找覆写），methodID 来自声明类即可，语义正确；对接口 owner
 * 同样适用（HotSpot 支持接口 methodID）。解析失败清异常防 pending 挂起。 */
jmethodID _mj2x_regc_cached_owner_method(mj2x_regc_instance_method_cache* cache, jobject receiver) {
    if (cache == NULL || receiver == NULL) return NULL;
    if (cache->method != NULL) return cache->method;
    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    cache->method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, owner, cache->name, cache->desc);
    if (cache->method == NULL) {
        (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env);
    }
    return cache->method;
}

jfieldID _mj2x_regc_cached_static_field(mj2x_regc_static_field_cache* cache) {
    if (cache == NULL) return NULL;
    if (cache->field != NULL) return cache->field;
    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jfieldID field = (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, owner, cache->name, cache->desc);
    if (field == NULL) return NULL;
    cache->field = field;
    return field;
}

jfieldID _mj2x_regc_cached_instance_field(mj2x_regc_instance_field_cache* cache) {
    if (cache == NULL) return NULL;
    if (cache->field != NULL) return cache->field;
    jclass owner = _mj2x_regc_global_class(cache->owner, &cache->ownerClass);
    if (owner == NULL) return NULL;
    jfieldID field = (*_mj2x_regc_env)->GetFieldID(_mj2x_regc_env, owner, cache->name, cache->desc);
    if (field == NULL) return NULL;
    cache->field = field;
    return field;
}

mj2x_ref _mj2x_regc_alloc_object(const char* type_desc) {
    jclass cls = _mj2x_regc_find_class(type_desc);
    if (cls == NULL) return NULL;
    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);
}

mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache) {
    jclass cls = _mj2x_regc_cached_class(cache);
    if (cls == NULL) return NULL;
    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);
}

mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length) {
    if (type_desc == NULL || type_desc[0] != '[') return NULL;
    switch (type_desc[1]) {
    case 'Z': return (mj2x_ref)(*_mj2x_regc_env)->NewBooleanArray(_mj2x_regc_env, length);
    case 'B': return (mj2x_ref)(*_mj2x_regc_env)->NewByteArray(_mj2x_regc_env, length);
    case 'C': return (mj2x_ref)(*_mj2x_regc_env)->NewCharArray(_mj2x_regc_env, length);
    case 'S': return (mj2x_ref)(*_mj2x_regc_env)->NewShortArray(_mj2x_regc_env, length);
    case 'I': return (mj2x_ref)(*_mj2x_regc_env)->NewIntArray(_mj2x_regc_env, length);
    case 'J': return (mj2x_ref)(*_mj2x_regc_env)->NewLongArray(_mj2x_regc_env, length);
    case 'F': return (mj2x_ref)(*_mj2x_regc_env)->NewFloatArray(_mj2x_regc_env, length);
    case 'D': return (mj2x_ref)(*_mj2x_regc_env)->NewDoubleArray(_mj2x_regc_env, length);
    default: return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, length, _mj2x_regc_find_class(type_desc + 1), NULL);
    }
}

mj2x_ref _mj2x_regc_alloc_ref_array_cached(mj2x_regc_class_cache* component_cache, int32_t length) {
    jclass component = _mj2x_regc_cached_class(component_cache);
    if (component == NULL) return NULL;
    return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, length, component, NULL);
}

mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, int32_t* dims) {
    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') {
        _mj2x_regc_throw_unsupported("RegC multi-array helper received a malformed descriptor");
        return NULL;
    }
    if (dim_count == 1) return _mj2x_regc_alloc_array(type_desc, dims[0]);
    jclass component = _mj2x_regc_find_class(type_desc + 1);
    if (component == NULL) return NULL;
    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], component, NULL);
    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
    /* Java multi-array allocation eagerly allocates every requested dimension.
       Dimensions beyond dim_count stay NULL, matching MULTIANEWARRAY partial forms. */
    for (int32_t i = 0; i < dims[0]; ++i) {
        mj2x_ref child = _mj2x_regc_alloc_multi_array(type_desc + 1, dim_count - 1, dims + 1);
        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, (jobject)child);
        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
    }
    return (mj2x_ref)array;
}

mj2x_ref _mj2x_regc_alloc_multi_array_cached(const char* type_desc, int32_t dim_count, int32_t* dims, mj2x_regc_class_cache** component_caches) {
    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') {
        _mj2x_regc_throw_unsupported("RegC cached multi-array helper received a malformed descriptor");
        return NULL;
    }
    if (dim_count == 1) {
        if (type_desc[1] == 'L' || type_desc[1] == '[') {
            return _mj2x_regc_alloc_ref_array_cached(component_caches[0], dims[0]);
        }
        return _mj2x_regc_alloc_array(type_desc, dims[0]);
    }
    if (component_caches == NULL || component_caches[0] == NULL) return NULL;
    jclass component = _mj2x_regc_cached_class(component_caches[0]);
    if (component == NULL) return NULL;
    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], component, NULL);
    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
    for (int32_t i = 0; i < dims[0]; ++i) {
        mj2x_ref child = _mj2x_regc_alloc_multi_array_cached(type_desc + 1, dim_count - 1, dims + 1, component_caches + 1);
        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, (jobject)child);
        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;
    }
    return (mj2x_ref)array;
}

int32_t _mj2x_regc_array_length(mj2x_ref array_ref) {
    if (array_ref == NULL) { _mj2x_regc_throw_null("array.length"); return 0; }
    return (int32_t)(*_mj2x_regc_env)->GetArrayLength(_mj2x_regc_env, (jarray)array_ref);
}

mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc) {
    if (value == NULL) return NULL;
    jclass cls = _mj2x_regc_find_class(type_desc);
    if (cls == NULL) return NULL;
    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return value;
    jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_cce_class_cache);
    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, type_desc);
    return NULL;
}

mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, mj2x_regc_class_cache* cache) {
    if (value == NULL) return NULL;
    jclass cls = _mj2x_regc_cached_class(cache);
    if (cls == NULL) return NULL;
    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return value;
    jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_cce_class_cache);
    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, cache != NULL ? cache->desc : "checkcast");
    return NULL;
}

int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc) {
    if (value == NULL) return 0;
    jclass cls = _mj2x_regc_find_class(type_desc);
    if (cls == NULL) return 0;
    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;
}

int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, mj2x_regc_class_cache* cache) {
    if (value == NULL) return 0;
    jclass cls = _mj2x_regc_cached_class(cache);
    if (cls == NULL) return 0;
    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;
}

void _mj2x_regc_monitor_enter(mj2x_ref value) { (*_mj2x_regc_env)->MonitorEnter(_mj2x_regc_env, (jobject)value); }
void _mj2x_regc_monitor_exit(mj2x_ref value) { (*_mj2x_regc_env)->MonitorExit(_mj2x_regc_env, (jobject)value); }
int32_t _mj2x_regc_exception_check(void) { return (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env) ? 1 : 0; }
mj2x_ref _mj2x_regc_exception_occurred(void) { return (mj2x_ref)(*_mj2x_regc_env)->ExceptionOccurred(_mj2x_regc_env); }
void _mj2x_regc_exception_clear(void) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); }
int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type) {
    if (catch_type == NULL || catch_type[0] == '\0') return 1;
    if (value == NULL) return 0;
    /* catch 类型按 internal name 走缓存（命中 0 JNI），FindClass 失败时保持
     * pending exception 并清空后返回不匹配，与原语义一致。 */
    jclass cls = _mj2x_regc_cached_class_by_name(catch_type);
    if (cls == NULL) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); return 0; }
    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;
}
void _mj2x_regc_throw(mj2x_ref value) {
    if (value == NULL) {
        jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_npe_class_cache);
        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, "athrow null");
        return;
    }
    (*_mj2x_regc_env)->Throw(_mj2x_regc_env, (jthrowable)value);
}

jclass _mj2x_regc_field_class(int32_t is_static, mj2x_ref receiver, const char* owner) {
    if (is_static) return _mj2x_regc_find_class(owner);
    if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return NULL; }
    return (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, (jobject)receiver);
}
jfieldID _mj2x_regc_field_id(int32_t is_static, jclass c, const char* name, const char* desc) {
    if (c == NULL) return NULL;
    return is_static ? (*_mj2x_regc_env)->GetStaticFieldID(_mj2x_regc_env, c, name, desc) : (*_mj2x_regc_env)->GetFieldID(_mj2x_regc_env, c, name, desc);
}
bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return false; return is_static ? (*_mj2x_regc_env)->GetStaticBooleanField(_mj2x_regc_env, c, f) == JNI_TRUE : (*_mj2x_regc_env)->GetBooleanField(_mj2x_regc_env, (jobject)receiver, f) == JNI_TRUE; }
int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (int8_t)(is_static ? (*_mj2x_regc_env)->GetStaticByteField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetByteField(_mj2x_regc_env, (jobject)receiver, f)); }
uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (uint16_t)(is_static ? (*_mj2x_regc_env)->GetStaticCharField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetCharField(_mj2x_regc_env, (jobject)receiver, f)); }
int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return (int16_t)(is_static ? (*_mj2x_regc_env)->GetStaticShortField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetShortField(_mj2x_regc_env, (jobject)receiver, f)); }
int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return is_static ? (*_mj2x_regc_env)->GetStaticIntField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetIntField(_mj2x_regc_env, (jobject)receiver, f); }
int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0; return is_static ? (*_mj2x_regc_env)->GetStaticLongField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetLongField(_mj2x_regc_env, (jobject)receiver, f); }
float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0.0f; return is_static ? (*_mj2x_regc_env)->GetStaticFloatField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetFloatField(_mj2x_regc_env, (jobject)receiver, f); }
double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return 0.0; return is_static ? (*_mj2x_regc_env)->GetStaticDoubleField(_mj2x_regc_env, c, f) : (*_mj2x_regc_env)->GetDoubleField(_mj2x_regc_env, (jobject)receiver, f); }
mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return NULL; return is_static ? (mj2x_ref)(*_mj2x_regc_env)->GetStaticObjectField(_mj2x_regc_env, c, f) : (mj2x_ref)(*_mj2x_regc_env)->GetObjectField(_mj2x_regc_env, (jobject)receiver, f); }
bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return false; return (*_mj2x_regc_env)->GetStaticBooleanField(_mj2x_regc_env, cache->ownerClass, f) == JNI_TRUE; }
int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (int8_t)(*_mj2x_regc_env)->GetStaticByteField(_mj2x_regc_env, cache->ownerClass, f); }
uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (uint16_t)(*_mj2x_regc_env)->GetStaticCharField(_mj2x_regc_env, cache->ownerClass, f); }
int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (int16_t)(*_mj2x_regc_env)->GetStaticShortField(_mj2x_regc_env, cache->ownerClass, f); }
int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->GetStaticIntField(_mj2x_regc_env, cache->ownerClass, f); }
int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->GetStaticLongField(_mj2x_regc_env, cache->ownerClass, f); }
float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0.0f; return (*_mj2x_regc_env)->GetStaticFloatField(_mj2x_regc_env, cache->ownerClass, f); }
double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return 0.0; return (*_mj2x_regc_env)->GetStaticDoubleField(_mj2x_regc_env, cache->ownerClass, f); }
mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* cache) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->GetStaticObjectField(_mj2x_regc_env, cache->ownerClass, f); }



bool _mj2x_regc_load_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return false; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return false; return (*_mj2x_regc_env)->GetBooleanField(_mj2x_regc_env, (jobject)receiver, f) == JNI_TRUE; }
int8_t _mj2x_regc_load_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (int8_t)(*_mj2x_regc_env)->GetByteField(_mj2x_regc_env, (jobject)receiver, f); }
uint16_t _mj2x_regc_load_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (uint16_t)(*_mj2x_regc_env)->GetCharField(_mj2x_regc_env, (jobject)receiver, f); }
int16_t _mj2x_regc_load_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (int16_t)(*_mj2x_regc_env)->GetShortField(_mj2x_regc_env, (jobject)receiver, f); }
int32_t _mj2x_regc_load_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (*_mj2x_regc_env)->GetIntField(_mj2x_regc_env, (jobject)receiver, f); }
int64_t _mj2x_regc_load_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0; return (*_mj2x_regc_env)->GetLongField(_mj2x_regc_env, (jobject)receiver, f); }
float _mj2x_regc_load_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0.0f; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0.0f; return (*_mj2x_regc_env)->GetFloatField(_mj2x_regc_env, (jobject)receiver, f); }
double _mj2x_regc_load_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return 0.0; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return 0.0; return (*_mj2x_regc_env)->GetDoubleField(_mj2x_regc_env, (jobject)receiver, f); }
mj2x_ref _mj2x_regc_load_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return NULL; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->GetObjectField(_mj2x_regc_env, (jobject)receiver, f); }



void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, bool value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticBooleanField(_mj2x_regc_env, c, f, value ? JNI_TRUE : JNI_FALSE); else (*_mj2x_regc_env)->SetBooleanField(_mj2x_regc_env, (jobject)receiver, f, value ? JNI_TRUE : JNI_FALSE); }
void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int8_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticByteField(_mj2x_regc_env, c, f, (jbyte)value); else (*_mj2x_regc_env)->SetByteField(_mj2x_regc_env, (jobject)receiver, f, (jbyte)value); }
void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, uint16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticCharField(_mj2x_regc_env, c, f, (jchar)value); else (*_mj2x_regc_env)->SetCharField(_mj2x_regc_env, (jobject)receiver, f, (jchar)value); }
void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticShortField(_mj2x_regc_env, c, f, (jshort)value); else (*_mj2x_regc_env)->SetShortField(_mj2x_regc_env, (jobject)receiver, f, (jshort)value); }
void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int32_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticIntField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetIntField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int64_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticLongField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetLongField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, float value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticFloatField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetFloatField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, double value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticDoubleField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetDoubleField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, mj2x_ref value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticObjectField(_mj2x_regc_env, c, f, (jobject)value); else (*_mj2x_regc_env)->SetObjectField(_mj2x_regc_env, (jobject)receiver, f, (jobject)value); }
void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, bool value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticBooleanField(_mj2x_regc_env, cache->ownerClass, f, value ? JNI_TRUE : JNI_FALSE); }
void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, int8_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticByteField(_mj2x_regc_env, cache->ownerClass, f, (jbyte)value); }
void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, uint16_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticCharField(_mj2x_regc_env, cache->ownerClass, f, (jchar)value); }
void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, int16_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticShortField(_mj2x_regc_env, cache->ownerClass, f, (jshort)value); }
void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, int32_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticIntField(_mj2x_regc_env, cache->ownerClass, f, value); }
void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, int64_t value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticLongField(_mj2x_regc_env, cache->ownerClass, f, value); }
void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, float value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticFloatField(_mj2x_regc_env, cache->ownerClass, f, value); }
void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, double value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticDoubleField(_mj2x_regc_env, cache->ownerClass, f, value); }
void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, mj2x_ref value) { jfieldID f = _mj2x_regc_cached_static_field(cache); if (f == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->SetStaticObjectField(_mj2x_regc_env, cache->ownerClass, f, (jobject)value); }



void _mj2x_regc_store_cached_instance_bool_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetBooleanField(_mj2x_regc_env, (jobject)receiver, f, value ? JNI_TRUE : JNI_FALSE); }
void _mj2x_regc_store_cached_instance_i8_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetByteField(_mj2x_regc_env, (jobject)receiver, f, (jbyte)value); }
void _mj2x_regc_store_cached_instance_char_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetCharField(_mj2x_regc_env, (jobject)receiver, f, (jchar)value); }
void _mj2x_regc_store_cached_instance_i16_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetShortField(_mj2x_regc_env, (jobject)receiver, f, (jshort)value); }
void _mj2x_regc_store_cached_instance_i32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetIntField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_cached_instance_i64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetLongField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_cached_instance_f32_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetFloatField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_cached_instance_f64_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetDoubleField(_mj2x_regc_env, (jobject)receiver, f, value); }
void _mj2x_regc_store_cached_instance_ref_field(mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value) { if (receiver == NULL) { _mj2x_regc_throw_null("instance field access"); return; } jfieldID f = _mj2x_regc_cached_instance_field(cache); if (f == NULL) return; (*_mj2x_regc_env)->SetObjectField(_mj2x_regc_env, (jobject)receiver, f, (jobject)value); }

mj2x_regc_class_cache _mj2x_regc_boolean_array_cache = {"[Z", NULL};
int32_t _mj2x_regc_is_boolean_array(mj2x_ref array_ref) { jclass c = _mj2x_regc_cached_class(&_mj2x_regc_boolean_array_cache); return c != NULL && (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)array_ref, c); }
int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0; } if (_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = JNI_FALSE; (*_mj2x_regc_env)->GetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, index, 1, &v); return v == JNI_TRUE ? 1 : 0; } jbyte v = 0; (*_mj2x_regc_env)->GetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, &v); return (int8_t)v; }
int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0; } jshort v = 0; (*_mj2x_regc_env)->GetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, 1, &v); return (int16_t)v; }
uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0; } jchar v = 0; (*_mj2x_regc_env)->GetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, &v); return (uint16_t)v; }
int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0; } jint v = 0; (*_mj2x_regc_env)->GetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, &v); return v; }
int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0; } jlong v = 0; (*_mj2x_regc_env)->GetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, &v); return v; }
float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0.0f; } jfloat v = 0; (*_mj2x_regc_env)->GetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, index, 1, &v); return v; }
double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return 0.0; } jdouble v = 0; (*_mj2x_regc_env)->GetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, index, 1, &v); return v; }
mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null("array load"); return NULL; } return (mj2x_ref)(*_mj2x_regc_env)->GetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, index); }
void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } if (_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = value != 0 ? JNI_TRUE : JNI_FALSE; (*_mj2x_regc_env)->SetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, index, 1, &v); return; } jbyte v = (jbyte)value; (*_mj2x_regc_env)->SetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jshort v = (jshort)value; (*_mj2x_regc_env)->SetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jchar v = (jchar)value; (*_mj2x_regc_env)->SetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jint v = value; (*_mj2x_regc_env)->SetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jlong v = value; (*_mj2x_regc_env)->SetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jfloat v = value; (*_mj2x_regc_env)->SetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } jdouble v = value; (*_mj2x_regc_env)->SetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, index, 1, &v); }
void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref value) { if (array_ref == NULL) { _mj2x_regc_throw_null("array store"); return; } (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, index, (jobject)value); }

const char* _mj2x_regc_skip_type(const char* p) {
    while (*p == '[') p++;
    if (*p == 'L') { while (*p != ';' && *p != '\0') p++; return *p == ';' ? p + 1 : p; }
    return *p == '\0' ? p : p + 1;
}

int32_t _mj2x_regc_count_params(const char* desc) {
    int32_t count = 0;
    const char* p = desc + 1;
    while (*p != ')' && *p != '\0') { p = _mj2x_regc_skip_type(p); count++; }
    return count;
}

jobject _mj2x_regc_collect_args(const char* desc, int32_t arg_count, va_list ap, jvalue* values) {
    int32_t param_count = _mj2x_regc_count_params(desc);
    if (param_count > 32) { _mj2x_regc_throw_unsupported("method call exceeds 32 parameters (jvalue values[32])"); return NULL; }
    jobject receiver = NULL;
    if (arg_count == param_count + 1) receiver = (jobject)va_arg(ap, mj2x_ref);
    const char* p = desc + 1;
    int32_t i = 0;
    while (*p != ')' && *p != '\0' && i < 32) {
        char tag = *p;
        /* Array params ([X) are references (jobject), NOT the element primitive.
           Previously the '[' was consumed and tag became the element type, so a
           byte[] arg was read as a jbyte via va_arg(ap,int) -- truncating the
           pointer and corrupting the constructor call (e.g. ByteArrayInputStream.<init>).
           Track array-ness and route to the .l slot. */
        int32_t is_array = 0;
        while (tag == '[') { is_array = 1; tag = *++p; }
        if (is_array || tag == 'L') {
            values[i].l = (jobject)va_arg(ap, mj2x_ref);
        } else {
            switch (tag) {
        case 'Z': values[i].z = (jboolean)va_arg(ap, int); break;
        case 'B': values[i].b = (jbyte)va_arg(ap, int); break;
        case 'C': values[i].c = (jchar)va_arg(ap, int); break;
        case 'S': values[i].s = (jshort)va_arg(ap, int); break;
        case 'I': values[i].i = (jint)va_arg(ap, int); break;
        case 'J': values[i].j = (jlong)va_arg(ap, int64_t); break;
        case 'F': values[i].f = (jfloat)va_arg(ap, double); break;
        case 'D': values[i].d = (jdouble)va_arg(ap, double); break;
        default: values[i].l = (jobject)va_arg(ap, mj2x_ref); break;
        }
        }
        p = _mj2x_regc_skip_type(p);
        i++;
    }
    return receiver;
}



jmethodID _mj2x_regc_method_id(jobject receiver, int32_t is_static, const char* owner, const char* name, const char* desc) {
    if (is_static) return (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, _mj2x_regc_find_class(owner), name, desc);
    if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return NULL; }
    return (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, (*_mj2x_regc_env)->GetObjectClass(_mj2x_regc_env, receiver), name, desc);
}
int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0; return is_static ? (*_mj2x_regc_env)->CallStaticIntMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallIntMethodA(_mj2x_regc_env, receiver, m, values); }
int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0; return is_static ? (*_mj2x_regc_env)->CallStaticLongMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallLongMethodA(_mj2x_regc_env, receiver, m, values); }
float _mj2x_regc_call_f32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0.0f; return is_static ? (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallFloatMethodA(_mj2x_regc_env, receiver, m, values); }
double _mj2x_regc_call_f64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return 0.0; return is_static ? (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (*_mj2x_regc_env)->CallDoubleMethodA(_mj2x_regc_env, receiver, m, values); }
mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return NULL; return is_static ? (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values) : (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethodA(_mj2x_regc_env, receiver, m, values); }
void _mj2x_regc_call_void(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_method_id(receiver, is_static, owner, name, desc); if (m == NULL) return; if (is_static) (*_mj2x_regc_env)->CallStaticVoidMethodA(_mj2x_regc_env, _mj2x_regc_find_class(owner), m, values); else (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); }

int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->CallStaticIntMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0; return (*_mj2x_regc_env)->CallStaticLongMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0.0f; return (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return 0.0; return (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); jmethodID m = _mj2x_regc_cached_static_method(cache); if (m == NULL || cache->ownerClass == NULL) return; (*_mj2x_regc_env)->CallStaticVoidMethodA(_mj2x_regc_env, cache->ownerClass, m, values); }
int32_t _mj2x_regc_call_cached_instance_i32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallIntMethodA(_mj2x_regc_env, receiver, m, values); }
int64_t _mj2x_regc_call_cached_instance_i64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0; return (*_mj2x_regc_env)->CallLongMethodA(_mj2x_regc_env, receiver, m, values); }
float _mj2x_regc_call_cached_instance_f32(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0f; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0.0f; return (*_mj2x_regc_env)->CallFloatMethodA(_mj2x_regc_env, receiver, m, values); }
double _mj2x_regc_call_cached_instance_f64(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return 0.0; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return 0.0; return (*_mj2x_regc_env)->CallDoubleMethodA(_mj2x_regc_env, receiver, m, values); }
mj2x_ref _mj2x_regc_call_cached_instance_ref(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return NULL; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return NULL; return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethodA(_mj2x_regc_env, receiver, m, values); }

int _mj2x_rt_trace_call_log = 0;
int _mj2x_rt_trace_depth = 0;
void mj2x_regc_set_trace_call_log(int on) { _mj2x_rt_trace_call_log = on; }
void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) {
    jvalue values[32]; va_list ap; va_start(ap, arg_count);
    jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap);
    if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; }
    jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver);
    if (m == NULL) return;
    if (_mj2x_rt_trace_call_log) {
        fprintf(stderr, "[CALL:%p] %s.%s%s\n", (void*)_mj2x_regc_env, cache->owner,
                cache->name, cache->desc);
        ++_mj2x_rt_trace_depth;
    }
    (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values);
    if (_mj2x_rt_trace_call_log) {
        --_mj2x_rt_trace_depth;
    }
}
mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8) {
    (void)context;
    if (utf8 == NULL) return NULL;
    static struct { const char* key; jstring value; } literal_cache[32];
    static int literal_cache_count = 0;
    for (int i = 0; i < literal_cache_count; ++i) {
        if (literal_cache[i].key == utf8 ||
            strcmp(literal_cache[i].key, utf8) == 0) {
            return literal_cache[i].value;
        }
    }
    jstring s = (jstring)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, utf8);
    if (s != NULL) {
        /* JVM 语义：ldc 常量是 interned 实例（#1117，jvmtp 1.7.1 修复④同款
         * 缺陷面）——首次物化经 String.intern() 归一化进 JVM 字符串池，同内容
         * 常量跨站点/跨类/跨 String.intern() 恒等；缓存 32 槽溢出后逐次物化
         * 也因 intern 恒等不再破坏语义（只多 2 次 JNI）。miss 路径 2 次 JNI，
         * 命中路径保持 0 JNI。intern_mid 首次并发竞态双方算得同一 methodID，
         * 覆写无害。 */
        static jmethodID intern_mid = NULL;
        if (intern_mid == NULL) {
            jclass string_class =
                (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/String");
            if (string_class != NULL) {
                intern_mid = (*_mj2x_regc_env)->GetMethodID(
                    _mj2x_regc_env, string_class, "intern", "()Ljava/lang/String;");
                (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, string_class);
            }
        }
        if (intern_mid != NULL) {
            jstring pooled = (jstring)(*_mj2x_regc_env)->CallObjectMethodA(
                _mj2x_regc_env, s, intern_mid, NULL);
            if (pooled != NULL) {
                (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, s);
                s = pooled;
            }
        }
    }
    if (s != NULL) {
        s = (jstring)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, s);
    }
    if (s != NULL && literal_cache_count < 32) {
        /* 索引按位保护：多线程首次并发的瞬时竞态最多覆盖已有槽，不越界 */
        literal_cache[literal_cache_count & 31].key = utf8;
        literal_cache[literal_cache_count & 31].value = s;
        ++literal_cache_count;
    }
    return (mj2x_ref)s;
}

/// 按 internal name 查找并缓存 jclass（全局引用）。
///
/// 同名类在同一 native library 内 loader 稳定，FindClass 结果对象恒等
/// （IsSameObject 语义与逐次 FindClass 完全一致）。表项永不释放，泄漏上界 =
/// 不同 internal name 数量，与 TU 级 class cache 的全局引用持有策略一致。
/// 命中路径 0 JNI：ldc Class / catch 类型匹配等热调用点逐次省 1 次 FindClass。
///
/// 并发口径（槽位 write-once）：key 经 CAS 从 NULL 一次性发布、永不改写，
/// value 由赢得槽位的线程随后写入；读侧对 value 未发布（NULL）的槽按 miss
/// 处理，自建 global（FindClass 恒等性下与表内值等价）。并发同名 miss 各建
/// 一个 global ref 属有界冗余（上限 = 并发 miss 线程数），与表项永不释放的
/// 持有策略同口径。
static jclass _mj2x_regc_cached_class_by_name(const char* internal_name)
{
    static struct { const char* volatile key; jclass value; } class_cache[64];
    for (int i = 0; i < 64; ++i) {
        const char* key = class_cache[i].key;
        jclass      cached = class_cache[i].value;
        if (key == NULL || cached == NULL) continue; /* 空槽或发布窗口：按 miss */
        if (key == internal_name || strcmp(key, internal_name) == 0) {
            return cached;
        }
    }
    jclass local = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, internal_name);
    if (local == NULL) return NULL;
    jclass global = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, local);
    (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, local);
    if (global == NULL) return NULL;
    for (int i = 0; i < 64; ++i) {
        if (class_cache[i].key != NULL) continue;
        /* CAS 抢占空槽：输家（同名被登记 / 槽被别名抢占）继续扫描或直接返回 */
        const char* prev = (const char*)_mj2x_regc_ptr_cas(
            (void* volatile*)&class_cache[i].key, NULL, (void*)internal_name);
        if (prev == NULL) {
            class_cache[i].value = global; /* 赢得槽位后发布 value，此后只读 */
            return global;
        }
        if (prev == internal_name) return global; /* 同名已被并发登记：恒等 */
    }
    return global; /* 表满：不缓存，与原实现一致 */
}

mj2x_ref _mj2x_regc_class_literal(mj2x_regc_context* context, const char* internal_name) {
    (void)context;
    if (internal_name == NULL) return NULL;
    return (mj2x_ref)_mj2x_regc_cached_class_by_name(internal_name);
}

/* xorshift32 一步推进:与 core/native_lib_crypto.hpp::xorshift32Step 同式。 */
static uint32_t _mj2x_regc_xorshift32(uint32_t state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void _mj2x_regc_decrypt_bytes(uint8_t* data, uint32_t size, uint32_t key)
{
    /* 逐字节推进密钥流再取 state & 0xFF XOR,与 xorStreamBytes 逐字节一致;
     * key == 0 用固定种子。仅由 JNI_OnLoad 单线程调用,无并发要求。 */
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    for (uint32_t i = 0; i < size; ++i) {
        state = _mj2x_regc_xorshift32(state);
        data[i] = (uint8_t)(data[i] ^ (uint8_t)(state & 0xFFu));
    }
}

/* ---- parse 族(JDK 17 语义逐条镜像) ----
 *
 * 探测口径(JDK 17.0.20.1 Temurin,实测 + java.base 源码逐字):
 *   Integer.parseInt/Long.parseLong/Short.parseShort/Byte.parseByte 与
 *   Integer.parseUnsignedInt/Long.parseUnsignedLong(含 radix 重载)对
 *   null 一律抛 NumberFormatException("Cannot parse null string");
 *   Float.parseFloat/Double.parseDouble 对 null 抛 NullPointerException
 *   (C 侧经 JNI 委派 JDK,行为由 JDK 自身保证,无需处理);
 *   Boolean.parseBoolean(null) 返回 false。
 * 消息文本逐字镜像(JDK 17 NumberFormatException.forInputString):
 *   null        -> "Cannot parse null string"
 *   radix 越界  -> "radix N less than Character.MIN_RADIX"
 *                  / "radix N greater than Character.MAX_RADIX"
 *   其余 NFE    -> "For input string: \"<s>\"" + radix != 10 时
 *                  " under radix N"
 *   parseUnsigned '-' -> "Illegal leading minus sign on unsigned string <s>."
 *   parseUnsigned 超界 -> "String value <s> exceeds range of unsigned int/long."
 *   parseUnsignedLong 拆分路径末位坏 digit -> "Bad digit at end of <s>"
 *   Short/Byte 范围 -> "Value out of range. Value:\"<s>\" Radix:<r>"
 * 已知残差:parseUnsignedLong 拆分路径(串长超阈值)中,前缀内坏 digit 的
 * JDK 消息含前缀子串而本实现含全串(值语义一致,均抛 NFE);消息缓冲
 * 1024 字节,超长输入截断(JDK 不截断,超 1K 的异常消息属病态输入)。
 *
 * digit 助手镜像 JDK 17 Character.digit(char,int) 的 BMP 行为:ASCII 快
 * 路径 + 非 ASCII Nd(十进制数字)区段 36 个(值 = ch - 区段起点)+ 全角
 * 字母别名区段 2 个(值 = ch - 起点 + 10)。区段表由 JDK 17 全 BMP 扫描
 * 生成(比 Python unicodedata 更权威:直接锁定 JDK 17 的 Unicode 14 表,
 * 扫描程序与结果见 PR 说明)。全角字母 U+FF21-FF3A/FF41-FF5A 在 radix>10
 * 时按 JDK 别名口径接受(Character.digit javadoc 明载,实测 parseInt
 * ("Ａ",16)==10),全角数字 U+FF10-FF19 属 Nd 区段;全角 '+'/'-' 不是
 * digit 也不在首字符符号判定内,自然走 NFE。surrogate 与其余字符一律 -1。
 * 解析按 modified UTF-8 解码为 UTF-16 code unit 逐位对齐 charAt 语义。 */

int32_t _mj2x_intrinsic_digit_char(uint16_t ch, int32_t radix) {
    static const uint16_t nd_lo[36] = {
        0x0660u, 0x06F0u, 0x07C0u, 0x0966u, 0x09E6u, 0x0A66u, 0x0AE6u, 0x0B66u,
        0x0BE6u, 0x0C66u, 0x0CE6u, 0x0D66u, 0x0DE6u, 0x0E50u, 0x0ED0u, 0x0F20u,
        0x1040u, 0x1090u, 0x17E0u, 0x1810u, 0x1946u, 0x19D0u, 0x1A80u, 0x1A90u,
        0x1B50u, 0x1BB0u, 0x1C40u, 0x1C50u, 0xA620u, 0xA8D0u, 0xA900u, 0xA9D0u,
        0xA9F0u, 0xAA50u, 0xABF0u, 0xFF10u,
    };
    static const uint16_t nd_hi[36] = {
        0x0669u, 0x06F9u, 0x07C9u, 0x096Fu, 0x09EFu, 0x0A6Fu, 0x0AEFu, 0x0B6Fu,
        0x0BEFu, 0x0C6Fu, 0x0CEFu, 0x0D6Fu, 0x0DEFu, 0x0E59u, 0x0ED9u, 0x0F29u,
        0x1049u, 0x1099u, 0x17E9u, 0x1819u, 0x194Fu, 0x19D9u, 0x1A89u, 0x1A99u,
        0x1B59u, 0x1BB9u, 0x1C49u, 0x1C59u, 0xA629u, 0xA8D9u, 0xA909u, 0xA9D9u,
        0xA9F9u, 0xAA59u, 0xABF9u, 0xFF19u,
    };
    int32_t value = -1;
    int32_t i;
    if (radix < 2 || radix > 36) return -1;
    if (ch < 0x80u) {
        if (ch >= (uint16_t)'0' && ch <= (uint16_t)'9') value = (int32_t)(ch - (uint16_t)'0');
        else if (ch >= (uint16_t)'A' && ch <= (uint16_t)'Z') value = (int32_t)(ch - (uint16_t)'A') + 10;
        else if (ch >= (uint16_t)'a' && ch <= (uint16_t)'z') value = (int32_t)(ch - (uint16_t)'a') + 10;
    }
    else {
        for (i = 0; i < 36; ++i) {
            if (ch >= nd_lo[i] && ch <= nd_hi[i]) { value = (int32_t)(ch - nd_lo[i]); break; }
        }
        if (value < 0) {
            if (ch >= 0xFF21u && ch <= 0xFF3Au) value = (int32_t)(ch - 0xFF21u) + 10;
            else if (ch >= 0xFF41u && ch <= 0xFF5Au) value = (int32_t)(ch - 0xFF41u) + 10;
        }
    }
    return value < radix ? value : -1;
}

/* modified UTF-8 -> UTF-16 code unit。GetStringUTFChars 的合法输出只有
 * 1/2/3 字节序列(U+0000 编码为 C0 80,补码面由调用方的首字符判定消化);
 * 其他首字节按无效单元返回 -1(防御路径,合法输入不触发)。 */
static int32_t _mj2x_regc_next_code_unit(const unsigned char** pp) {
    const unsigned char* p     = *pp;
    const unsigned char  lead  = p[0];
    if (lead < 0x80u) {
        *pp = p + 1;
        return (int32_t)lead;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        *pp = p + 2;
        return ((int32_t)(lead & 0x1Fu) << 6) | (int32_t)(p[1] & 0x3Fu);
    }
    if ((lead & 0xF0u) == 0xE0u) {
        *pp = p + 3;
        return ((int32_t)(lead & 0x0Fu) << 12) | ((int32_t)(p[1] & 0x3Fu) << 6) |
               (int32_t)(p[2] & 0x3Fu);
    }
    *pp = p + 1;
    return -1;
}

/* modified UTF-8 的 UTF-16 code unit 计数(JDK String.length() 口径) */
static int32_t _mj2x_regc_utf8_unit_count(const unsigned char* p) {
    int32_t count = 0;
    while (*p != '\0') {
        const unsigned char lead = *p;
        if (lead < 0x80u) p += 1;
        else if ((lead & 0xE0u) == 0xC0u) p += 2;
        else if ((lead & 0xF0u) == 0xE0u) p += 3;
        else p += 1;
        count++;
    }
    return count;
}

static void _mj2x_intrinsic_throw_nfe_message(mj2x_regc_context* context, const char* message) {
    if (context != NULL && context->throwException != NULL) {
        context->throwException(context->env, "java/lang/NumberFormatException", message);
    }
}

static void _mj2x_intrinsic_throw_parse_null(mj2x_regc_context* context) {
    _mj2x_intrinsic_throw_nfe_message(context, "Cannot parse null string");
}

/* JDK NumberFormatException.forInputString(s, radix):消息含全串,
 * radix != 10 时追加 " under radix N"。缓冲 1024 字节,超长截断。 */
static void _mj2x_intrinsic_throw_for_input(mj2x_regc_context* context, const char* utf,
                                            int32_t radix) {
    char buffer[1024];
    if (radix == 10) {
        snprintf(buffer, sizeof buffer, "For input string: \"%s\"", utf);
    } else {
        snprintf(buffer, sizeof buffer, "For input string: \"%s\" under radix %d", utf, radix);
    }
    _mj2x_intrinsic_throw_nfe_message(context, buffer);
}

static void _mj2x_intrinsic_throw_radix_bounds(mj2x_regc_context* context, int32_t radix) {
    char buffer[96];
    if (radix < 2) {
        snprintf(buffer, sizeof buffer, "radix %d less than Character.MIN_RADIX", radix);
    } else {
        snprintf(buffer, sizeof buffer, "radix %d greater than Character.MAX_RADIX", radix);
    }
    _mj2x_intrinsic_throw_nfe_message(context, buffer);
}

/* 有符号核心(Integer.parseInt(String,int)/Long.parseLong(String,int)):
 * 顺序 null -> radix -> 首字符符号判定 -> 逐 unit digit 累积。
 * max_abs 为正数上界(INT32_MAX / INT64_MAX);成功返回 0,*out_value 给值,
 * *out_utf 给待调用方释放的 GetStringUTFChars 结果;失败返回 -1 并挂异常
 * (utf 已释放)。 */
static int _mj2x_intrinsic_parse_signed_core(mj2x_regc_context* context, mj2x_ref jstr_ref,
                                             int32_t radix, uint64_t max_abs,
                                             int64_t* out_value, const char** out_utf) {
    const unsigned char* p;
    const char*          utf;
    uint64_t             limit;
    uint64_t             value = 0;
    int32_t              first;
    int                  negative = 0;
    int                  failed   = 0;
    if (jstr_ref == NULL) {
        _mj2x_intrinsic_throw_parse_null(context);
        return -1;
    }
    if (radix < 2 || radix > 36) {
        _mj2x_intrinsic_throw_radix_bounds(context, radix);
        return -1;
    }
    utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);
    if (utf == NULL) return -1; /* OOM:pending 已挂 */
    p = (const unsigned char*)utf;
    if (*p == '\0') {
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
        return -1;
    }
    first = _mj2x_regc_next_code_unit(&p);
    if (first < (int32_t)(unsigned char)'0') { /* 可能是 '+'/'-'(JDK firstChar<'0' 分支) */
        if (first == '-') {
            negative = 1;
        } else if (first != '+') {
            _mj2x_intrinsic_throw_for_input(context, utf, radix);
            (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
            return -1;
        }
        if (*p == '\0') { /* 孤立 '+'/'-' */
            _mj2x_intrinsic_throw_for_input(context, utf, radix);
            (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
            return -1;
        }
    }
    else {
        /* 首 unit 是数字:计入累积(JDK digit 循环自 i=0 起) */
        int32_t first_digit = _mj2x_intrinsic_digit_char((uint16_t)first, radix);
        if (first_digit < 0) {
            _mj2x_intrinsic_throw_for_input(context, utf, radix);
            (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
            return -1;
        }
        value = (uint64_t)first_digit;
    }
    limit = negative ? max_abs + UINT64_C(1) : max_abs;
    while (*p != '\0') {
        int32_t unit  = _mj2x_regc_next_code_unit(&p);
        int32_t digit = _mj2x_intrinsic_digit_char((uint16_t)unit, radix);
        if (digit < 0 || value > (limit - (uint64_t)digit) / (uint64_t)radix) {
            failed = 1;
            break;
        }
        value = value * (uint64_t)radix + (uint64_t)digit;
    }
    if (failed) {
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
        return -1;
    }
    *out_value = negative ? (int64_t)(UINT64_C(0) - value) : (int64_t)value;
    *out_utf   = utf;
    return 0;
}

static void _mj2x_intrinsic_release_utf(mj2x_ref jstr_ref, const char* utf) {
    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
}

int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_int_radix(context, jstr_ref, 10);
}

int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_long_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_signed_core(context, jstr_ref, radix, UINT64_C(2147483647),
                                          &value, &utf) != 0) {
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return (int32_t)value;
}

int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_signed_core(context, jstr_ref, radix, UINT64_C(9223372036854775807),
                                          &value, &utf) != 0) {
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return value;
}

/* parseUnsigned 核心:JDK 17 Integer.parseUnsignedInt/Long.parseUnsignedLong。
 * JDK 结构:parseInt/parseUnsignedInt 短串走有符号核心(顺带校验 radix),
 * 长串走拆分(前 n-1 位有符号解析,末位 digit 单独判定);有符号解析溢出
 * (超 Long)报 forInputString,无符号上界溢出报 exceeds range。此处按
 * unit 数复刻该分流:threshold 内单循环(INT64 限界),超阈值拆分
 * (仅 parseUnsignedLong;parseUnsignedInt 无拆分,一律全串有符号解析)。 */
static int _mj2x_intrinsic_parse_unsigned_common(mj2x_regc_context* context, mj2x_ref jstr_ref,
                                                 int32_t radix, int is_long,
                                                 int64_t* out_value, const char** out_utf) {
    const unsigned char* p;
    const char*          utf;
    uint64_t             value = 0;
    int32_t              first;
    int32_t              unit_count;
    int                  failed   = 0;
    int                  overflow = 0;
    if (jstr_ref == NULL) {
        _mj2x_intrinsic_throw_parse_null(context);
        return -1;
    }
    utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);
    if (utf == NULL) return -1; /* OOM:pending 已挂 */
    p = (const unsigned char*)utf;
    if (*p == '\0') { /* JDK:len==0 先于 '-' 判定报 forInputString */
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    first = _mj2x_regc_next_code_unit(&p);
    if (first == '-') { /* '-' 先于 radix 校验(JDK parseUnsigned 结构) */
        char buffer[1024];
        snprintf(buffer, sizeof buffer,
                 "Illegal leading minus sign on unsigned string %s.", utf);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        _mj2x_intrinsic_throw_nfe_message(context, buffer);
        return -1;
    }
    if (radix < 2 || radix > 36) { /* JDK 在 parseInt/parseLong 内校验,位于 '-' 之后 */
        _mj2x_intrinsic_throw_radix_bounds(context, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    if (first < (int32_t)(unsigned char)'0' && first != '+') {
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    if (first == (int32_t)(unsigned char)'+' && *p == '\0') { /* 孤立 '+' */
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    if (first != (int32_t)(unsigned char)'+') {
        /* 首 unit 是数字:计入累积(对齐 JDK digit 循环自 i=0 起) */
        int32_t first_digit = _mj2x_intrinsic_digit_char((uint16_t)first, radix);
        if (first_digit < 0) {
            _mj2x_intrinsic_throw_for_input(context, utf, radix);
            _mj2x_intrinsic_release_utf(jstr_ref, utf);
            return -1;
        }
        value = (uint64_t)first_digit;
    }
    unit_count = _mj2x_regc_utf8_unit_count((const unsigned char*)utf);
    if (is_long && unit_count > (radix == 10 ? 18 : 12)) {
        /* 拆分路径:前缀(全串去掉末位)按有符号 Long 限界解析(对齐 JDK
         * parseLong(s,0,len-1,radix)),末位 digit 单独判定。首 unit 已消费,
         * 余下前缀 unit 数为 unit_count-2 */
        int32_t  remain  = unit_count - 2;
        uint64_t limit   = UINT64_C(9223372036854775807);
        int32_t  last_digit;
        while (remain-- > 0) {
            int32_t unit  = _mj2x_regc_next_code_unit(&p);
            int32_t digit = _mj2x_intrinsic_digit_char((uint16_t)unit, radix);
            if (digit < 0 || value > (limit - (uint64_t)digit) / (uint64_t)radix) {
                /* JDK 该分支报 forInputString(前缀);此处含全串 */
                _mj2x_intrinsic_throw_for_input(context, utf, radix);
                _mj2x_intrinsic_release_utf(jstr_ref, utf);
                return -1;
            }
            value = value * (uint64_t)radix + (uint64_t)digit;
        }
        last_digit = _mj2x_intrinsic_digit_char((uint16_t)_mj2x_regc_next_code_unit(&p), radix);
        if (last_digit < 0) {
            char buffer[1024];
            snprintf(buffer, sizeof buffer, "Bad digit at end of %s", utf);
            _mj2x_intrinsic_release_utf(jstr_ref, utf);
            _mj2x_intrinsic_throw_nfe_message(context, buffer);
            return -1;
        }
        if (value > (UINT64_MAX - (uint64_t)last_digit) / (uint64_t)radix) {
            char buffer[1024];
            snprintf(buffer, sizeof buffer,
                     "String value %s exceeds range of unsigned long.", utf);
            _mj2x_intrinsic_release_utf(jstr_ref, utf);
            _mj2x_intrinsic_throw_nfe_message(context, buffer);
            return -1;
        }
        value = value * (uint64_t)radix + (uint64_t)last_digit;
        *out_value = (int64_t)value;
        *out_utf   = utf;
        return 0;
    }
    while (*p != '\0') {
        int32_t unit  = _mj2x_regc_next_code_unit(&p);
        int32_t digit = _mj2x_intrinsic_digit_char((uint16_t)unit, radix);
        if (digit < 0) { /* 阈值内坏 digit 走 parseLong/parseInt 口径 forInputString */
            failed = 1;
            break;
        }
        if (value > (UINT64_C(9223372036854775807) - (uint64_t)digit) / (uint64_t)radix) {
            overflow = 1;
            break;
        }
        value = value * (uint64_t)radix + (uint64_t)digit;
    }
    if (failed) {
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    if (overflow) {
        /* 超有符号 Long:JDK 短串路径 parseLong 报 forInputString */
        _mj2x_intrinsic_throw_for_input(context, utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        return -1;
    }
    if (!is_long && value > UINT64_C(0xFFFFFFFF)) {
        char buffer[1024];
        snprintf(buffer, sizeof buffer, "String value %s exceeds range of unsigned int.", utf);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        _mj2x_intrinsic_throw_nfe_message(context, buffer);
        return -1;
    }
    *out_value = (int64_t)value;
    *out_utf   = utf;
    return 0;
}

int32_t _mj2x_intrinsic_parse_unsigned_int(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_unsigned_int_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_unsigned_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref,
                                                 int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_unsigned_common(context, jstr_ref, radix, 0, &value, &utf) != 0) {
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return (int32_t)(uint32_t)(uint64_t)value;
}

int64_t _mj2x_intrinsic_parse_unsigned_long(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_unsigned_long_radix(context, jstr_ref, 10);
}

int64_t _mj2x_intrinsic_parse_unsigned_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref,
                                                  int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_unsigned_common(context, jstr_ref, radix, 1, &value, &utf) != 0) {
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return value;
}

int32_t _mj2x_intrinsic_parse_short(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_short_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_signed_core(context, jstr_ref, radix, UINT64_C(2147483647),
                                          &value, &utf) != 0) {
        return 0;
    }
    if (value < INT16_MIN || value > INT16_MAX) {
        char buffer[1024];
        snprintf(buffer, sizeof buffer, "Value out of range. Value:\"%s\" Radix:%d", utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        _mj2x_intrinsic_throw_nfe_message(context, buffer);
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return (int32_t)(int16_t)value;
}

int32_t _mj2x_intrinsic_parse_byte(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_byte_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int64_t     value;
    const char* utf;
    if (_mj2x_intrinsic_parse_signed_core(context, jstr_ref, radix, UINT64_C(2147483647),
                                          &value, &utf) != 0) {
        return 0;
    }
    if (value < INT8_MIN || value > INT8_MAX) {
        char buffer[1024];
        snprintf(buffer, sizeof buffer, "Value out of range. Value:\"%s\" Radix:%d", utf, radix);
        _mj2x_intrinsic_release_utf(jstr_ref, utf);
        _mj2x_intrinsic_throw_nfe_message(context, buffer);
        return 0;
    }
    _mj2x_intrinsic_release_utf(jstr_ref, utf);
    return (int32_t)(int8_t)value;
}

int32_t _mj2x_intrinsic_parse_boolean(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    (void)context;
    if (jstr_ref == NULL) return 0;
    if ((*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)jstr_ref) != 4) return 0;
    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);
    if (chars == NULL) return 0;
    int32_t result = ((chars[0] | 0x20) == 't' && (chars[1] | 0x20) == 'r' && (chars[2] | 0x20) == 'u' && (chars[3] | 0x20) == 'e') ? 1 : 0;
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)jstr_ref, chars);
    return result;
}

float _mj2x_intrinsic_parse_float(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    (void)context;
    jmethodID method = _mj2x_regc_cached_static_method(&_mj2x_rt_float_parse_float_cache);
    if (method == NULL || _mj2x_rt_float_parse_float_cache.ownerClass == NULL) return 0.0f;
    jvalue args[1]; args[0].l = (jobject)jstr_ref;
    return (*_mj2x_regc_env)->CallStaticFloatMethodA(_mj2x_regc_env, _mj2x_rt_float_parse_float_cache.ownerClass, method, args);
}

double _mj2x_intrinsic_parse_double(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    (void)context;
    jmethodID method = _mj2x_regc_cached_static_method(&_mj2x_rt_double_parse_double_cache);
    if (method == NULL || _mj2x_rt_double_parse_double_cache.ownerClass == NULL) return 0.0;
    jvalue args[1]; args[0].l = (jobject)jstr_ref;
    return (*_mj2x_regc_env)->CallStaticDoubleMethodA(_mj2x_regc_env, _mj2x_rt_double_parse_double_cache.ownerClass, method, args);
}

mj2x_ref _mj2x_intrinsic_float_to_string(mj2x_regc_context* context, float value) {
    (void)context;
    jmethodID value_of = _mj2x_regc_cached_static_method(&_mj2x_rt_string_value_of_float_cache);
    if (value_of == NULL || _mj2x_rt_string_value_of_float_cache.ownerClass == NULL) return NULL;
    jvalue args[1];
    args[0].f = (jfloat)value;
    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, _mj2x_rt_string_value_of_float_cache.ownerClass, value_of, args);
}

mj2x_ref _mj2x_intrinsic_double_to_string(mj2x_regc_context* context, double value) {
    (void)context;
    jmethodID value_of = _mj2x_regc_cached_static_method(&_mj2x_rt_string_value_of_double_cache);
    if (value_of == NULL || _mj2x_rt_string_value_of_double_cache.ownerClass == NULL) return NULL;
    jvalue args[1];
    args[0].d = (jdouble)value;
    return (mj2x_ref)(*_mj2x_regc_env)->CallStaticObjectMethodA(_mj2x_regc_env, _mj2x_rt_string_value_of_double_cache.ownerClass, value_of, args);
}

mj2x_ref _mj2x_intrinsic_object_to_string(mj2x_regc_context* context, mj2x_ref value) {
    (void)context;
    if (value == NULL) return (mj2x_ref)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, "null");
    jmethodID to_string = _mj2x_regc_cached_instance_method(&_mj2x_rt_object_to_string_method_cache, (jobject)value);
    if (to_string == NULL) return NULL;
    return (mj2x_ref)(*_mj2x_regc_env)->CallObjectMethod(_mj2x_regc_env, (jobject)value, to_string);
}

int32_t _mj2x_intrinsic_is_string(mj2x_ref value) {
    if (value == NULL) return 0;
    jclass cls = _mj2x_regc_cached_class(&_mj2x_rt_string_class_cache);
    if (cls == NULL) return 0;
    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;
}

int32_t _mj2x_intrinsic_string_length(mj2x_ref value) {
    if (value == NULL) { _mj2x_regc_throw_null("String.length"); return 0; }
    return (int32_t)(*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
}

int32_t _mj2x_intrinsic_string_is_empty(mj2x_ref value) {
    if (value == NULL) { _mj2x_regc_throw_null("String.isEmpty"); return 0; }
    return (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value) == 0;
}

int32_t _mj2x_intrinsic_string_starts_with(mj2x_ref value, mj2x_ref prefix) {
    if (value == NULL) { _mj2x_regc_throw_null("String.startsWith"); return 0; }
    if (prefix == NULL) { _mj2x_regc_throw_null("String.startsWith prefix"); return 0; }
    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
    jsize prefix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)prefix);
    if (prefix_len > value_len) return 0;
    if (prefix_len == 0) return 1;
    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)value, NULL);
    if (value_chars == NULL) return 0;
    const jchar* prefix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)prefix, NULL);
    if (prefix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars); return 0; }
    int32_t same = 1;
    for (jsize i = 0; i < prefix_len; ++i) { if (value_chars[i] != prefix_chars[i]) { same = 0; break; } }
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars);
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)prefix, prefix_chars);
    return same;
}

int32_t _mj2x_intrinsic_string_ends_with(mj2x_ref value, mj2x_ref suffix) {
    if (value == NULL) { _mj2x_regc_throw_null("String.endsWith"); return 0; }
    if (suffix == NULL) { _mj2x_regc_throw_null("String.endsWith suffix"); return 0; }
    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
    jsize suffix_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)suffix);
    if (suffix_len > value_len) return 0;
    if (suffix_len == 0) return 1;
    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)value, NULL);
    if (value_chars == NULL) return 0;
    const jchar* suffix_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)suffix, NULL);
    if (suffix_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars); return 0; }
    int32_t same = 1;
    jsize start = value_len - suffix_len;
    for (jsize i = 0; i < suffix_len; ++i) { if (value_chars[start + i] != suffix_chars[i]) { same = 0; break; } }
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars);
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)suffix, suffix_chars);
    return same;
}

int32_t _mj2x_intrinsic_string_index_of(mj2x_ref value, mj2x_ref needle) {
    if (value == NULL) { _mj2x_regc_throw_null("String.indexOf"); return -1; }
    if (needle == NULL) { _mj2x_regc_throw_null("String.indexOf needle"); return -1; }
    jsize value_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
    jsize needle_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)needle);
    if (needle_len == 0) return 0;
    if (needle_len > value_len) return -1;
    const jchar* value_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)value, NULL);
    if (value_chars == NULL) return -1;
    const jchar* needle_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)needle, NULL);
    if (needle_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars); return -1; }
    int32_t found = -1;
    for (jsize i = 0; i <= value_len - needle_len; ++i) {
        jsize j = 0;
        while (j < needle_len && value_chars[i + j] == needle_chars[j]) j++;
        if (j == needle_len) { found = (int32_t)i; break; }
    }
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, value_chars);
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)needle, needle_chars);
    return found;
}

jmethodID _mj2x_intrinsic_string_contains_method = NULL;
int32_t _mj2x_intrinsic_string_contains(mj2x_ref value, mj2x_ref seq) {
    if (value == NULL) { _mj2x_regc_throw_null("String.contains"); return 0; }
    if (seq == NULL) { _mj2x_regc_throw_null("String.contains sequence"); return 0; }
    if (_mj2x_intrinsic_is_string(seq)) {
        return _mj2x_intrinsic_string_index_of(value, seq) >= 0;
    }
    jclass cls = _mj2x_regc_cached_class(&_mj2x_rt_string_class_cache);
    if (cls == NULL) return 0;
    if (_mj2x_intrinsic_string_contains_method == NULL) {
        _mj2x_intrinsic_string_contains_method = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, cls, "contains", "(Ljava/lang/CharSequence;)Z");
        if (_mj2x_intrinsic_string_contains_method == NULL) return 0;
    }
    return (*_mj2x_regc_env)->CallBooleanMethod(_mj2x_regc_env, (jobject)value, _mj2x_intrinsic_string_contains_method, (jobject)seq) == JNI_TRUE;
}

// invokedynamic (stage 3) box/unbox helpers: primitive arguments are boxed
// into Object[]; invokeWithArguments results are unboxed back into scalar
// registers. class/methodID are cached globally to avoid repeated lookups.
/// 小值包装类缓存：JDK Integer/Long/Short/Byte 的 valueOf 对 [-128,127]、
/// Character 对 [0,127]、Boolean 对 TRUE/FALSE 恒返回同一缓存对象。
/// 这里的表缓存的是 JDK valueOf 返回对象的全局引用——命中时返回的对象与
/// JDK valueOf 逐次调用对象恒等（IsSameObject/== 语义完全一致），越界值
/// 恒透传 JDK 调用（每次新对象，同样一致）。表项与 JDK 自身 cache 同寿命，
/// 泄漏上界 256+128+2 个 bootstrap 类包装对象。
static jobject _mj2x_regc_box_cached(jobject* table, int32_t slot)
{
    if (slot < 0 || slot > 255) return NULL;
    return table[slot];
}

static void _mj2x_regc_box_fill(jobject* table, int32_t slot, jobject box)
{
    if (slot < 0 || slot > 255 || box == NULL) return;
    jobject global = (*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, box);
    /* 竞态并发回填无害：JDK cache 对同值恒返回同一对象，两线程写入等价。
       NewGlobalRef 失败（OOM）时槽位留空，下次调用重试。 */
    if (global != NULL) table[slot] = global;
}

jobject _mj2x_regc_box_i32(int32_t value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[256]; if (value >= -128 && value <= 127) { jobject hit = _mj2x_regc_box_cached(cache, value + 128); if (hit != NULL) return hit; } if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Integer"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(I)Ljava/lang/Integer;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); if (box != NULL && value >= -128 && value <= 127) _mj2x_regc_box_fill(cache, value + 128, box); return box; }
jobject _mj2x_regc_box_i64(int64_t value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[256]; if (value >= -128 && value <= 127) { jobject hit = _mj2x_regc_box_cached(cache, (int32_t)(value + 128)); if (hit != NULL) return hit; } if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Long"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(J)Ljava/lang/Long;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); if (box != NULL && value >= -128 && value <= 127) _mj2x_regc_box_fill(cache, (int32_t)(value + 128), box); return box; }
jobject _mj2x_regc_box_i8(int8_t value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[256]; jobject hit = _mj2x_regc_box_cached(cache, (int32_t)value + 128); if (hit != NULL) return hit; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Byte"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(B)Ljava/lang/Byte;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); _mj2x_regc_box_fill(cache, (int32_t)value + 128, box); return box; }
jobject _mj2x_regc_box_i16(int16_t value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[256]; jobject hit = _mj2x_regc_box_cached(cache, (int32_t)value + 128); if (hit != NULL) return hit; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Short"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(S)Ljava/lang/Short;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); _mj2x_regc_box_fill(cache, (int32_t)value + 128, box); return box; }
jobject _mj2x_regc_box_char(uint16_t value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[128]; jobject hit = (value <= 127) ? cache[value] : NULL; if (hit != NULL) return hit; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Character"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(C)Ljava/lang/Character;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); if (box != NULL && value <= 127) { jobject global = (*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, box); if (global != NULL) cache[value] = global; } return box; }
jobject _mj2x_regc_box_bool(bool value) { static jclass c = NULL; static jmethodID m = NULL; static jobject cache[2]; jobject hit = cache[value ? 1 : 0]; if (hit != NULL) return hit; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Boolean"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(Z)Ljava/lang/Boolean;"); if (c == NULL || m == NULL) return NULL; jobject box = (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value ? JNI_TRUE : JNI_FALSE); if (box != NULL) { jobject global = (*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, box); if (global != NULL) cache[value ? 1 : 0] = global; } return box; }
jobject _mj2x_regc_box_f32(float value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Float"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(F)Ljava/lang/Float;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
jobject _mj2x_regc_box_f64(double value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Double"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(D)Ljava/lang/Double;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
int32_t _mj2x_regc_unbox_i32(jobject value) { if (value == NULL) return 0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Integer"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "intValue", "()I"); if (c == NULL || m == NULL) return 0; return (*_mj2x_regc_env)->CallIntMethod(_mj2x_regc_env, value, m); }
int64_t _mj2x_regc_unbox_i64(jobject value) { if (value == NULL) return 0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Long"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "longValue", "()J"); if (c == NULL || m == NULL) return 0; return (*_mj2x_regc_env)->CallLongMethod(_mj2x_regc_env, value, m); }
float _mj2x_regc_unbox_f32(jobject value) { if (value == NULL) return 0.0f; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Float"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "floatValue", "()F"); if (c == NULL || m == NULL) return 0.0f; return (*_mj2x_regc_env)->CallFloatMethod(_mj2x_regc_env, value, m); }
double _mj2x_regc_unbox_f64(jobject value) { if (value == NULL) return 0.0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Double"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "doubleValue", "()D"); if (c == NULL || m == NULL) return 0.0; return (*_mj2x_regc_env)->CallDoubleMethod(_mj2x_regc_env, value, m); }



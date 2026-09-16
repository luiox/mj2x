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
mj2x_regc_class_cache _mj2x_rt_sioobe_class_cache = {"java/lang/StringIndexOutOfBoundsException", NULL};
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
    if (desc[0] == 'L') {
        char name[1024];
        int i = 0;
        while (desc[i + 1] != ';' && desc[i + 1] != '\0' && i < 1023) { name[i] = desc[i + 1]; i++; }
        name[i] = '\0';
        return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, name);
    }
    return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, desc);
}

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
    jclass cls = _mj2x_regc_find_class(catch_type);
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

mj2x_ref _mj2x_regc_class_literal(mj2x_regc_context* context, const char* internal_name) {
    (void)context;
    if (internal_name == NULL) return NULL;
    return (mj2x_ref)(*_mj2x_regc_env)->FindClass(_mj2x_regc_env, internal_name);
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

int32_t _mj2x_intrinsic_digit_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return (int32_t)(ch - '0');
    if (ch >= 'a' && ch <= 'z') return (int32_t)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'Z') return (int32_t)(ch - 'A') + 10;
    return -1;
}

void _mj2x_intrinsic_throw_number_format(mj2x_regc_context* context) {
    if (context != NULL && context->throwException != NULL) {
        context->throwException(context->env, "java/lang/NumberFormatException", "For input string");
    }
}

int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);

int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);
int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix);

int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_int_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_int_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); return 0; }
    if (jstr_ref == NULL) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(context->env, "java/lang/NullPointerException", "parseInt");
        }
        return 0;
    }
    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);
    if (utf == NULL) return 0;
    const unsigned char* p = (const unsigned char*)utf;
    int negative = 0;
    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }
    uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;
    uint64_t value = 0;
    int digit_count = 0;
    int failed = 0;
    while (*p != '\0') {
        int32_t digit = _mj2x_intrinsic_digit_value(*p++);
        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / (uint64_t)radix) { failed = 1; break; }
        value = value * (uint64_t)radix + (uint64_t)digit;
        digit_count++;
    }
    if (digit_count == 0) failed = 1;
    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
    if (failed) {
        _mj2x_intrinsic_throw_number_format(context);
        return 0;
    }
    if (negative) {
        if (value == 2147483648ULL) return (int32_t)INT32_MIN;
        return -(int32_t)value;
    }
    return (int32_t)value;
}

int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_long_radix(context, jstr_ref, 10);
}

int64_t _mj2x_intrinsic_parse_long_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    if (radix < 2 || radix > 36) { _mj2x_intrinsic_throw_number_format(context); return 0; }
    if (jstr_ref == NULL) {
        if (context != NULL && context->throwException != NULL) {
            context->throwException(context->env, "java/lang/NullPointerException", "parseLong");
        }
        return 0;
    }
    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);
    if (utf == NULL) return 0;
    const unsigned char* p = (const unsigned char*)utf;
    int negative = 0;
    if (*p == '-' || *p == '+') { negative = (*p == '-'); p++; }
    uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    uint64_t value = 0;
    int digit_count = 0;
    int failed = 0;
    while (*p != '\0') {
        int32_t digit = _mj2x_intrinsic_digit_value(*p++);
        if (digit < 0 || digit >= radix || value > (limit - (uint64_t)digit) / (uint64_t)radix) { failed = 1; break; }
        value = value * (uint64_t)radix + (uint64_t)digit;
        digit_count++;
    }
    if (digit_count == 0) failed = 1;
    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);
    if (failed) {
        _mj2x_intrinsic_throw_number_format(context);
        return 0;
    }
    if (negative) {
        if (value == 9223372036854775808ULL) return (int64_t)INT64_MIN;
        return -(int64_t)value;
    }
    return (int64_t)value;
}

int32_t _mj2x_intrinsic_parse_short(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_short_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_short_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);
    if (_mj2x_regc_exception_check()) return 0;
    if (value < INT16_MIN || value > INT16_MAX) { _mj2x_intrinsic_throw_number_format(context); return 0; }
    return (int32_t)(int16_t)value;
}

int32_t _mj2x_intrinsic_parse_byte(mj2x_regc_context* context, mj2x_ref jstr_ref) {
    return _mj2x_intrinsic_parse_byte_radix(context, jstr_ref, 10);
}

int32_t _mj2x_intrinsic_parse_byte_radix(mj2x_regc_context* context, mj2x_ref jstr_ref, int32_t radix) {
    int32_t value = _mj2x_intrinsic_parse_int_radix(context, jstr_ref, radix);
    if (_mj2x_regc_exception_check()) return 0;
    if (value < INT8_MIN || value > INT8_MAX) { _mj2x_intrinsic_throw_number_format(context); return 0; }
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

int32_t _mj2x_intrinsic_string_equals(mj2x_ref lhs, mj2x_ref rhs) {
    if (lhs == NULL) { _mj2x_regc_throw_null("String.equals"); return 0; }
    if (lhs == rhs) return 1;
    if (rhs == NULL || !_mj2x_intrinsic_is_string(rhs)) return 0;
    jsize lhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)lhs);
    jsize rhs_len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)rhs);
    if (lhs_len != rhs_len) return 0;
    if (lhs_len == 0) return 1;
    const jchar* lhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)lhs, NULL);
    if (lhs_chars == NULL) return 0;
    const jchar* rhs_chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)rhs, NULL);
    if (rhs_chars == NULL) { (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)lhs, lhs_chars); return 0; }
    int32_t same = 1;
    for (jsize i = 0; i < lhs_len; ++i) { if (lhs_chars[i] != rhs_chars[i]) { same = 0; break; } }
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)lhs, lhs_chars);
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)rhs, rhs_chars);
    return same;
}

int32_t _mj2x_intrinsic_string_length(mj2x_ref value) {
    if (value == NULL) { _mj2x_regc_throw_null("String.length"); return 0; }
    return (int32_t)(*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
}

uint16_t _mj2x_intrinsic_string_char_at(mj2x_ref value, int32_t index) {
    if (value == NULL) { _mj2x_regc_throw_null("String.charAt"); return 0; }
    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
    if (index < 0 || index >= len) {
        jclass ex = _mj2x_regc_cached_class(&_mj2x_rt_sioobe_class_cache);
        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, "String.charAt");
        return 0;
    }
    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)value, NULL);
    if (chars == NULL) return 0;
    uint16_t ch = (uint16_t)chars[index];
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);
    return ch;
}

int32_t _mj2x_intrinsic_string_hash_code(mj2x_ref value) {
    if (value == NULL) { _mj2x_regc_throw_null("String.hashCode"); return 0; }
    jsize len = (*_mj2x_regc_env)->GetStringLength(_mj2x_regc_env, (jstring)value);
    if (len == 0) return 0;
    const jchar* chars = (*_mj2x_regc_env)->GetStringChars(_mj2x_regc_env, (jstring)value, NULL);
    if (chars == NULL) return 0;
    uint32_t hash = 0;
    for (jsize i = 0; i < len; ++i) hash = hash * 31u + (uint32_t)chars[i];
    (*_mj2x_regc_env)->ReleaseStringChars(_mj2x_regc_env, (jstring)value, chars);
    return (int32_t)hash;
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
jobject _mj2x_regc_box_i32(int32_t value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Integer"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(I)Ljava/lang/Integer;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
jobject _mj2x_regc_box_i64(int64_t value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Long"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(J)Ljava/lang/Long;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
jobject _mj2x_regc_box_f32(float value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Float"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(F)Ljava/lang/Float;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
jobject _mj2x_regc_box_f64(double value) { static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Double"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetStaticMethodID(_mj2x_regc_env, c, "valueOf", "(D)Ljava/lang/Double;"); if (c == NULL || m == NULL) return NULL; return (*_mj2x_regc_env)->CallStaticObjectMethod(_mj2x_regc_env, c, m, value); }
int32_t _mj2x_regc_unbox_i32(jobject value) { if (value == NULL) return 0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Integer"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "intValue", "()I"); if (c == NULL || m == NULL) return 0; return (*_mj2x_regc_env)->CallIntMethod(_mj2x_regc_env, value, m); }
int64_t _mj2x_regc_unbox_i64(jobject value) { if (value == NULL) return 0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Long"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "longValue", "()J"); if (c == NULL || m == NULL) return 0; return (*_mj2x_regc_env)->CallLongMethod(_mj2x_regc_env, value, m); }
float _mj2x_regc_unbox_f32(jobject value) { if (value == NULL) return 0.0f; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Float"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "floatValue", "()F"); if (c == NULL || m == NULL) return 0.0f; return (*_mj2x_regc_env)->CallFloatMethod(_mj2x_regc_env, value, m); }
double _mj2x_regc_unbox_f64(jobject value) { if (value == NULL) return 0.0; static jclass c = NULL; static jmethodID m = NULL; if (c == NULL) { jclass l = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, "java/lang/Double"); if (l != NULL) c = (jclass)(*_mj2x_regc_env)->NewGlobalRef(_mj2x_regc_env, l); (*_mj2x_regc_env)->DeleteLocalRef(_mj2x_regc_env, l); } if (c != NULL && m == NULL) m = (*_mj2x_regc_env)->GetMethodID(_mj2x_regc_env, c, "doubleValue", "()D"); if (c == NULL || m == NULL) return 0.0; return (*_mj2x_regc_env)->CallDoubleMethod(_mj2x_regc_env, value, m); }



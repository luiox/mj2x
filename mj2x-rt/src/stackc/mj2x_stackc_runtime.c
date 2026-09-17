/// @file mj2x_stackc_runtime.c
/// @brief StackC 运行时 slow-path 实现(#894 运行时/模板分离下沉)。
///
/// 原为 resources/codegen/stackc_runtime_prelude.c.in 的逐 TU 内联文本,
/// 现物理下沉 mj2x-rt;fast-path 缓存原语仍在头内(static __inline)。
/// 语义逐位不变。

#include "stackc/mj2x_stackc_runtime.h"

jclass _mj2x_find_class(JNIEnv* env, jclass owner, const char* name)
{
    if (name == NULL) return NULL;
    if (owner == NULL) return (*env)->FindClass(env, name);
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    if ((*env)->ExceptionCheck(env)) return NULL;
    jmethodID get_loader =
        (*env)->GetMethodID(env, class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, class_class);
        return NULL;
    }
    jobject loader = (*env)->CallObjectMethod(env, owner, get_loader);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, class_class);
        return NULL;
    }
    if (loader == NULL) {
        jclass result = (*env)->FindClass(env, name);
        (*env)->DeleteLocalRef(env, class_class);
        return result;
    }
    char binary_name[1024];
    int  i = 0;
    while (name[i] != '\0' && i < 1023) {
        binary_name[i] = (name[i] == '/') ? '.' : name[i];
        i++;
    }
    binary_name[i] = '\0';
    jstring class_name = (*env)->NewStringUTF(env, binary_name);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, class_class);
        return NULL;
    }
    jmethodID for_name = (*env)->GetStaticMethodID(
        env, class_class, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, class_name);
        (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, class_class);
        return NULL;
    }
    jclass result =
        (jclass)(*env)->CallStaticObjectMethod(env, class_class, for_name, class_name, JNI_FALSE, loader);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, class_name);
        (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, class_class);
        return NULL;
    }
    (*env)->DeleteLocalRef(env, class_name);
    (*env)->DeleteLocalRef(env, loader);
    (*env)->DeleteLocalRef(env, class_class);
    return result;
}

jclass _mj2x_component_class(JNIEnv* env, jclass owner, const char* desc)
{
    if (desc[0] == '[') return _mj2x_find_class(env, owner, desc);
    if (desc[0] == 'L') {
        char name[1024];
        int  i = 0;
        while (desc[i + 1] != ';' && desc[i + 1] != '\0' && i < 1023) {
            name[i] = desc[i + 1];
            i++;
        }
        name[i] = '\0';
        return _mj2x_find_class(env, owner, name);
    }
    return (*env)->FindClass(env, "java/lang/Object");
}

jobject _mj2x_new_multi_array(JNIEnv* env, jclass owner, const char* desc, jint* dims, int dimCount)
{
    jint        len       = dims[0];
    const char* component = desc + 1;
    if (dimCount == 1) {
        switch (component[0]) {
        case 'Z': return (jobject)(*env)->NewBooleanArray(env, len);
        case 'B': return (jobject)(*env)->NewByteArray(env, len);
        case 'C': return (jobject)(*env)->NewCharArray(env, len);
        case 'S': return (jobject)(*env)->NewShortArray(env, len);
        case 'I': return (jobject)(*env)->NewIntArray(env, len);
        case 'J': return (jobject)(*env)->NewLongArray(env, len);
        case 'F': return (jobject)(*env)->NewFloatArray(env, len);
        case 'D': return (jobject)(*env)->NewDoubleArray(env, len);
        default: {
            jclass component_class = _mj2x_component_class(env, owner, component);
            jobject result         = (jobject)(*env)->NewObjectArray(env, len, component_class, NULL);
            if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);
            return result;
        }
        }
    }
    jclass       component_class = _mj2x_find_class(env, owner, component);
    jobjectArray outer           = (*env)->NewObjectArray(env, len, component_class, NULL);
    if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);
    for (jint i = 0; i < len; ++i) {
        jobject child = _mj2x_new_multi_array(env, owner, component, dims + 1, dimCount - 1);
        (*env)->SetObjectArrayElement(env, outer, i, child);
        if (child != NULL) (*env)->DeleteLocalRef(env, child);
    }
    return (jobject)outer;
}

jmethodID _mj2x_mid_slow(JNIEnv* env, jmethodID* slot, jclass cls, const char* name,
                         const char* desc, int is_static)
{
    jmethodID loaded = is_static ? (*env)->GetStaticMethodID(env, cls, name, desc)
                                 : (*env)->GetMethodID(env, cls, name, desc);
    if (loaded != NULL) {
        void* previous = _mj2x_cache_cas((void* volatile*)slot, NULL, loaded);
        return (jmethodID)(previous != NULL ? previous : loaded);
    }
    return NULL;
}

jfieldID _mj2x_fid_slow(JNIEnv* env, jfieldID* slot, jclass cls, const char* name,
                        const char* desc, int is_static)
{
    jfieldID loaded = is_static ? (*env)->GetStaticFieldID(env, cls, name, desc)
                                : (*env)->GetFieldID(env, cls, name, desc);
    if (loaded != NULL) {
        void* previous = _mj2x_cache_cas((void* volatile*)slot, NULL, loaded);
        return (jfieldID)(previous != NULL ? previous : loaded);
    }
    return NULL;
}

jclass _mj2x_class_slow(JNIEnv* env, jclass* slot, jclass owner, const char* name)
{
    jclass loaded = (jclass)_mj2x_find_class(env, owner, name);
    if (loaded != NULL) {
        jclass global = (jclass)(*env)->NewGlobalRef(env, loaded);
        (*env)->DeleteLocalRef(env, loaded);
        if (global != NULL) {
            void* previous = _mj2x_cache_cas((void* volatile*)slot, NULL, global);
            return (jclass)(previous != NULL ? previous : global);
        }
    }
    return NULL;
}

/* xorshift32 一步推进:与 core/native_lib_crypto.hpp::xorshift32Step 及
 * NativeLoader 字节码内实现同式(x^=x<<13; x^=x>>17; x^=x<<5)。 */
static uint32_t mj2x_stackc_xorshift32(uint32_t state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void mj2x_stackc_string_pool_decrypt(uint16_t* pool, size_t unit_count, uint32_t key)
{
    /* 逐单位推进密钥流再取低 16 位 XOR(不依赖字节序,与发射侧加密逐位互逆);
     * key == 0 用固定种子,与 xorStreamBytes 同约定。 */
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    for (size_t i = 0; i < unit_count; ++i) {
        state = mj2x_stackc_xorshift32(state);
        pool[i] = (uint16_t)(pool[i] ^ (uint16_t)(state & 0xFFFFu));
    }
}

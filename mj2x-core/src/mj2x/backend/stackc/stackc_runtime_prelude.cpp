#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"

#include <string>

namespace mj2x::backend {

std::string stackCRuntimePrelude() {
    std::string s;
    s.reserve(4096);

    s += "#define _MJ2X_LOCAL_REF_LIMIT 512\n\n";

    s += "static jobject _mj2x_track_local_ref(JNIEnv* env, jobject* refs, int* refCount, jobject ref) {\n";
    s += "    (void)env;\n";
    s += "    if (ref != NULL && *refCount < _MJ2X_LOCAL_REF_LIMIT) refs[(*refCount)++] = ref;\n";
    s += "    return ref;\n";
    s += "}\n\n";

    s += "static void _mj2x_clear_local_refs(JNIEnv* env, jobject* refs, int* refCount, jobject keep) {\n";
    s += "    for (int i = 0; i < *refCount; ++i) {\n";
    s += "        if (refs[i] != NULL && refs[i] != keep) (*env)->DeleteLocalRef(env, refs[i]);\n";
    s += "        refs[i] = NULL;\n";
    s += "    }\n";
    s += "    *refCount = 0;\n";
    s += "}\n\n";

    s += "static jclass _mj2x_find_class(JNIEnv* env, jclass owner, const char* name) {\n";
    s += "    if (name == NULL) return NULL;\n";
    s += "    if (owner == NULL) return (*env)->FindClass(env, name);\n";
    s += "    jclass class_class = (*env)->FindClass(env, \"java/lang/Class\");\n";
    s += "    if ((*env)->ExceptionCheck(env)) return NULL;\n";
    s += "    jmethodID get_loader = (*env)->GetMethodID(env, class_class, \"getClassLoader\", \"()Ljava/lang/ClassLoader;\");\n";
    s += "    if ((*env)->ExceptionCheck(env)) return NULL;\n";
    s += "    jobject loader = (*env)->CallObjectMethod(env, owner, get_loader);\n";
    s += "    if ((*env)->ExceptionCheck(env)) return NULL;\n";
    s += "    if (loader == NULL) {\n";
    s += "        jclass result = (*env)->FindClass(env, name);\n";
    s += "        (*env)->DeleteLocalRef(env, class_class);\n";
    s += "        return result;\n";
    s += "    }\n";
    s += "    char binary_name[1024];\n";
    s += "    int i = 0;\n";
    s += "    while (name[i] != '\\0' && i < 1023) { binary_name[i] = (name[i] == '/') ? '.' : name[i]; i++; }\n";
    s += "    binary_name[i] = '\\0';\n";
    s += "    jstring class_name = (*env)->NewStringUTF(env, binary_name);\n";
    s += "    if ((*env)->ExceptionCheck(env)) return NULL;\n";
    s += "    jmethodID for_name = (*env)->GetStaticMethodID(env, class_class, \"forName\", \"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;\");\n";
    s += "    if ((*env)->ExceptionCheck(env)) return NULL;\n";
    s += "    jclass result = (jclass)(*env)->CallStaticObjectMethod(env, class_class, for_name, class_name, JNI_FALSE, loader);\n";
    s += "    (*env)->DeleteLocalRef(env, class_name);\n";
    s += "    (*env)->DeleteLocalRef(env, loader);\n";
    s += "    (*env)->DeleteLocalRef(env, class_class);\n";
    s += "    return result;\n";
    s += "}\n\n";

    s += "static jclass _mj2x_component_class(JNIEnv* env, jclass owner, const char* desc) {\n";
    s += "    if (desc[0] == '[') return _mj2x_find_class(env, owner, desc);\n";
    s += "    if (desc[0] == 'L') {\n";
    s += "        char name[1024];\n";
    s += "        int i = 0;\n";
    s += "        while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = desc[i + 1]; i++; }\n";
    s += "        name[i] = '\\0';\n";
    s += "        return _mj2x_find_class(env, owner, name);\n";
    s += "    }\n";
    s += "    return (*env)->FindClass(env, \"java/lang/Object\");\n";
    s += "}\n\n";

    s += "static jobject _mj2x_new_multi_array(JNIEnv* env, jclass owner, const char* desc, jint* dims, int dimCount) {\n";
    s += "    jint len = dims[0];\n";
    s += "    const char* component = desc + 1;\n";
    s += "    if (dimCount == 1) {\n";
    s += "        switch (component[0]) {\n";
    s += "        case 'Z': return (jobject)(*env)->NewBooleanArray(env, len);\n";
    s += "        case 'B': return (jobject)(*env)->NewByteArray(env, len);\n";
    s += "        case 'C': return (jobject)(*env)->NewCharArray(env, len);\n";
    s += "        case 'S': return (jobject)(*env)->NewShortArray(env, len);\n";
    s += "        case 'I': return (jobject)(*env)->NewIntArray(env, len);\n";
    s += "        case 'J': return (jobject)(*env)->NewLongArray(env, len);\n";
    s += "        case 'F': return (jobject)(*env)->NewFloatArray(env, len);\n";
    s += "        case 'D': return (jobject)(*env)->NewDoubleArray(env, len);\n";
    s += "        default: {\n";
    s += "            jclass component_class = _mj2x_component_class(env, owner, component);\n";
    s += "            jobject result = (jobject)(*env)->NewObjectArray(env, len, component_class, NULL);\n";
    s += "            if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);\n";
    s += "            return result;\n";
    s += "        }\n";
    s += "        }\n";
    s += "    }\n";
    s += "    jclass component_class = _mj2x_find_class(env, owner, component);\n";
    s += "    jobjectArray outer = (*env)->NewObjectArray(env, len, component_class, NULL);\n";
    s += "    if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);\n";
    s += "    for (jint i = 0; i < len; ++i) {\n";
    s += "        jobject child = _mj2x_new_multi_array(env, owner, component, dims + 1, dimCount - 1);\n";
    s += "        (*env)->SetObjectArrayElement(env, outer, i, child);\n";
    s += "        if (child != NULL) (*env)->DeleteLocalRef(env, child);\n";
    s += "    }\n";
    s += "    return (jobject)outer;\n";
    s += "}\n\n";

    return s;
}

}  // namespace mj2x::backend

#include "mj2x/backend/cbackend/c_runtime_prelude.hpp"

#include <string>

namespace mj2x::backend {

std::string cBackendRuntimePrelude() {
    // These two helpers are emitted verbatim into every CBackend TU.  They
    // resolve a JVM class descriptor and recursively allocate a multi-dim
    // array; both are `static` so they are local to the TU and stripped by the
    // linker when unused.  The body matches the original inline emit so the
    // generated source stays byte-compatible with prior CBackend output.
    std::string s;
    s.reserve(1024);

    s += "static jclass _mj2x_component_class(JNIEnv* env, const char* desc) {\n";
    s += "    if (desc[0] == '[') return (*env)->FindClass(env, desc);\n";
    s += "    if (desc[0] == 'L') {\n";
    s += "        char name[1024];\n";
    s += "        int i = 0;\n";
    s += "        while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = desc[i + 1]; i++; }\n";
    s += "        name[i] = '\\0';\n";
    s += "        return (*env)->FindClass(env, name);\n";
    s += "    }\n";
    s += "    return (*env)->FindClass(env, \"java/lang/Object\");\n";
    s += "}\n\n";

    s += "static jobject _mj2x_new_multi_array(JNIEnv* env, const char* desc, jint* dims, int dimCount) {\n";
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
    s += "        default: return (jobject)(*env)->NewObjectArray(env, len, _mj2x_component_class(env, component), NULL);\n";
    s += "        }\n";
    s += "    }\n";
    s += "    jobjectArray outer = (*env)->NewObjectArray(env, len, (*env)->FindClass(env, component), NULL);\n";
    s += "    for (jint i = 0; i < len; ++i) {\n";
    s += "        jobject child = _mj2x_new_multi_array(env, component, dims + 1, dimCount - 1);\n";
    s += "        (*env)->SetObjectArrayElement(env, outer, i, child);\n";
    s += "    }\n";
    s += "    return (jobject)outer;\n";
    s += "}\n\n";

    return s;
}

}  // namespace mj2x::backend

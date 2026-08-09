#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"

#include "mj2x/codegen/c_source.hpp"

#include <string>

namespace mj2x::backend {

std::string stackCRuntimePrelude()
{
    codegen::CSourceStream src;

    src.emitLine("#if defined(_MSC_VER)");
    src.emitLine("#include <intrin.h>");
    src.emitLine("#define _MJ2X_LIKELY(x)   (x)");
    src.emitLine("#define _MJ2X_UNLIKELY(x) (x)");
    src.emitLine("static __inline void* _mj2x_cache_load(void* volatile* slot) { return *slot; }");
    src.emitLine("static __inline void* _mj2x_cache_cas(void* volatile* slot, void* expected, "
                 "void* desired) {");
    src.emitLine("    return _InterlockedCompareExchangePointer(slot, desired, expected);");
    src.emitLine("}");
    src.emitLine("#else");
    src.emitLine("#define _MJ2X_LIKELY(x)   __builtin_expect((x), 1)");
    src.emitLine("#define _MJ2X_UNLIKELY(x) __builtin_expect((x), 0)");
    src.emitLine("static __inline void* _mj2x_cache_load(void* volatile* slot) {");
    src.emitLine("    return __atomic_load_n(slot, __ATOMIC_RELAXED);");
    src.emitLine("}");
    src.emitLine("static __inline void* _mj2x_cache_cas(void* volatile* slot, void* expected, "
                 "void* desired) {");
    src.emitLine("    void* previous = expected;");
    src.emitLine("    __atomic_compare_exchange_n(slot, &previous, desired, 0, __ATOMIC_RELAXED, "
                 "__ATOMIC_RELAXED);");
    src.emitLine("    return previous;");
    src.emitLine("}");
    src.emitLine("#endif");
    src.emitLine("");

    src.emitLine("static jclass _mj2x_find_class(JNIEnv* env, jclass owner, const char* name) {");
    src.indent();
    src.emitLine("if (name == NULL) return NULL;");
    src.emitLine("if (owner == NULL) return (*env)->FindClass(env, name);");
    src.emitLine("jclass class_class = (*env)->FindClass(env, \"java/lang/Class\");");
    src.emitLine("if ((*env)->ExceptionCheck(env)) return NULL;");
    src.emitLine("jmethodID get_loader = (*env)->GetMethodID(env, class_class, \"getClassLoader\", "
                 "\"()Ljava/lang/ClassLoader;\");");
    src.emitLine("if ((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, class_class); "
                 "return NULL; }");
    src.emitLine("jobject loader = (*env)->CallObjectMethod(env, owner, get_loader);");
    src.emitLine("if ((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, class_class); "
                 "return NULL; }");
    src.emitLine("if (loader == NULL) {");
    src.indent();
    src.emitLine("jclass result = (*env)->FindClass(env, name);");
    src.emitLine("(*env)->DeleteLocalRef(env, class_class);");
    src.emitLine("return result;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("char binary_name[1024];");
    src.emitLine("int i = 0;");
    src.emitLine("while (name[i] != '\\0' && i < 1023) { binary_name[i] = (name[i] == '/') ? '.' : "
                 "name[i]; i++; }");
    src.emitLine("binary_name[i] = '\\0';");
    src.emitLine("jstring class_name = (*env)->NewStringUTF(env, binary_name);");
    src.emitLine("if ((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, loader); "
                 "(*env)->DeleteLocalRef(env, class_class); return NULL; }");
    src.emitLine("jmethodID for_name = (*env)->GetStaticMethodID(env, class_class, \"forName\", "
                 "\"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;\");");
    src.emitLine("if ((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, class_name); "
                 "(*env)->DeleteLocalRef(env, loader); (*env)->DeleteLocalRef(env, class_class); "
                 "return NULL; }");
    src.emitLine("jclass result = (jclass)(*env)->CallStaticObjectMethod(env, class_class, "
                 "for_name, class_name, JNI_FALSE, loader);");
    src.emitLine("if ((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, class_name); "
                 "(*env)->DeleteLocalRef(env, loader); (*env)->DeleteLocalRef(env, class_class); "
                 "return NULL; }");
    src.emitLine("(*env)->DeleteLocalRef(env, class_name);");
    src.emitLine("(*env)->DeleteLocalRef(env, loader);");
    src.emitLine("(*env)->DeleteLocalRef(env, class_class);");
    src.emitLine("return result;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    src.emitLine(
        "static jclass _mj2x_component_class(JNIEnv* env, jclass owner, const char* desc) {");
    src.indent();
    src.emitLine("if (desc[0] == '[') return _mj2x_find_class(env, owner, desc);");
    src.emitLine("if (desc[0] == 'L') {");
    src.indent();
    src.emitLine("char name[1024];");
    src.emitLine("int i = 0;");
    src.emitLine("while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = "
                 "desc[i + 1]; i++; }");
    src.emitLine("name[i] = '\\0';");
    src.emitLine("return _mj2x_find_class(env, owner, name);");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return (*env)->FindClass(env, \"java/lang/Object\");");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    src.emitLine("static jobject _mj2x_new_multi_array(JNIEnv* env, jclass owner, const char* "
                 "desc, jint* dims, int dimCount) {");
    src.indent();
    src.emitLine("jint len = dims[0];");
    src.emitLine("const char* component = desc + 1;");
    src.emitLine("if (dimCount == 1) {");
    src.indent();
    src.emitLine("switch (component[0]) {");
    src.indent();
    src.emitLine("case 'Z': return (jobject)(*env)->NewBooleanArray(env, len);");
    src.emitLine("case 'B': return (jobject)(*env)->NewByteArray(env, len);");
    src.emitLine("case 'C': return (jobject)(*env)->NewCharArray(env, len);");
    src.emitLine("case 'S': return (jobject)(*env)->NewShortArray(env, len);");
    src.emitLine("case 'I': return (jobject)(*env)->NewIntArray(env, len);");
    src.emitLine("case 'J': return (jobject)(*env)->NewLongArray(env, len);");
    src.emitLine("case 'F': return (jobject)(*env)->NewFloatArray(env, len);");
    src.emitLine("case 'D': return (jobject)(*env)->NewDoubleArray(env, len);");
    src.emitLine("default: {");
    src.indent();
    src.emitLine("jclass component_class = _mj2x_component_class(env, owner, component);");
    src.emitLine(
        "jobject result = (jobject)(*env)->NewObjectArray(env, len, component_class, NULL);");
    src.emitLine("if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);");
    src.emitLine("return result;");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.emitLine("jclass component_class = _mj2x_find_class(env, owner, component);");
    src.emitLine("jobjectArray outer = (*env)->NewObjectArray(env, len, component_class, NULL);");
    src.emitLine("if (component_class != NULL) (*env)->DeleteLocalRef(env, component_class);");
    src.emitLine("for (jint i = 0; i < len; ++i) {");
    src.indent();
    src.emitLine(
        "jobject child = _mj2x_new_multi_array(env, owner, component, dims + 1, dimCount - 1);");
    src.emitLine("(*env)->SetObjectArrayElement(env, outer, i, child);");
    src.emitLine("if (child != NULL) (*env)->DeleteLocalRef(env, child);");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return (jobject)outer;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    return src.str();
}

}   // namespace mj2x::backend

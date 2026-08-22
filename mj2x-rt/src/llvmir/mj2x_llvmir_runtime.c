/// @file mj2x_llvmir_runtime.c
/// @brief LLVM IR 后端运行时实现(#894 mj2x-llvmrt 下沉,阶段一)。
///
/// 原 llvm_ir_helpers.cpp 的 `define internal` IR 体逐语义翻译为 C99
/// (IR 手搓 JNIEnv 槽位分发 ≙ (*env)->调用)。异常消息与控制流保持一致:
/// 抛出后由生成代码的 ExceptionCheck 面感知,本层不额外清理。

#include "llvmir/mj2x_llvmir_runtime.h"

void mj2x_llvm_throw_null(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/NullPointerException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "null");
        (*env)->DeleteLocalRef(env, _c);
    }
}

void mj2x_llvm_throw_arith(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/ArithmeticException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "/ by zero");
        (*env)->DeleteLocalRef(env, _c);
    }
}

jobject mj2x_llvm_exception_occurred(JNIEnv* env) { return (*env)->ExceptionOccurred(env); }

void mj2x_llvm_exception_clear(JNIEnv* env) { (*env)->ExceptionClear(env); }

// ---- 阶段二 ----

// FindClass + AllocObject(不调 <init>;构造器由 invokespecial 处理)。
void* mj2x_llvm_new_object(JNIEnv* env, const char* name)
{
    jclass cls = (*env)->FindClass(env, name);
    if (cls == NULL) return NULL;
    return (*env)->AllocObject(env, cls);
}

// CHECKCAST:失败抛 ClassCastException。原 IR 失败路径误把类名字符串常量
// 直接当 jclass 传给 ThrowNew(未执行到的潜在缺陷);此处按 JVM 语义修正。
void* mj2x_llvm_check_cast(JNIEnv* env, void* obj, const char* desc)
{
    jclass cls = (*env)->FindClass(env, desc);
    if (cls != NULL && !(*env)->IsInstanceOf(env, obj, cls)) {
        jclass cce = (*env)->FindClass(env, "java/lang/ClassCastException");
        if (cce != NULL) {
            (*env)->ThrowNew(env, cce, "null");
            (*env)->DeleteLocalRef(env, cce);
        }
    }
    if (cls != NULL) (*env)->DeleteLocalRef(env, cls);
    return obj;
}

int32_t mj2x_llvm_instance_of(JNIEnv* env, void* obj, const char* desc)
{
    jclass cls = (*env)->FindClass(env, desc);
    if (cls == NULL) return 0;
    const int32_t ok = (*env)->IsInstanceOf(env, obj, cls) ? 1 : 0;
    (*env)->DeleteLocalRef(env, cls);
    return ok;
}

int mj2x_llvm_exception_check(JNIEnv* env) { return (*env)->ExceptionCheck(env) ? 1 : 0; }

// 空串 = catch-all/finally,恒匹配(对齐 RegC 语义)。
int mj2x_llvm_exception_matches(JNIEnv* env, void* ex, const char* catch_type)
{
    if (catch_type == NULL || catch_type[0] == '\0') return 1;
    jclass cls = (*env)->FindClass(env, catch_type);
    if (cls == NULL) return 0;
    const int r = (*env)->IsInstanceOf(env, ex, cls) ? 1 : 0;
    (*env)->DeleteLocalRef(env, cls);
    return r;
}

void mj2x_llvm_throw(JNIEnv* env, void* obj)
{
    if (obj == NULL) {
        mj2x_llvm_throw_null(env);
        return;
    }
    (*env)->Throw(env, (jthrowable)obj);
}

void* mj2x_llvm_string_const(JNIEnv* env, const char* utf8) { return (*env)->NewStringUTF(env, utf8); }

// JNI local ref 是句柄:同一性必须走 IsSameObject(对齐 RegC regc_flow.cpp)。
int mj2x_llvm_ref_eq(JNIEnv* env, void* a, void* b)
{
    return (*env)->IsSameObject(env, a, b) ? 1 : 0;
}

// ---- 阶段三:数组族 ----

// 按元素描述符首字符分发:基本类型 → NewXxxArray;否则 FindClass + NewObjectArray。
void* mj2x_llvm_new_array(JNIEnv* env, const char* desc, int32_t count)
{
    switch (desc[1]) {
    case 'Z': return (*env)->NewBooleanArray(env, count);
    case 'B': return (*env)->NewByteArray(env, count);
    case 'C': return (*env)->NewCharArray(env, count);
    case 'S': return (*env)->NewShortArray(env, count);
    case 'I': return (*env)->NewIntArray(env, count);
    case 'J': return (*env)->NewLongArray(env, count);
    case 'F': return (*env)->NewFloatArray(env, count);
    case 'D': return (*env)->NewDoubleArray(env, count);
    default: break;
    }
    jclass cls = (*env)->FindClass(env, desc + 1);
    if (cls == NULL) return NULL;
    void* arr = (*env)->NewObjectArray(env, count, cls, NULL);
    (*env)->DeleteLocalRef(env, cls);
    return arr;
}

// NewObjectArray 递归(JVM multianewarray 语义):外层一维建好后逐元素递归
// 填充剩余维度。
void* mj2x_llvm_new_multi_array(JNIEnv* env, const char* desc, const int32_t* dims, int32_t ndims)
{
    const char* edesc = desc + 1;
    jclass cls = (*env)->FindClass(env, edesc);
    if (cls == NULL) return NULL;
    void* arr = (*env)->NewObjectArray(env, dims[0], cls, NULL);
    if (arr != NULL && ndims > 1) {
        for (int32_t i = 0; i < dims[0]; i++) {
            void* elem = mj2x_llvm_new_multi_array(env, edesc, dims + 1, ndims - 1);
            (*env)->SetObjectArrayElement(env, arr, i, elem);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    return arr;
}

void* mj2x_llvm_new_object_array(JNIEnv* env, int32_t count, void* cls)
{
    return (*env)->NewObjectArray(env, count, (jclass)cls, NULL);
}

void mj2x_llvm_set_object_array_element(JNIEnv* env, void* arr, int32_t idx, void* val)
{
    (*env)->SetObjectArrayElement(env, arr, idx, val);
}

int32_t mj2x_llvm_array_length(JNIEnv* env, void* arr)
{
    return (*env)->GetArrayLength(env, arr);
}

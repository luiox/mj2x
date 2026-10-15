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

// ---- 阶段三b:字段族(load/store ×9 kind;static/instance 合一) ----

// load:FindClass(owner) 后按 is_static 分派;实例路径 receiver 为 null 先
// 释放 cls 再抛 NPE 返零值。窄类型经 WIDEN 宽化(bool/char 零扩展,
// byte/short 符号扩展);cls 局部引用用毕释放。
#define MJ2X_FIELD_LOAD(kind, RetT, JRetT, JN, WIDEN)                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_field_load_##kind(JNIEnv* env, void* rcv,           \
        const char* owner, const char* name, const char* desc, int32_t is_static)       \
    {                                                                                   \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return (RetT)0;                                                \
        if (!is_static)                                                                 \
        {                                                                               \
            if (rcv == NULL)                                                            \
            {                                                                           \
                (*env)->DeleteLocalRef(env, cls);                                       \
                mj2x_llvm_throw_null(env);                                              \
                return (RetT)0;                                                         \
            }                                                                           \
            jfieldID fid = (*env)->GetFieldID(env, cls, name, desc);                    \
            const JRetT v = (*env)->Get##JN##Field(env, rcv, fid);                      \
            (*env)->DeleteLocalRef(env, cls);                                           \
            return (RetT)(WIDEN);                                                       \
        }                                                                               \
        jfieldID fid = (*env)->GetStaticFieldID(env, cls, name, desc);                  \
        const JRetT v = (*env)->GetStatic##JN##Field(env, cls, fid);                    \
        (*env)->DeleteLocalRef(env, cls);                                               \
        return (RetT)(WIDEN);                                                           \
    }

// store:同一 null 检查面;窄类型经 NARROW 窄化(bool 归一 0/1)后写入。
#define MJ2X_FIELD_STORE(kind, RetT, JRetT, JN, NARROW)                                 \
    MJ2X_LLVMIR_API void mj2x_llvm_field_store_##kind(JNIEnv* env, void* rcv,          \
        const char* owner, const char* name, const char* desc, int32_t is_static,       \
        RetT val)                                                                       \
    {                                                                                   \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return;                                                        \
        if (!is_static)                                                                 \
        {                                                                               \
            if (rcv == NULL)                                                            \
            {                                                                           \
                (*env)->DeleteLocalRef(env, cls);                                       \
                mj2x_llvm_throw_null(env);                                              \
                return;                                                                 \
            }                                                                           \
            jfieldID fid = (*env)->GetFieldID(env, cls, name, desc);                    \
            (*env)->Set##JN##Field(env, rcv, fid, (JRetT)(NARROW));                     \
            (*env)->DeleteLocalRef(env, cls);                                           \
            return;                                                                     \
        }                                                                               \
        jfieldID fid = (*env)->GetStaticFieldID(env, cls, name, desc);                  \
        (*env)->SetStatic##JN##Field(env, cls, fid, (JRetT)(NARROW));                   \
        (*env)->DeleteLocalRef(env, cls);                                               \
    }

MJ2X_FIELD_LOAD(bool, int32_t, jboolean, Boolean, (uint32_t)v)
MJ2X_FIELD_LOAD(byte, int32_t, jbyte, Byte, (int32_t)v)
MJ2X_FIELD_LOAD(char, int32_t, jchar, Char, (uint32_t)v)
MJ2X_FIELD_LOAD(short, int32_t, jshort, Short, (int32_t)v)
MJ2X_FIELD_LOAD(int, int32_t, jint, Int, v)
MJ2X_FIELD_LOAD(long, int64_t, jlong, Long, v)
MJ2X_FIELD_LOAD(float, float, jfloat, Float, v)
MJ2X_FIELD_LOAD(double, double, jdouble, Double, v)
MJ2X_FIELD_LOAD(ref, void*, jobject, Object, v)

MJ2X_FIELD_STORE(bool, int32_t, jboolean, Boolean, (val ? 1 : 0))
MJ2X_FIELD_STORE(byte, int32_t, jbyte, Byte, val)
MJ2X_FIELD_STORE(char, int32_t, jchar, Char, val)
MJ2X_FIELD_STORE(short, int32_t, jshort, Short, val)
MJ2X_FIELD_STORE(int, int32_t, jint, Int, val)
MJ2X_FIELD_STORE(long, int64_t, jlong, Long, val)
MJ2X_FIELD_STORE(float, float, jfloat, Float, val)
MJ2X_FIELD_STORE(double, double, jdouble, Double, val)
MJ2X_FIELD_STORE(ref, void*, jobject, Object, val)

// ---- 阶段三c:数组元素族(load/store ×8 kind) ----

// 通用原语形态:region 读/写 1 元素;窄类型读后宽化(WIDEN)/写前窄化
//(NARROW);数组为 null 先抛 NPE。越界由 JNI region 调用抛
// ArrayIndexOutOfBoundsException,存储违例抛 ArrayStoreException。
#define MJ2X_ARRAY_LOAD(kind, RetT, JRetT, JN, WIDEN)                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_array_load_##kind(JNIEnv* env, void* arr,           \
        int32_t idx)                                                                    \
    {                                                                                   \
        if (arr == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return (RetT)0;                                                             \
        }                                                                               \
        JRetT v = (JRetT)0;                                                             \
        (*env)->Get##JN##ArrayRegion(env, arr, idx, 1, &v);                             \
        return (RetT)(WIDEN);                                                           \
    }
#define MJ2X_ARRAY_STORE(kind, RetT, JRetT, JN, NARROW)                                 \
    MJ2X_LLVMIR_API void mj2x_llvm_array_store_##kind(JNIEnv* env, void* arr,          \
        int32_t idx, RetT val)                                                          \
    {                                                                                   \
        if (arr == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return;                                                                     \
        }                                                                               \
        const JRetT v = (JRetT)(NARROW);                                                \
        (*env)->Set##JN##ArrayRegion(env, arr, idx, 1, &v);                             \
    }

// byte:同一 baload/bastore 站点可能落在 byte[] 或 boolean[](描述符同为
// [B),按实际数组类型 IsInstanceOf "[Z" 分派;jboolean 读后零扩展、写前
// 归一 0/1。
MJ2X_LLVMIR_API int32_t mj2x_llvm_array_load_byte(JNIEnv* env, void* arr, int32_t idx)
{
    if (arr == NULL) {
        mj2x_llvm_throw_null(env);
        return 0;
    }
    jclass zcls = (*env)->FindClass(env, "[Z");
    if (zcls != NULL) {
        const jboolean isBool = (*env)->IsInstanceOf(env, arr, zcls);
        (*env)->DeleteLocalRef(env, zcls);
        if (isBool) {
            jboolean v = JNI_FALSE;
            (*env)->GetBooleanArrayRegion(env, arr, idx, 1, &v);
            return (int32_t)(uint32_t)v;
        }
    }
    jbyte v = 0;
    (*env)->GetByteArrayRegion(env, arr, idx, 1, &v);
    return (int32_t)v;
}

MJ2X_LLVMIR_API void mj2x_llvm_array_store_byte(JNIEnv* env, void* arr, int32_t idx, int32_t val)
{
    if (arr == NULL) {
        mj2x_llvm_throw_null(env);
        return;
    }
    jclass zcls = (*env)->FindClass(env, "[Z");
    if (zcls != NULL) {
        const jboolean isBool = (*env)->IsInstanceOf(env, arr, zcls);
        (*env)->DeleteLocalRef(env, zcls);
        if (isBool) {
            const jboolean b = (jboolean)(val ? 1 : 0);
            (*env)->SetBooleanArrayRegion(env, arr, idx, 1, &b);
            return;
        }
    }
    const jbyte v = (jbyte)val;
    (*env)->SetByteArrayRegion(env, arr, idx, 1, &v);
}

// ref:Get/SetObjectArrayElement 直传元素(无 region 缓冲;类型不符由 JVM
// 抛 ArrayStoreException)。
MJ2X_LLVMIR_API void* mj2x_llvm_array_load_ref(JNIEnv* env, void* arr, int32_t idx)
{
    if (arr == NULL) {
        mj2x_llvm_throw_null(env);
        return NULL;
    }
    return (*env)->GetObjectArrayElement(env, arr, idx);
}

MJ2X_LLVMIR_API void mj2x_llvm_array_store_ref(JNIEnv* env, void* arr, int32_t idx, void* val)
{
    if (arr == NULL) {
        mj2x_llvm_throw_null(env);
        return;
    }
    (*env)->SetObjectArrayElement(env, arr, idx, val);
}

MJ2X_ARRAY_LOAD(short, int32_t, jshort, Short, (int32_t)v)
MJ2X_ARRAY_LOAD(char, int32_t, jchar, Char, (uint32_t)v)
MJ2X_ARRAY_LOAD(int, int32_t, jint, Int, v)
MJ2X_ARRAY_LOAD(long, int64_t, jlong, Long, v)
MJ2X_ARRAY_LOAD(float, float, jfloat, Float, v)
MJ2X_ARRAY_LOAD(double, double, jdouble, Double, v)

MJ2X_ARRAY_STORE(short, int32_t, jshort, Short, val)
MJ2X_ARRAY_STORE(char, int32_t, jchar, Char, val)
MJ2X_ARRAY_STORE(int, int32_t, jint, Int, val)
MJ2X_ARRAY_STORE(long, int64_t, jlong, Long, val)
MJ2X_ARRAY_STORE(float, float, jfloat, Float, val)
MJ2X_ARRAY_STORE(double, double, jdouble, Double, val)


// ---- 阶段三d:方法调用族(virtual/nonvirtual/static ×10 kind) ----

// virtual/interface:receiver 为 null 先抛 NPE 返零值;FindClass(owner)+
// GetMethodID+Call<JNI>MethodA。nonvirtual(invokespecial:<init>/private/
// super):解析类取 receiver 实际类(GetObjectClass(rcv))而非 owner,
// CallNonvirtual<JNI>MethodA。static 外部:FindClass+GetStaticMethodID+
// CallStatic<JNI>MethodA(无 receiver 面)。cls/rcls 局部引用用毕释放;
// 方法抛出的异常由生成代码的 ExceptionCheck 面感知。
#define MJ2X_CALL(kind, RetT, JRetT, JN)                                                \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_##kind(JNIEnv* env, void* rcv,                 \
        const char* owner, const char* name, const char* desc, jvalue* jv)              \
    {                                                                                   \
        if (rcv == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return (RetT)0;                                                             \
        }                                                                               \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return (RetT)0;                                                \
        jmethodID mid = (*env)->GetMethodID(env, cls, name, desc);                      \
        const JRetT r = (*env)->Call##JN##MethodA(env, rcv, mid, jv);                   \
        (*env)->DeleteLocalRef(env, cls);                                               \
        return (RetT)r;                                                                 \
    }                                                                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_nonvirtual_##kind(JNIEnv* env, void* rcv,      \
        const char* owner, const char* name, const char* desc, jvalue* jv)              \
    {                                                                                   \
        (void)owner;                                                                    \
        if (rcv == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return (RetT)0;                                                             \
        }                                                                               \
        jclass rcls = (*env)->GetObjectClass(env, rcv);                                 \
        jmethodID mid = (*env)->GetMethodID(env, rcls, name, desc);                     \
        const JRetT r = (*env)->CallNonvirtual##JN##MethodA(env, rcv, rcls, mid, jv);   \
        (*env)->DeleteLocalRef(env, rcls);                                              \
        return (RetT)r;                                                                 \
    }                                                                                   \
    MJ2X_LLVMIR_API RetT mj2x_llvm_call_static_##kind(JNIEnv* env, const char* owner,  \
        const char* name, const char* desc, jvalue* jv)                                 \
    {                                                                                   \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return (RetT)0;                                                \
        jmethodID mid = (*env)->GetStaticMethodID(env, cls, name, desc);                \
        const JRetT r = (*env)->CallStatic##JN##MethodA(env, cls, mid, jv);             \
        (*env)->DeleteLocalRef(env, cls);                                               \
        return (RetT)r;                                                                 \
    }

// void 返回形态(零值返回路径改为直接返回)。
#define MJ2X_CALL_VOID(kind, JN)                                                        \
    MJ2X_LLVMIR_API void mj2x_llvm_call_##kind(JNIEnv* env, void* rcv,                 \
        const char* owner, const char* name, const char* desc, jvalue* jv)              \
    {                                                                                   \
        if (rcv == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return;                                                                     \
        }                                                                               \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return;                                                        \
        jmethodID mid = (*env)->GetMethodID(env, cls, name, desc);                      \
        (*env)->Call##JN##MethodA(env, rcv, mid, jv);                                   \
        (*env)->DeleteLocalRef(env, cls);                                               \
    }                                                                                   \
    MJ2X_LLVMIR_API void mj2x_llvm_call_nonvirtual_##kind(JNIEnv* env, void* rcv,      \
        const char* owner, const char* name, const char* desc, jvalue* jv)              \
    {                                                                                   \
        (void)owner;                                                                    \
        if (rcv == NULL)                                                                \
        {                                                                               \
            mj2x_llvm_throw_null(env);                                                  \
            return;                                                                     \
        }                                                                               \
        jclass rcls = (*env)->GetObjectClass(env, rcv);                                 \
        jmethodID mid = (*env)->GetMethodID(env, rcls, name, desc);                     \
        (*env)->CallNonvirtual##JN##MethodA(env, rcv, rcls, mid, jv);                   \
        (*env)->DeleteLocalRef(env, rcls);                                              \
    }                                                                                   \
    MJ2X_LLVMIR_API void mj2x_llvm_call_static_##kind(JNIEnv* env, const char* owner,  \
        const char* name, const char* desc, jvalue* jv)                                 \
    {                                                                                   \
        jclass cls = (*env)->FindClass(env, owner);                                     \
        if (cls == NULL) return;                                                        \
        jmethodID mid = (*env)->GetStaticMethodID(env, cls, name, desc);                \
        (*env)->CallStatic##JN##MethodA(env, cls, mid, jv);                             \
        (*env)->DeleteLocalRef(env, cls);                                               \
    }

MJ2X_CALL(object, void*, jobject, Object)
MJ2X_CALL(bool, int8_t, jboolean, Boolean)
MJ2X_CALL(byte, int8_t, jbyte, Byte)
MJ2X_CALL(char, int16_t, jchar, Char)
MJ2X_CALL(short, int16_t, jshort, Short)
MJ2X_CALL(int, int32_t, jint, Int)
MJ2X_CALL(long, int64_t, jlong, Long)
MJ2X_CALL(float, float, jfloat, Float)
MJ2X_CALL(double, double, jdouble, Double)
MJ2X_CALL_VOID(void, Void)


// ---- 阶段四a:class_const + throw_new ----

// 基本类型经包装类 TYPE 静态字段取 primitive Class 对象(FindClass("I")
// 会抛 ClassNotFoundException,必须走 Integer.TYPE 形态)。
static void* mj2x_prim_class_type(JNIEnv* env, const char* wrapper)
{
    jclass cls = (*env)->FindClass(env, wrapper);
    if (cls == NULL) return NULL;
    jfieldID fid = (*env)->GetStaticFieldID(env, cls, "TYPE", "Ljava/lang/Class;");
    void* type = (fid != NULL) ? (*env)->GetStaticObjectField(env, cls, fid) : NULL;
    (*env)->DeleteLocalRef(env, cls);
    return type;
}

// 类常量:单字符名 V/Z/B/C/S/I/J/F/D → Void.class / 包装类 TYPE(ldc
// class I 等 javac 形态);其余按名 FindClass。与原 IR switch 语义一致
//(switch default 也落 plain 路径)。
void* mj2x_llvm_class_const(JNIEnv* env, const char* name)
{
    if (name[0] != '\0' && name[1] == '\0')
    {
        switch (name[0]) {
        case 'V': return (*env)->FindClass(env, "java/lang/Void");
        case 'Z': return mj2x_prim_class_type(env, "java/lang/Boolean");
        case 'B': return mj2x_prim_class_type(env, "java/lang/Byte");
        case 'C': return mj2x_prim_class_type(env, "java/lang/Character");
        case 'S': return mj2x_prim_class_type(env, "java/lang/Short");
        case 'I': return mj2x_prim_class_type(env, "java/lang/Integer");
        case 'J': return mj2x_prim_class_type(env, "java/lang/Long");
        case 'F': return mj2x_prim_class_type(env, "java/lang/Float");
        case 'D': return mj2x_prim_class_type(env, "java/lang/Double");
        default: break;
        }
    }
    return (*env)->FindClass(env, name);
}

// 按类名抛新异常(class_const + ThrowNew):发射端预检类语义陷阱用
//(如 Random.nextInt(bound<=0) 的 IllegalArgumentException)。类解析
// 失败时保留挂起异常直接返回。
void mj2x_llvm_throw_new(JNIEnv* env, const char* cls_name, const char* msg)
{
    jclass cls = (jclass)mj2x_llvm_class_const(env, cls_name);
    if (cls == NULL) return;
    (*env)->ThrowNew(env, cls, msg);
    (*env)->DeleteLocalRef(env, cls);
}


// ---- 阶段四b:monitor ----

// monitorenter/monitorexit:null → NPE(经 mj2x_llvm_throw_null,与原 IR
// 的 FindClass+ThrowNew("null") 同语义);MonitorExit 失败(非持有者
// IMSE 等)由 JNI 挂异常,生成代码的 ExceptionCheck 面感知。
void mj2x_llvm_monitor_enter(JNIEnv* env, void* obj)
{
    if (obj == NULL) {
        mj2x_llvm_throw_null(env);
        return;
    }
    (*env)->MonitorEnter(env, obj);
}

void mj2x_llvm_monitor_exit(JNIEnv* env, void* obj)
{
    if (obj == NULL) {
        mj2x_llvm_throw_null(env);
        return;
    }
    (*env)->MonitorExit(env, obj);
}


// ---- 阶段四c:indy 直调面 + 装箱/拆箱 ----

// cls+mid 已解析的直调(CallStaticObjectMethodA / CallObjectMethodA):
// indy 解析链与装箱共用的最小调用面。
void* mj2x_llvm_indy_call_static_mid(JNIEnv* env, void* cls, void* mid, jvalue* jv)
{
    return (*env)->CallStaticObjectMethodA(env, (jclass)cls, (jmethodID)mid, jv);
}

void* mj2x_llvm_indy_call_obj_mid(JNIEnv* env, void* obj, void* cls, void* mid, jvalue* jv)
{
    return (*env)->CallObjectMethodA(env, obj, (jmethodID)mid, jv);
}

// valueOf 公共体:FindClass + GetStaticMethodID + 单元素 jvalue 经
// call_static_mid;类/方法解析失败返 NULL(挂起异常由调用侧感知),
// 包装类局部引用用毕释放。
static void* mj2x_box_valueof(JNIEnv* env, const char* wrapper,
                              const char* valueOfDesc, jvalue* jv)
{
    jclass cls = (*env)->FindClass(env, wrapper);
    if (cls == NULL) return NULL;
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "valueOf", valueOfDesc);
    void* boxed = (mid != NULL) ? mj2x_llvm_indy_call_static_mid(env, cls, mid, jv) : NULL;
    (*env)->DeleteLocalRef(env, cls);
    return boxed;
}

// 装箱:窄类型按包装类 valueOf 形参窄化入槽(boolean 归一 0/1,对齐原
// IR trunc-to-i1 再 zext),宽类型原样。
#define MJ2X_BOX(kind, InT, JField, wrapper, voDesc, NARROW)                            \
    MJ2X_LLVMIR_API void* mj2x_llvm_box_##kind(JNIEnv* env, InT v)                     \
    {                                                                                   \
        jvalue jv[1];                                                                   \
        jv[0].JField = (NARROW);                                                        \
        return mj2x_box_valueof(env, wrapper, voDesc, jv);                              \
    }

MJ2X_BOX(boolean, int32_t, z, "java/lang/Boolean", "(Z)Ljava/lang/Boolean;", (v ? 1 : 0))
MJ2X_BOX(byte, int32_t, b, "java/lang/Byte", "(B)Ljava/lang/Byte;", (jbyte)v)
MJ2X_BOX(char, int32_t, c, "java/lang/Character", "(C)Ljava/lang/Character;", (jchar)v)
MJ2X_BOX(short, int32_t, s, "java/lang/Short", "(S)Ljava/lang/Short;", (jshort)v)
MJ2X_BOX(int, int32_t, i, "java/lang/Integer", "(I)Ljava/lang/Integer;", v)
MJ2X_BOX(long, int64_t, j, "java/lang/Long", "(J)Ljava/lang/Long;", v)
MJ2X_BOX(float, float, f, "java/lang/Float", "(F)Ljava/lang/Float;", v)
MJ2X_BOX(double, double, d, "java/lang/Double", "(D)Ljava/lang/Double;", v)

// 拆箱:XxxValue() 无参 Call<JNI>MethodA;boolean/char 零扩展回计算位
// 宽,byte/short 符号扩展,int 原位,long/float/double 按位宽返回。
#define MJ2X_UNBOX(kind, RetT, JRetT, JN, wrapper, method, desc, WIDEN)                 \
    MJ2X_LLVMIR_API RetT mj2x_llvm_unbox_##kind(JNIEnv* env, void* obj)                \
    {                                                                                   \
        jclass cls = (*env)->FindClass(env, wrapper);                                   \
        if (cls == NULL) return (RetT)0;                                                \
        jmethodID mid = (*env)->GetMethodID(env, cls, method, desc);                    \
        if (mid == NULL)                                                                \
        {                                                                               \
            (*env)->DeleteLocalRef(env, cls);                                           \
            return (RetT)0;                                                             \
        }                                                                               \
        const JRetT v = (*env)->Call##JN##MethodA(env, obj, mid, NULL);                 \
        (*env)->DeleteLocalRef(env, cls);                                               \
        return (RetT)(WIDEN);                                                           \
    }

MJ2X_UNBOX(boolean, int32_t, jboolean, Boolean, "java/lang/Boolean", "booleanValue", "()Z",
           (uint32_t)v)
MJ2X_UNBOX(byte, int32_t, jbyte, Byte, "java/lang/Byte", "byteValue", "()B", (int32_t)v)
MJ2X_UNBOX(char, int32_t, jchar, Char, "java/lang/Character", "charValue", "()C", (uint32_t)v)
MJ2X_UNBOX(short, int32_t, jshort, Short, "java/lang/Short", "shortValue", "()S", (int32_t)v)
MJ2X_UNBOX(int, int32_t, jint, Int, "java/lang/Integer", "intValue", "()I", v)
MJ2X_UNBOX(long, int64_t, jlong, Long, "java/lang/Long", "longValue", "()J", v)
MJ2X_UNBOX(float, float, jfloat, Float, "java/lang/Float", "floatValue", "()F", v)
MJ2X_UNBOX(double, double, jdouble, Double, "java/lang/Double", "doubleValue", "()D", v)


// ---- 阶段四d:indy 固定解析 helper 组 ----

// MethodType.fromMethodDescriptorString(desc, owner.getClassLoader()):
// loader 经 call_object 直调取得;desc 物化为 String 后两参静态调用。
void* mj2x_llvm_indy_method_type(JNIEnv* env, void* owner, const char* desc)
{
    void* loader = mj2x_llvm_call_object(env, owner, "java/lang/Class", "getClassLoader",
                                         "()Ljava/lang/ClassLoader;", NULL);
    void* descstr = mj2x_llvm_string_const(env, desc);
    jclass mtcls = (*env)->FindClass(env, "java/lang/invoke/MethodType");
    if (mtcls == NULL) return NULL;
    jmethodID mid = (*env)->GetStaticMethodID(env, mtcls, "fromMethodDescriptorString",
        "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;");
    void* mt = NULL;
    if (mid != NULL)
    {
        jvalue jv[2];
        jv[0].l = (jobject)descstr;
        jv[1].l = (jobject)loader;
        mt = mj2x_llvm_indy_call_static_mid(env, mtcls, mid, jv);
    }
    (*env)->DeleteLocalRef(env, mtcls);
    return mt;
}

// 包装类 TYPE 静态字段 → 基本类型 Class(与阶段四a prim 形态同体)。
void* mj2x_llvm_indy_wrapper_type(JNIEnv* env, const char* name)
{
    return mj2x_prim_class_type(env, name);
}

// Class 对象:首字符 V/Z/B/C/S/I/J/F/D → Void.TYPE / 包装类 TYPE(原 IR
// 仅按首字符 switch),其余 FindClass。
void* mj2x_llvm_indy_class_object(JNIEnv* env, const char* desc)
{
    switch (desc[0]) {
    case 'V': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Void");
    case 'Z': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Boolean");
    case 'B': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Byte");
    case 'C': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Character");
    case 'S': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Short");
    case 'I': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Integer");
    case 'J': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Long");
    case 'F': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Float");
    case 'D': return mj2x_llvm_indy_wrapper_type(env, "java/lang/Double");
    default: return mj2x_llvm_class_const(env, desc);
    }
}

// 字段型 Handle:find[Static][Getter|Setter](owner_cls, String(name),
// ftype);ftype 经 class_object(desc),find* 方法在 Lookup 类上解析。
#define MJ2X_HANDLE_JV(n)                                                               \
    jmethodID mid = (*env)->GetMethodID(env, (jclass)lkcls, findName, findDesc);        \
    if (mid == NULL) return NULL;                                                       \
    jvalue jv[n]

void* mj2x_llvm_indy_field_handle(JNIEnv* env, void* lkcls, void* lookup, void* ocls,
                                  const char* name, const char* desc,
                                  const char* findName, const char* findDesc)
{
    void* ftype = mj2x_llvm_indy_class_object(env, desc);
    void* namestr = mj2x_llvm_string_const(env, name);
    MJ2X_HANDLE_JV(3);
    jv[0].l = (jobject)ocls;
    jv[1].l = (jobject)namestr;
    jv[2].l = (jobject)ftype;
    return mj2x_llvm_indy_call_obj_mid(env, lookup, lkcls, mid, jv);
}

// 方法型三参:findVirtual/findStatic(Class, String, MethodType)。
void* mj2x_llvm_indy_method_handle3(JNIEnv* env, void* lkcls, void* lookup, void* ocls,
                                    const char* name, void* type,
                                    const char* findName, const char* findDesc)
{
    void* namestr = mj2x_llvm_string_const(env, name);
    MJ2X_HANDLE_JV(3);
    jv[0].l = (jobject)ocls;
    jv[1].l = (jobject)namestr;
    jv[2].l = (jobject)type;
    return mj2x_llvm_indy_call_obj_mid(env, lookup, lkcls, mid, jv);
}

// 方法型四参:findSpecial(Class, String, MethodType, Class);special_cls
// 为运行时 owner 类(非 receiver 类)。
void* mj2x_llvm_indy_method_handle4(JNIEnv* env, void* lkcls, void* lookup, void* ocls,
                                    const char* name, void* type, void* special_cls,
                                    const char* findName, const char* findDesc)
{
    void* namestr = mj2x_llvm_string_const(env, name);
    MJ2X_HANDLE_JV(4);
    jv[0].l = (jobject)ocls;
    jv[1].l = (jobject)namestr;
    jv[2].l = (jobject)type;
    jv[3].l = (jobject)special_cls;
    return mj2x_llvm_indy_call_obj_mid(env, lookup, lkcls, mid, jv);
}

// 方法型二参:findConstructor(Class, MethodType)(无 name 形态)。
void* mj2x_llvm_indy_method_handle2(JNIEnv* env, void* lkcls, void* lookup, void* ocls,
                                    void* type, const char* findName, const char* findDesc)
{
    MJ2X_HANDLE_JV(2);
    jv[0].l = (jobject)ocls;
    jv[1].l = (jobject)type;
    return mj2x_llvm_indy_call_obj_mid(env, lookup, lkcls, mid, jv);
}

#undef MJ2X_HANDLE_JV

// MethodHandle 常量按 tag 分派到 Lookup.find*(JVM H_* 常量:1-4 字段型,
// 5/6/9 findVirtual/findStatic/findVirtual 三参,7 findSpecial 四参,
// 8 findConstructor 二参);未知 tag 返 null(对齐原 IR fail 块)。
// lkcls/ocls 局部引用用毕释放。
void* mj2x_llvm_indy_handle(JNIEnv* env, void* lookup, void* owner_class, int32_t tag,
                            const char* owner, const char* name, const char* desc)
{
    static const char* const kFieldFind =
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
        "Ljava/lang/invoke/MethodHandle;";
    static const char* const kMethod3Find =
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
        "Ljava/lang/invoke/MethodHandle;";
    static const char* const kSpecialFind =
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
        "Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
    static const char* const kCtorFind =
        "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
        "Ljava/lang/invoke/MethodHandle;";
    jclass lkcls = (*env)->FindClass(env, "java/lang/invoke/MethodHandles$Lookup");
    if (lkcls == NULL) return NULL;
    jclass ocls = (*env)->FindClass(env, owner);
    void* result = NULL;
    switch (tag) {
    case 1:
        result = mj2x_llvm_indy_field_handle(env, lkcls, lookup, ocls, name, desc,
                                             "findGetter", kFieldFind);
        break;
    case 2:
        result = mj2x_llvm_indy_field_handle(env, lkcls, lookup, ocls, name, desc,
                                             "findStaticGetter", kFieldFind);
        break;
    case 3:
        result = mj2x_llvm_indy_field_handle(env, lkcls, lookup, ocls, name, desc,
                                             "findSetter", kFieldFind);
        break;
    case 4:
        result = mj2x_llvm_indy_field_handle(env, lkcls, lookup, ocls, name, desc,
                                             "findStaticSetter", kFieldFind);
        break;
    case 5:
    case 6:
    case 9:
    {
        const char* fn = (tag == 6) ? "findStatic" : "findVirtual";
        void* type = mj2x_llvm_indy_method_type(env, owner_class, desc);
        result = mj2x_llvm_indy_method_handle3(env, lkcls, lookup, ocls, name, type, fn,
                                               kMethod3Find);
        break;
    }
    case 7:
    {
        void* type = mj2x_llvm_indy_method_type(env, owner_class, desc);
        result = mj2x_llvm_indy_method_handle4(env, lkcls, lookup, ocls, name, type,
                                               owner_class, "findSpecial", kSpecialFind);
        break;
    }
    case 8:
    {
        void* type = mj2x_llvm_indy_method_type(env, owner_class, desc);
        result = mj2x_llvm_indy_method_handle2(env, lkcls, lookup, ocls, type,
                                               "findConstructor", kCtorFind);
        break;
    }
    default:
        break;
    }
    (*env)->DeleteLocalRef(env, lkcls);
    if (ocls != NULL) (*env)->DeleteLocalRef(env, ocls);
    return result;
}


// ---- 阶段五:立即数隐藏 per-site 密钥派生 ----

// LTO 不可折叠不透明常量：volatile 读迫使编译器每次真实加载——cross-TU
// IPSCCP/常量传播无法把 const_key(site) 折成常量（否则 E ^ K 被折回 C，
// 立即数隐藏白做；--native-opt-level 的 LTO 档同样守得住）。
// 值与 RegIR pass（reg_transform_passes.cpp kOpaqueConstant）同源，改必须同步。
static volatile uint64_t g_const_key_opaque = 0x517CC1B727220A95ull;

// splitmix64 单步（黄金比例增量后四步混合），输入先掺不透明常量。与
// reg_transform_passes.cpp 的 splitmix64（ca::random::SplitMix64 正典，
// #1027 A3 逐位锁定）+ kOpaqueConstant 同源——改常数/移位/顺序即使
// E ^ K != C，两端必须同步改。site 为函数内局部计数器；不掺 seed 的理由见
// mj2x_llvmir_runtime.h 契约注释。
int64_t mj2x_llvm_const_key(int64_t site)
{
    const uint64_t x     = (uint64_t)site ^ g_const_key_opaque;
    uint64_t       state = x + 0x9E3779B97F4A7C15ull;
    uint64_t       z     = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (int64_t)(z ^ (z >> 31));
}

// ---- 阶段六:调用目标间接化 per-signature dispatcher ----

// 按 (table, index) 解析目标方法并缓存入条目；返回 NULL 表示解析失败
// （pending exception 由生成代码的 ExceptionCheck 面感知）。
static void* resolveLlvmCallSite(JNIEnv* env, mj2x_llvm_call_site* site, void* rcv)
{
    jclass cls;
    jclass target;
    if (site->method != NULL) return site->method;
    cls = (*env)->FindClass(env, site->owner);
    if (cls == NULL) return NULL;
    target = cls;
    if (site->invokeKind == 2 && rcv != NULL) {
        // invokespecial：解析类取 receiver 实际类（与 RegC cached 帮手对齐）。
        target = (*env)->GetObjectClass(env, rcv);
        if (target == NULL) {
            (*env)->DeleteLocalRef(env, cls);
            return NULL;
        }
    }
    if (site->invokeKind == 1) {
        site->ownerClass = target;
        site->method     = (void*)(*env)->GetStaticMethodID(env, target, site->name, site->desc);
        return site->method;
    }
    if (rcv == NULL) {
        (*env)->DeleteLocalRef(env, cls);
        mj2x_llvm_throw_null(env);
        return NULL;
    }
    site->ownerClass = target;
    site->method     = (void*)(*env)->GetMethodID(env, target, site->name, site->desc);
    return site->method;
}

// per-signature dispatcher：解析后按返回类型分派一次 JNI 调用。
// static/virtual 由条目 invokeKind 决定（special 走虚调用面：GetObjectClass
// 已把解析类定为 receiver 实际类，与 RegC cached 同语义）。
#define MJ2X_LLVM_DISPATCH_VALUE(name, RetT, JN)                                        \
    MJ2X_LLVMIR_API RetT mj2x_llvm_dispatch_##name(JNIEnv* env,                       \
                                                  mj2x_llvm_call_site* table,           \
                                                  int32_t index, void* rcv, jvalue* jv)    \
    {                                                                                     \
        mj2x_llvm_call_site* site = &table[index];                                      \
        void*                 m   = resolveLlvmCallSite(env, site, rcv);                  \
        if (m == NULL) return (RetT)0;                                                    \
        if (site->invokeKind == 1)                                                        \
            return (RetT)(*env)->CallStatic##JN##MethodA(env, (jclass)site->ownerClass,     \
                                                     (jmethodID)m, jv);                  \
        return (RetT)(*env)->Call##JN##MethodA(env, (jobject)rcv, (jmethodID)m, jv);       \
    }

#define MJ2X_LLVM_DISPATCH_VOID()                                                        \
    MJ2X_LLVMIR_API void mj2x_llvm_dispatch_void(JNIEnv* env,                        \
                                                  mj2x_llvm_call_site* table,           \
                                                  int32_t index, void* rcv, jvalue* jv)   \
    {                                                                                     \
        mj2x_llvm_call_site* site = &table[index];                                      \
        void*                 m   = resolveLlvmCallSite(env, site, rcv);                  \
        if (m == NULL) return;                                                            \
        if (site->invokeKind == 1)                                                        \
            (*env)->CallStaticVoidMethodA(env, (jclass)site->ownerClass,                   \
                                             (jmethodID)m, jv);                         \
        else                                                                              \
            (*env)->CallVoidMethodA(env, (jobject)rcv, (jmethodID)m, jv);                 \
    }

MJ2X_LLVM_DISPATCH_VOID()
MJ2X_LLVM_DISPATCH_VALUE(i32, int32_t, Int)
MJ2X_LLVM_DISPATCH_VALUE(i64, int64_t, Long)
MJ2X_LLVM_DISPATCH_VALUE(f32, float, Float)
MJ2X_LLVM_DISPATCH_VALUE(f64, double, Double)
MJ2X_LLVM_DISPATCH_VALUE(ref, void*, Object)

#undef MJ2X_LLVM_DISPATCH_VOID
#undef MJ2X_LLVM_DISPATCH_VALUE

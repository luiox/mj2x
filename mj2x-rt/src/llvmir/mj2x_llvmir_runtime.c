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

#include "regc_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// JNI mangling is shared with StackC via codegen::; alias the symbols so
// existing call sites in this file stay one-line.
using codegen::CSourceStream;
using codegen::mangleMethod;
using codegen::mangleName;

const char* jniType(PrimitiveKind kind) {
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "jboolean";
    case PrimitiveKind::BYTE:    return "jbyte";
    case PrimitiveKind::CHAR:    return "jchar";
    case PrimitiveKind::SHORT:   return "jshort";
    case PrimitiveKind::INT:     return "jint";
    case PrimitiveKind::LONG:    return "jlong";
    case PrimitiveKind::FLOAT:   return "jfloat";
    case PrimitiveKind::DOUBLE:  return "jdouble";
    case PrimitiveKind::VOID:    return "void";
    default:                     return "jobject";
    }
}

const char* regcType(PrimitiveKind kind) {
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "bool";
    case PrimitiveKind::BYTE:    return "int8_t";
    case PrimitiveKind::CHAR:    return "uint16_t";
    case PrimitiveKind::SHORT:   return "int16_t";
    case PrimitiveKind::INT:     return "int32_t";
    case PrimitiveKind::LONG:    return "int64_t";
    case PrimitiveKind::FLOAT:   return "float";
    case PrimitiveKind::DOUBLE:  return "double";
    case PrimitiveKind::VOID:    return "void";
    default:                     return "mj2x_ref";
    }
}

const char* defaultReturnValue(PrimitiveKind kind) {
    switch (kind) {
    case PrimitiveKind::VOID:   return "";
    case PrimitiveKind::FLOAT:  return "0.0f";
    case PrimitiveKind::DOUBLE: return "0.0";
    case PrimitiveKind::OBJECT: return "NULL";
    default:                    return "0";
    }
}

PrimitiveKind returnKind(const std::string& desc) {
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str()))
            .getDescriptor()
            .to_std_string());
}

std::vector<PrimitiveKind> argumentKinds(const std::string& desc) {
    std::vector<PrimitiveKind> kinds;
    auto methodType = Type::getMethodType(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    for (const auto& argType : methodType.getArgumentTypes()) {
        kinds.push_back(jvmSortToPrimitive(argType.getSort()));
    }
    return kinds;
}

std::string emitFailureBody(const std::string& reason, PrimitiveKind retKind) {
    std::ostringstream out;
    out << "    /* RegC lowering failed: " << reason << " */\n";
    if (retKind == PrimitiveKind::VOID) {
        out << "    return;\n";
    } else {
        out << "    return " << defaultReturnValue(retKind) << ";\n";
    }
    return out.str();
}

std::string emitJniWrapper(const std::string& className,
                           const MethodNode& method,
                           const RegCEmitResult& emitted) {
    const std::string methodName = method.name.to_std_string();
    const std::string methodDesc = method.desc.to_std_string();
    const bool isStatic = (method.access & Opcodes::ACC_STATIC) != 0;
    const auto retKind = returnKind(methodDesc);
    const auto args = argumentKinds(methodDesc);

    std::ostringstream out;
    out << "JNIEXPORT " << jniType(retKind) << " JNICALL\n";
    out << mangleMethod(className, methodName, methodDesc) << "(JNIEnv* env, "
        << (isStatic ? "jclass clazz" : "jobject thisObject");
    for (size_t i = 0; i < args.size(); ++i) {
        out << ", " << jniType(args[i]) << " p" << i;
    }
    out << ") {\n";
    out << "    (void)" << (isStatic ? "clazz" : "thisObject") << ";\n";
    out << "    _mj2x_regc_env = env;\n";
    out << "    mj2x_regc_init_context(&_mj2x_regc_ctx, env, _mj2x_regc_throw_runtime);\n";

    if (!emitted.ok) {
        out << emitFailureBody(emitted.reason, retKind);
        out << "}\n\n";
        return out.str();
    }

    out << "    ";
    if (retKind != PrimitiveKind::VOID) out << "return (" << jniType(retKind) << ")";
    out << emitted.functionName << "(";
    bool first = true;
    auto appendArg = [&](const std::string& expr, const char* type) {
        if (!first) out << ", ";
        first = false;
        out << "(" << type << ")(" << expr << ")";
    };
    if (!isStatic) appendArg("thisObject", "mj2x_ref");
    for (size_t i = 0; i < args.size(); ++i) {
        appendArg("p" + std::to_string(i), regcType(args[i]));
    }
    out << ");\n";
    if (retKind == PrimitiveKind::VOID) out << "    return;\n";
    out << "}\n\n";
    return out.str();
}

void emitRegCFieldAccessHelpers(std::ostringstream& out) {
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
static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, bool value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticBooleanField(_mj2x_regc_env, c, f, value ? JNI_TRUE : JNI_FALSE); else (*_mj2x_regc_env)->SetBooleanField(_mj2x_regc_env, (jobject)receiver, f, value ? JNI_TRUE : JNI_FALSE); }
static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int8_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticByteField(_mj2x_regc_env, c, f, (jbyte)value); else (*_mj2x_regc_env)->SetByteField(_mj2x_regc_env, (jobject)receiver, f, (jbyte)value); }
static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, uint16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticCharField(_mj2x_regc_env, c, f, (jchar)value); else (*_mj2x_regc_env)->SetCharField(_mj2x_regc_env, (jobject)receiver, f, (jchar)value); }
static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int16_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticShortField(_mj2x_regc_env, c, f, (jshort)value); else (*_mj2x_regc_env)->SetShortField(_mj2x_regc_env, (jobject)receiver, f, (jshort)value); }
static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int32_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticIntField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetIntField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int64_t value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticLongField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetLongField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, float value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticFloatField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetFloatField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, double value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticDoubleField(_mj2x_regc_env, c, f, value); else (*_mj2x_regc_env)->SetDoubleField(_mj2x_regc_env, (jobject)receiver, f, value); }
static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, mj2x_ref value) { jclass c = _mj2x_regc_field_class(is_static, receiver, owner); jfieldID f = _mj2x_regc_field_id(is_static, c, name, desc); if (f == NULL) return; if (is_static) (*_mj2x_regc_env)->SetStaticObjectField(_mj2x_regc_env, c, f, (jobject)value); else (*_mj2x_regc_env)->SetObjectField(_mj2x_regc_env, (jobject)receiver, f, (jobject)value); }

)mj2x");
    out << src.str();
}

void emitRegCMethodCallHelpers(std::ostringstream& out) {
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

)mj2x");
    out << src.str();
}

void emitRuntimeHelpers(std::ostringstream& out) {
    out << "typedef void* mj2x_ref;\n";
    out << "#ifndef MJ2X_REGC_RUNTIME_ABI_DECLARED\n";
    out << "#define MJ2X_REGC_RUNTIME_ABI_DECLARED\n";
    out << "typedef void (*mj2x_regc_throw_callback)(void* env, const char* class_name, const char* message);\n";
    out << "typedef struct mj2x_regc_context {\n";
    out << "    void* env;\n";
    out << "    mj2x_regc_throw_callback throwException;\n";
    out << "} mj2x_regc_context;\n";
    out << "void mj2x_regc_init_context(mj2x_regc_context* context, void* env, mj2x_regc_throw_callback throwException);\n";
    out << "void mj2x_regc_set_env(mj2x_regc_context* context, void* env);\n";
    out << "void* mj2x_regc_env(const mj2x_regc_context* context);\n";
    out << "void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message);\n";
    out << "int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);\n";
    out << "int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);\n";
    out << "int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);\n";
    out << "int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);\n";
    out << "void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);\n\n";
    out << "#endif\n\n";
    out << "static JNIEnv* _mj2x_regc_env = NULL;\n\n";
    out << "static mj2x_regc_context _mj2x_regc_ctx;\n";
    out << "static mj2x_regc_context* _mj2x_regc_current_context(void) { return &_mj2x_regc_ctx; }\n\n";
    out << "static void _mj2x_regc_throw_runtime(void* env, const char* class_name, const char* message) {\n";
    out << "    JNIEnv* jni = (JNIEnv*)env;\n";
    out << "    if (jni == NULL || class_name == NULL) return;\n";
    out << "    jclass ex = (*jni)->FindClass(jni, class_name);\n";
    out << "    if (ex != NULL) (*jni)->ThrowNew(jni, ex, message != NULL ? message : \"\");\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_throw_unsupported(const char* feature) {\n";
    out << "    jclass ex = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, \"java/lang/UnsupportedOperationException\");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, feature);\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_throw_null(const char* message) {\n";
    out << "    jclass ex = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, \"java/lang/NullPointerException\");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, message);\n";
    out << "}\n\n";
    out << "static jclass _mj2x_regc_find_class(const char* desc) {\n";
    out << "    if (desc == NULL) return NULL;\n";
    out << "    if (desc[0] == 'L') {\n";
    out << "        char name[1024];\n";
    out << "        int i = 0;\n";
    out << "        while (desc[i + 1] != ';' && desc[i + 1] != '\\0' && i < 1023) { name[i] = desc[i + 1]; i++; }\n";
    out << "        name[i] = '\\0';\n";
    out << "        return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, name);\n";
    out << "    }\n";
    out << "    return (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, desc);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc) {\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->AllocObject(_mj2x_regc_env, cls);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length) {\n";
    out << "    if (type_desc == NULL || type_desc[0] != '[') return NULL;\n";
    out << "    switch (type_desc[1]) {\n";
    out << "    case 'Z': return (mj2x_ref)(*_mj2x_regc_env)->NewBooleanArray(_mj2x_regc_env, length);\n";
    out << "    case 'B': return (mj2x_ref)(*_mj2x_regc_env)->NewByteArray(_mj2x_regc_env, length);\n";
    out << "    case 'C': return (mj2x_ref)(*_mj2x_regc_env)->NewCharArray(_mj2x_regc_env, length);\n";
    out << "    case 'S': return (mj2x_ref)(*_mj2x_regc_env)->NewShortArray(_mj2x_regc_env, length);\n";
    out << "    case 'I': return (mj2x_ref)(*_mj2x_regc_env)->NewIntArray(_mj2x_regc_env, length);\n";
    out << "    case 'J': return (mj2x_ref)(*_mj2x_regc_env)->NewLongArray(_mj2x_regc_env, length);\n";
    out << "    case 'F': return (mj2x_ref)(*_mj2x_regc_env)->NewFloatArray(_mj2x_regc_env, length);\n";
    out << "    case 'D': return (mj2x_ref)(*_mj2x_regc_env)->NewDoubleArray(_mj2x_regc_env, length);\n";
    out << "    default: return (mj2x_ref)(*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, length, _mj2x_regc_find_class(type_desc + 1), NULL);\n";
    out << "    }\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, int32_t* dims) {\n";
    out << "    if (type_desc == NULL || dims == NULL || dim_count <= 0 || type_desc[0] != '[') {\n";
    out << "        _mj2x_regc_throw_unsupported(\"RegC multi-array helper received a malformed descriptor\");\n";
    out << "        return NULL;\n";
    out << "    }\n";
    out << "    if (dim_count == 1) return _mj2x_regc_alloc_array(type_desc, dims[0]);\n";
    out << "    jclass component = _mj2x_regc_find_class(type_desc + 1);\n";
    out << "    if (component == NULL) return NULL;\n";
    out << "    jobjectArray array = (*_mj2x_regc_env)->NewObjectArray(_mj2x_regc_env, dims[0], component, NULL);\n";
    out << "    if (array == NULL || (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;\n";
    out << "    /* Java multi-array allocation eagerly allocates every requested dimension.\n";
    out << "       Dimensions beyond dim_count stay NULL, matching MULTIANEWARRAY partial forms. */\n";
    out << "    for (int32_t i = 0; i < dims[0]; ++i) {\n";
    out << "        mj2x_ref child = _mj2x_regc_alloc_multi_array(type_desc + 1, dim_count - 1, dims + 1);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;\n";
    out << "        (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, array, i, (jobject)child);\n";
    out << "        if ((*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env)) return (mj2x_ref)array;\n";
    out << "    }\n";
    out << "    return (mj2x_ref)array;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref) {\n";
    out << "    if (array_ref == NULL) { _mj2x_regc_throw_null(\"array.length\"); return 0; }\n";
    out << "    return (int32_t)(*_mj2x_regc_env)->GetArrayLength(_mj2x_regc_env, (jarray)array_ref);\n";
    out << "}\n\n";
    out << "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc) {\n";
    out << "    if (value == NULL) return NULL;\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return NULL;\n";
    out << "    if ((*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls)) return value;\n";
    out << "    jclass ex = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, \"java/lang/ClassCastException\");\n";
    out << "    if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, type_desc);\n";
    out << "    return NULL;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc) {\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_find_class(type_desc);\n";
    out << "    if (cls == NULL) return 0;\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;\n";
    out << "}\n\n";
    out << "static void _mj2x_regc_monitor_enter(mj2x_ref value) { (*_mj2x_regc_env)->MonitorEnter(_mj2x_regc_env, (jobject)value); }\n";
    out << "static void _mj2x_regc_monitor_exit(mj2x_ref value) { (*_mj2x_regc_env)->MonitorExit(_mj2x_regc_env, (jobject)value); }\n";
    out << "static int32_t _mj2x_regc_exception_check(void) { return (*_mj2x_regc_env)->ExceptionCheck(_mj2x_regc_env) ? 1 : 0; }\n";
    out << "static mj2x_ref _mj2x_regc_exception_occurred(void) { return (mj2x_ref)(*_mj2x_regc_env)->ExceptionOccurred(_mj2x_regc_env); }\n";
    out << "static void _mj2x_regc_exception_clear(void) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); }\n";
    out << "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type) {\n";
    out << "    if (catch_type == NULL || catch_type[0] == '\\0') return 1;\n";
    out << "    if (value == NULL) return 0;\n";
    out << "    jclass cls = _mj2x_regc_find_class(catch_type);\n";
    out << "    if (cls == NULL) { (*_mj2x_regc_env)->ExceptionClear(_mj2x_regc_env); return 0; }\n";
    out << "    return (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)value, cls) ? 1 : 0;\n";
    out << "}\n";
    out << "static void _mj2x_regc_throw(mj2x_ref value) {\n";
    out << "    if (value == NULL) {\n";
    out << "        jclass ex = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, \"java/lang/NullPointerException\");\n";
    out << "        if (ex != NULL) (*_mj2x_regc_env)->ThrowNew(_mj2x_regc_env, ex, \"athrow null\");\n";
    out << "        return;\n";
    out << "    }\n";
    out << "    (*_mj2x_regc_env)->Throw(_mj2x_regc_env, (jthrowable)value);\n";
    out << "}\n";
    // Static and instance helpers share code, but the generated C passes an
    // explicit mode bit so a null instance receiver still throws NPE.
    emitRegCFieldAccessHelpers(out);
    out << "static int32_t _mj2x_regc_is_boolean_array(mj2x_ref array_ref) { jclass c = (*_mj2x_regc_env)->FindClass(_mj2x_regc_env, \"[Z\"); return c != NULL && (*_mj2x_regc_env)->IsInstanceOf(_mj2x_regc_env, (jobject)array_ref, c); }\n";
    out << "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } if (_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = JNI_FALSE; (*_mj2x_regc_env)->GetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, index, 1, &v); return v == JNI_TRUE ? 1 : 0; } jbyte v = 0; (*_mj2x_regc_env)->GetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, &v); return (int8_t)v; }\n";
    out << "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jshort v = 0; (*_mj2x_regc_env)->GetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, 1, &v); return (int16_t)v; }\n";
    out << "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jchar v = 0; (*_mj2x_regc_env)->GetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, &v); return (uint16_t)v; }\n";
    out << "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jint v = 0; (*_mj2x_regc_env)->GetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, &v); return v; }\n";
    out << "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0; } jlong v = 0; (*_mj2x_regc_env)->GetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, &v); return v; }\n";
    out << "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0f; } jfloat v = 0; (*_mj2x_regc_env)->GetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, index, 1, &v); return v; }\n";
    out << "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return 0.0; } jdouble v = 0; (*_mj2x_regc_env)->GetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, index, 1, &v); return v; }\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array load\"); return NULL; } return (mj2x_ref)(*_mj2x_regc_env)->GetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, index); }\n";
    out << "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } if (_mj2x_regc_is_boolean_array(array_ref)) { jboolean v = value != 0 ? JNI_TRUE : JNI_FALSE; (*_mj2x_regc_env)->SetBooleanArrayRegion(_mj2x_regc_env, (jbooleanArray)array_ref, index, 1, &v); return; } jbyte v = (jbyte)value; (*_mj2x_regc_env)->SetByteArrayRegion(_mj2x_regc_env, (jbyteArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jshort v = (jshort)value; (*_mj2x_regc_env)->SetShortArrayRegion(_mj2x_regc_env, (jshortArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jchar v = (jchar)value; (*_mj2x_regc_env)->SetCharArrayRegion(_mj2x_regc_env, (jcharArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jint v = value; (*_mj2x_regc_env)->SetIntArrayRegion(_mj2x_regc_env, (jintArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jlong v = value; (*_mj2x_regc_env)->SetLongArrayRegion(_mj2x_regc_env, (jlongArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jfloat v = value; (*_mj2x_regc_env)->SetFloatArrayRegion(_mj2x_regc_env, (jfloatArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } jdouble v = value; (*_mj2x_regc_env)->SetDoubleArrayRegion(_mj2x_regc_env, (jdoubleArray)array_ref, index, 1, &v); }\n";
    out << "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref value) { if (array_ref == NULL) { _mj2x_regc_throw_null(\"array store\"); return; } (*_mj2x_regc_env)->SetObjectArrayElement(_mj2x_regc_env, (jobjectArray)array_ref, index, (jobject)value); }\n\n";
    out << "static const char* _mj2x_regc_skip_type(const char* p) {\n";
    out << "    while (*p == '[') p++;\n";
    out << "    if (*p == 'L') { while (*p != ';' && *p != '\\0') p++; return *p == ';' ? p + 1 : p; }\n";
    out << "    return *p == '\\0' ? p : p + 1;\n";
    out << "}\n\n";
    out << "static int32_t _mj2x_regc_count_params(const char* desc) {\n";
    out << "    int32_t count = 0;\n";
    out << "    const char* p = desc + 1;\n";
    out << "    while (*p != ')' && *p != '\\0') { p = _mj2x_regc_skip_type(p); count++; }\n";
    out << "    return count;\n";
    out << "}\n\n";
    out << "static jobject _mj2x_regc_collect_args(const char* desc, int32_t arg_count, va_list ap, jvalue* values) {\n";
    out << "    int32_t param_count = _mj2x_regc_count_params(desc);\n";
    out << "    jobject receiver = NULL;\n";
    out << "    if (arg_count == param_count + 1) receiver = (jobject)va_arg(ap, mj2x_ref);\n";
    out << "    const char* p = desc + 1;\n";
    out << "    int32_t i = 0;\n";
    out << "    while (*p != ')' && *p != '\\0' && i < 32) {\n";
    out << "        char tag = *p;\n";
    out << "        while (tag == '[') tag = *++p;\n";
    out << "        switch (tag) {\n";
    out << "        case 'Z': values[i].z = (jboolean)va_arg(ap, int); break;\n";
    out << "        case 'B': values[i].b = (jbyte)va_arg(ap, int); break;\n";
    out << "        case 'C': values[i].c = (jchar)va_arg(ap, int); break;\n";
    out << "        case 'S': values[i].s = (jshort)va_arg(ap, int); break;\n";
    out << "        case 'I': values[i].i = (jint)va_arg(ap, int); break;\n";
    out << "        case 'J': values[i].j = (jlong)va_arg(ap, int64_t); break;\n";
    out << "        case 'F': values[i].f = (jfloat)va_arg(ap, double); break;\n";
    out << "        case 'D': values[i].d = (jdouble)va_arg(ap, double); break;\n";
    out << "        default: values[i].l = (jobject)va_arg(ap, mj2x_ref); break;\n";
    out << "        }\n";
    out << "        p = _mj2x_regc_skip_type(p);\n";
    out << "        i++;\n";
    out << "    }\n";
    out << "    return receiver;\n";
    out << "}\n\n";
    emitRegCMethodCallHelpers(out);
    // String LDC intrinsic helper.  This is the only JNI touchpoint of the
    // String intrinsic path: a String literal becomes a jstring via
    // NewStringUTF so it can flow into parseInt/toString without per-call JNI
    // method dispatch.  context is accepted for ABI symmetry with future
    // throwing String intrinsics.
    out << "static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8) {\n";
    out << "    (void)context;\n";
    out << "    if (utf8 == NULL) return NULL;\n";
    out << "    return (mj2x_ref)(*_mj2x_regc_env)->NewStringUTF(_mj2x_regc_env, utf8);\n";
    out << "}\n\n";
    // Integer.parseInt intrinsic.  Reads the jstring's modified-UTF-8 bytes,
    // parses the leading integer, and throws NumberFormatException through the
    // context callback when the input is not a parseable int.  This is the only
    // JNI touchpoint of parseInt: the parse itself stays in native strtol, no
    // CallStaticIntMethod dispatch is emitted for the parseInt call site.
    out << "static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr_ref) {\n";
    out << "    if (jstr_ref == NULL) {\n";
    out << "        if (context != NULL && context->throwException != NULL) {\n";
    out << "            context->throwException(context->env, \"java/lang/NullPointerException\", \"parseInt\");\n";
    out << "        }\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    const char* utf = (*_mj2x_regc_env)->GetStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, NULL);\n";
    out << "    if (utf == NULL) return 0;\n";
    out << "    char* end = NULL;\n";
    out << "    errno = 0;\n";
    out << "    long parsed = strtol(utf, &end, 10);\n";
    out << "    int failed = (end == utf) || (errno == ERANGE) ||\n";
    // Use explicit casts: a bare 2147483648L literal is ill-typed on 32-bit
    // long targets (MSVC), which would flip the range check on valid inputs.
    out << "                 (parsed > (long)INT32_MAX) || (parsed < (long)INT32_MIN);\n";
    // JDK Integer.parseInt rejects trailing non-digit characters too; mirror
    // that by checking the whole string (skipping ASCII whitespace only at the
    // leading edge, matching JDK which uses Character.isWhitespace via the
    // leading-sign/no-trim Integer.parseInt(String) contract).
    out << "    if (!failed) { while (*end != '\\0' && (*end == ' ' || *end == '\\t')) end++; failed = (*end != '\\0'); }\n";
    out << "    (*_mj2x_regc_env)->ReleaseStringUTFChars(_mj2x_regc_env, (jstring)jstr_ref, utf);\n";
    out << "    if (failed) {\n";
    out << "        if (context != NULL && context->throwException != NULL) {\n";
    out << "            context->throwException(context->env, \"java/lang/NumberFormatException\", \"For input string\");\n";
    out << "        }\n";
    out << "        return 0;\n";
    out << "    }\n";
    out << "    return (int32_t)parsed;\n";
    out << "}\n\n";
}

} // namespace

struct RegCBackend::Impl {
    std::ostringstream source;
};

RegCBackend::RegCBackend()
    : impl_(std::make_unique<Impl>())
{}

RegCBackend::~RegCBackend() = default;

BackendResult RegCBackend::compileMethod(
    const std::string& className,
    CompilerMethod& method,
    const std::vector<Block*>& blocks) {
    BackendResult backendResult;
    auto* mn = method.methodNode;
    const std::string methodName = mn->name.to_std_string();
    const std::string methodDesc = mn->desc.to_std_string();
    const bool isStatic = (mn->access & Opcodes::ACC_STATIC) != 0;

    std::string reason;
    SSABuilder ssaBuilder;
    // RegCBackend is a facade over the existing IR pipeline: L1 blocks are
    // supplied by Pipeline, then this backend owns the SSA and RegIR handoff.
    auto ssa = ssaBuilder.build(blocks, className, methodName, methodDesc,
                                isStatic, mn->maxLocals, reason);
    if (!ssa) {
        backendResult.text = emitJniWrapper(className, *mn, RegCEmitResult{false, {}, {}, reason});
        return backendResult;
    }

    RegLowering lowering;
    auto reg = lowering.lower(*ssa, reason);
    if (!reg) {
        backendResult.text = emitJniWrapper(className, *mn, RegCEmitResult{false, {}, {}, reason});
        return backendResult;
    }

    RegCEmitter emitter;
    auto emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        backendResult.text = emitJniWrapper(className, *mn, emitted);
        return backendResult;
    }

    // Keep the stackless helper as the trace surface and add a thin JNI entry
    // point only for JVM native binding.  The wrapper must not reintroduce a
    // StackC-style operand stack.
    std::ostringstream methodSource;
    methodSource << emitted.source << "\n";
    methodSource << emitJniWrapper(className, *mn, emitted);

    backendResult.kind = BackendResult::Kind::C_SOURCE;
    backendResult.text = methodSource.str();
    impl_->source << methodSource.str();
    return backendResult;
}

void RegCBackend::injectIntoClass(
    CompilerClass& clazz,
    CompilerMethod& method,
    const BackendResult& result) {
    // Failed lowering leaves the Java bytecode intact.  This prevents a class
    // from being rewritten to native when no working RegC body was emitted.
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    auto* mn = method.methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack = 0;
    mn->maxLocals = 0;
    clazz.modified = true;
}

void RegCBackend::finalize(const CompilerIndex& index) {
    (void)index;

    // strtol + errno back the Integer.parseInt intrinsic body emitted below.
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdbool.h");
    includes.addSystem("stdint.h");
    includes.addSystem("stddef.h");
    includes.addSystem("stdarg.h");
    includes.addSystem("math.h");
    includes.addSystem("stdlib.h");
    includes.addSystem("errno.h");

    std::ostringstream full;
    full << "/* Generated by mj2x RegCBackend */\n";
    full << includes.render();
    full << "\n";
    emitRuntimeHelpers(full);
    full << impl_->source.str();

    sourceCode_ = full.str();
    if (!cOutputPath_.empty()) {
        std::ofstream out(cOutputPath_, std::ios::binary);
        if (out) {
            out << sourceCode_;
            printf("[RegCBackend] Written: %s (%zu bytes)\n",
                   cOutputPath_.c_str(), sourceCode_.size());
        } else {
            printf("[RegCBackend] ERROR: cannot write %s\n", cOutputPath_.c_str());
        }
    }
}

} // namespace mj2x

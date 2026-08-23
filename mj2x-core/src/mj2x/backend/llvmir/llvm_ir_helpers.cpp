/// @file llvm_ir_helpers.cpp
/// @brief LLVM IR 后端共享 JNI helper 发射：异常/对象/字段/数组/调用/monitor 辅助函数。
#include "llvm_ir_internal.hpp"

#include <cmath>
#include <iomanip>

namespace mj2x {
namespace llvmir {

using namespace jbytecode::core;
using namespace jbytecode::tree;


std::string emitThrowArithmeticHelper()
{
    // throw_arith:#894 mj2x-llvmrt 阶段一下沉 mj2x-rt
    // (src/llvmir/mj2x_llvmir_runtime.c);私有字符串常量随实现走,模块只
    // 携带 declare。
    return "declare void @mj2x_llvm_throw_arith(ptr %env)\n\n";
}

/// @brief JNIEnv 函数表槽位（本机 jni.h 精确提取 + reserved0-3 偏移）。

/// @brief 对象/字段/类型检查共享 helper 集合（finalize 统一生成）。
///
/// @details 字段 kind 表：JNI Get/Set 函数槽、返回/参数类型、窄类型扩展方向。
/// helper 首参统一 JNIEnv*；字段访问带 null 检查（实例 receiver 为 null 抛 NPE）。
std::string emitObjectHelpers()
{
    struct FieldKindInfo
    {
        const char* kind;       // helper 后缀
        const char* retType;    // helper 返回类型（窄提升后）
    };
    static const FieldKindInfo kinds[] = {
        {"bool", "i32"},   {"byte", "i32"},   {"char", "i32"},  {"short", "i32"},
        {"int", "i32"},    {"long", "i64"},   {"float", "float"}, {"double", "double"},
        {"ref", "ptr"},
    };

    std::ostringstream out;

    // throw_null:#894 mj2x-llvmrt 阶段一下沉 mj2x-rt;后续 helper 体仍以
    // 调用点引用它,模块携带 declare。
    out << "declare void @mj2x_llvm_throw_null(ptr %env)\n\n";

    // new_object：FindClass + AllocObject（不调用 <init>；构造器由 invokespecial 处理）。
    // new_object:#894 mj2x-llvmrt 阶段二下沉 mj2x-rt,模块携带 declare。
    out << "declare ptr @mj2x_llvm_new_object(ptr %env, ptr %name)\n\n";

    // new_array：按元素描述符首字符分发 JNI NewXxxArray / NewObjectArray。
    // new_array:#894 mj2x-llvmrt 阶段三下沉。
    out << "declare ptr @mj2x_llvm_new_array(ptr %env, ptr %desc, i32 %count)\n\n";

    // new_multi_array：NewObjectArray 递归（JVM EagerAlloc 语义）。
    out << "declare ptr @mj2x_llvm_new_multi_array(ptr %env, ptr %desc, ptr %dims, i32 %ndims)\n\n";

    // 字段 load/store helpers(static/instance 合一 + null 检查)。
    // 字段族:#894 mj2x-llvmrt 阶段三b 下沉 mj2x-rt,模块只携带 declare。
    for (const auto& k : kinds) {
        out << "declare " << k.retType << " @mj2x_llvm_field_load_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static)\n\n";
        out << "declare void @mj2x_llvm_field_store_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static, "
            << k.retType << " %val)\n\n";
    }

    // 数组元素读写 helper：null → NPE；越界由 JNI Get/Set*ArrayRegion /
    // Get/SetObjectArrayElement 抛 ArrayIndexOutOfBoundsException / 存储违例抛
    // ArrayStoreException。byte 形态特判 boolean[]（JNI 要求 GetBooleanArrayRegion）。
    // 数组元素族:#894 mj2x-llvmrt 阶段三c 下沉 mj2x-rt,模块只携带 declare。
    struct ArrayKindInfo
    {
        const char* kind;      // helper 后缀
        SSAType     ssaType;   // 元素 SSA 类型
        const char* retType;   // helper 返回类型（窄提升后）
    };
    static const ArrayKindInfo arrayKinds[] = {
        {"byte", SSAType::I8, "i32"},
        {"short", SSAType::I16, "i32"},
        {"char", SSAType::CHAR, "i32"},
        {"int", SSAType::I32, "i32"},
        {"long", SSAType::I64, "i64"},
        {"float", SSAType::F32, "float"},
        {"double", SSAType::F64, "double"},
        {"ref", SSAType::REF, "ptr"},
    };
    for (const auto& k : arrayKinds) {
        out << "declare " << k.retType << " @mj2x_llvm_array_load_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx)\n\n";
        out << "declare void @mj2x_llvm_array_store_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx, " << k.retType << " %val)\n\n";
    }
    // array_length：null → NPE；GetArrayLength。
    out << "declare i32 @mj2x_llvm_array_length(ptr %env, ptr %arr)\n\n";

    // JNI 方法调用 helper：CallXxxMethodA（jvalue 数组）。
    // virtual/interface → Call<Type>MethodA；special → CallNonvirtual<Type>MethodA
    // （GetObjectClass + 显式类，用于 <init>/private/super）；static 外部 →
    // CallStatic<Type>MethodA。null receiver（实例）抛 NPE。
    // 调用族:#894 mj2x-llvmrt 阶段三d 下沉 mj2x-rt,模块只携带 declare。
    struct CallKindInfo
    {
        const char* kind;
        const char* boundary;       // JNI 返回类型
    };
    static const CallKindInfo callKinds[] = {
        {"object", "ptr"},  {"bool", "i8"},    {"byte", "i8"},   {"char", "i16"},
        {"short", "i16"},   {"int", "i32"},    {"long", "i64"},  {"float", "float"},
        {"double", "double"}, {"void", "void"},
    };
    for (const auto& k : callKinds) {
        out << "declare " << k.boundary << " @mj2x_llvm_call_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv)\n\n";
        out << "declare " << k.boundary << " @mj2x_llvm_call_nonvirtual_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv)\n\n";
        out << "declare " << k.boundary << " @mj2x_llvm_call_static_" << k.kind
            << "(ptr %env, ptr %owner, ptr %name, ptr %desc, ptr %jv)\n\n";
    }

    // check_cast：IsInstanceOf == 0 → 挂起 ClassCastException；返回原引用。
    // check_cast:#894 mj2x-llvmrt 阶段二下沉(失败路径按 JVM 语义抛 CCE)。
    out << "declare ptr @mj2x_llvm_check_cast(ptr %env, ptr %obj, ptr %desc)\n\n";

    // instance_of：IsInstanceOf → i32 0/1（intrinsic "instanceof:<desc>"）。
    // instance_of:#894 mj2x-llvmrt 阶段二下沉。
    out << "declare i32 @mj2x_llvm_instance_of(ptr %env, ptr %obj, ptr %desc)\n\n";

    // 异常基础设施：ExceptionCheck/Occurred/Clear/Matches（catch 类型匹配，
    // 空串 = catch-all/finally）/Throw（athrow；null → NPE）。
    // exception_check:#894 mj2x-llvmrt 阶段二下沉。
    out << "declare i1 @mj2x_llvm_exception_check(ptr %env)\n\n";
    // exception_occurred/clear:#894 mj2x-llvmrt 阶段一下沉 mj2x-rt
    // (src/llvmir/mj2x_llvmir_runtime.c),模块只携带 declare。
    out << "declare ptr @mj2x_llvm_exception_occurred(ptr %env)\n\n";
    out << "declare void @mj2x_llvm_exception_clear(ptr %env)\n\n";
    // exception_matches:#894 mj2x-llvmrt 阶段二下沉。
    out << "declare i1 @mj2x_llvm_exception_matches(ptr %env, ptr %ex, ptr %catch)\n\n";
    // throw(athrow):#894 mj2x-llvmrt 阶段二下沉。
    out << "declare void @mj2x_llvm_throw(ptr %env, ptr %obj)\n\n";
    // 按类名抛新异常（FindClass + ThrowNew）：发射端预检类语义陷阱用
    //（如 Random.nextInt(bound<=0) 的 IllegalArgumentException）。
    // throw_new:#894 mj2x-llvmrt 阶段四a 下沉 mj2x-rt(内部转调
    // mj2x_llvm_class_const),模块只携带 declare。
    out << "declare void @mj2x_llvm_throw_new(ptr %env, ptr %cls_name, ptr %msg)\n\n";
    // String/Class 常量：NewStringUTF（modified UTF-8 字节）/ FindClass。
    // string_const:#894 mj2x-llvmrt 阶段二下沉。
    out << "declare ptr @mj2x_llvm_string_const(ptr %env, ptr %utf8)\n\n";
    // 引用同一性比较：JNI local ref 是句柄，同一对象经两次 JNI 调用返回的句柄
    // 指针不一定相等（对齐 RegC regc_flow.cpp 的 IsSameObject 语义）。
    // ref_eq:#894 mj2x-llvmrt 阶段二下沉。
    out << "declare i1 @mj2x_llvm_ref_eq(ptr %env, ptr %a, ptr %b)\n\n";
    // 类常量：普通名 FindClass；基本类型描述符（V/Z/B/C/S/I/J/F/D）→
    // Void.class / 包装类 TYPE 字段（ldc class I 等 javac 形态，FindClass("I")
    // 会抛 ClassNotFoundException）。class_const:#894 mj2x-llvmrt 阶段四a
    // 下沉 mj2x-rt,包装类名/TYPE 常量随实现走,模块只携带 declare。
    out << "declare ptr @mj2x_llvm_class_const(ptr %env, ptr %name)\n\n";
    // MonitorEnter/Exit（null → NPE ThrowNew）。monitor:#894 mj2x-llvmrt
    // 阶段四b 下沉 mj2x-rt,模块只携带 declare。
    out << "declare void @mj2x_llvm_monitor_enter(ptr %env, ptr %obj)\n\n";
    out << "declare void @mj2x_llvm_monitor_exit(ptr %env, ptr %obj)\n\n";

    return out.str();
}

// ---- invokedynamic 运行时解析 helper（含 indy 站点时 finalize 统一生成）----

namespace {

/// @brief 生成私有常量串全局（固定名；与 GlobalStringTable 相同转义规则）。
std::string llvmConstantString(const std::string& name, const std::string& text)
{
    std::ostringstream out;
    out << "@" << name << " = private unnamed_addr constant [" << (text.size() + 1)
        << " x i8] c\"";
    for (const unsigned char ch : text) {
        if (ch == '\\' || ch == '"') {
            out << '\\' << static_cast<char>(ch);
        }
        else if (ch < 32 || ch > 126) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%02X", ch);
            out << buf;
        }
        else {
            out << static_cast<char>(ch);
        }
    }
    out << "\\00\"\n";
    return out.str();
}

/// @brief LLVM 浮点字面量（NaN/±Inf/-0.0 用 hex 位模式；正常值 decimal scientific）。
std::string llvmFloatLiteral(double value, bool isF32)
{
    std::string bits;
    if (isF32) {
        if (std::isnan(value)) bits = "0x7FC00000";
        else if (std::isinf(value))
            bits = std::signbit(value) ? "0xFF800000" : "0x7F800000";
        else if (value == 0.0 && std::signbit(value)) bits = "0x80000000";
    }
    else {
        if (std::isnan(value)) bits = "0x7FF8000000000000";
        else if (std::isinf(value))
            bits = std::signbit(value) ? "0xFFF0000000000000" : "0x7FF0000000000000";
        else if (value == 0.0 && std::signbit(value)) bits = "0x8000000000000000";
    }
    if (!bits.empty()) return bits;
    std::ostringstream literal;
    literal << std::scientific << std::showpoint;
    if (isF32) {
        literal << std::setprecision(9) << static_cast<float>(value);
    }
    else {
        literal << std::setprecision(17) << value;
    }
    return literal.str();
}

/// @brief bsm 静态参数类型描述串：METHOD → 方法描述符；OBJECT → 内部名
/// （FindClass 需要 "java/lang/String" 而非 "Ljava/lang/String;"）；
/// 其余（原语/数组）→ 原样描述符。
std::string indyTypeText(const jbytecode::core::Type& type)
{
    const auto desc = type.getDescriptor();
    if (type.getSort() == jbytecode::core::Type::OBJECT) {
        // 剥掉 L…;（OBJECT 排序的 descriptor 带包裹符）。
        const auto text = desc.to_std_string();
        return text.size() >= 2 ? text.substr(1, text.size() - 2) : text;
    }
    return desc.to_std_string();
}

/// @brief 包装类名 → 合法 LLVM 全局标识符（/ → _）。
std::string llvmGlobalTag(const char* text)
{
    std::string result = "_mj2x_llvm_indy_";
    for (const char ch : std::string(text)) result += ch == '/' ? '_' : ch;
    return result;
}

/// @brief bsm 静态参数 → jobject 物化 RHS（空串 = null 常量；常量串进 consts）。
///
/// @param consts 收集本 site 需要的常量串全局定义（函数文本之后统一输出）。
/// @param seq    唯一后缀（bsmArgIndex 或 varargs 下标），保证常量名不冲突。
std::string indyBsmArgExpr(const std::string& siteTag, const ca::core::Any& value,
                           std::ostringstream& consts, size_t seq)
{
    const std::string g = "_mj2x_llvm_indy_s" + siteTag + "_arg_" + std::to_string(seq);
    if (const auto* p = value.as<std::string>()) {
        consts << llvmConstantString(g + "_str", *p);
        return "call ptr @mj2x_llvm_string_const(ptr %env, ptr @" + g + "_str)";
    }
    if (const auto* p = value.as<ca::str::Utf8StringRef>()) {
        consts << llvmConstantString(g + "_str", p->to_std_string());
        return "call ptr @mj2x_llvm_string_const(ptr %env, ptr @" + g + "_str)";
    }
    if (const auto* p = value.as<Type>()) {
        consts << llvmConstantString(g + "_desc", indyTypeText(*p));
        if (p->getSort() == Type::METHOD) {
            return "call ptr @mj2x_llvm_indy_method_type(ptr %env, ptr %owner, ptr @" + g +
                   "_desc)";
        }
        return "call ptr @mj2x_llvm_indy_class_object(ptr %env, ptr @" + g + "_desc)";
    }
    if (const auto* p = value.as<jbytecode::core::Handle>()) {
        consts << llvmConstantString(g + "_owner", p->getOwner().to_std_string());
        consts << llvmConstantString(g + "_name", p->getName().to_std_string());
        consts << llvmConstantString(g + "_desc", p->getDesc().to_std_string());
        return "call ptr @mj2x_llvm_indy_handle(ptr %env, ptr %lookup2, ptr %owner, i32 " +
               std::to_string(p->getTag()) + ", ptr @" + g + "_owner, ptr @" + g +
               "_name, ptr @" + g + "_desc)";
    }
    if (const auto* p = value.as<ca::i32>()) {
        return "call ptr @mj2x_llvm_box_int(ptr %env, i32 " + std::to_string(*p) + ")";
    }
    if (const auto* p = value.as<ca::i64>()) {
        return "call ptr @mj2x_llvm_box_long(ptr %env, i64 " + std::to_string(*p) + ")";
    }
    if (const auto* p = value.as<float>()) {
        return "call ptr @mj2x_llvm_box_float(ptr %env, float " + llvmFloatLiteral(*p, true) +
               ")";
    }
    if (const auto* p = value.as<double>()) {
        return "call ptr @mj2x_llvm_box_double(ptr %env, double " + llvmFloatLiteral(*p, false) +
               ")";
    }
    return "";   // 未知形态 → null 常量
}

}   // namespace

std::string emitIndyHelpers()
{
    std::ostringstream out;
    out << "; ---- invokedynamic runtime helpers ----\n";
    out << llvmConstantString("_mj2x_llvm_indy_java_lang_Object", "java/lang/Object");
    out << llvmConstantString("_mj2x_llvm_indy_java_lang_Class", "java/lang/Class");
    out << llvmConstantString("_mj2x_llvm_indy_getClassLoader", "getClassLoader");
    out << llvmConstantString("_mj2x_llvm_indy_getClassLoader_desc",
                              "()Ljava/lang/ClassLoader;");
    out << llvmConstantString("_mj2x_llvm_indy_MethodType", "java/lang/invoke/MethodType");
    out << llvmConstantString("_mj2x_llvm_indy_fromMethodDescriptorString",
                              "fromMethodDescriptorString");
    out << llvmConstantString("_mj2x_llvm_indy_fds_desc",
                              "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/"
                              "MethodType;");
    out << llvmConstantString("_mj2x_llvm_indy_TYPE", "TYPE");
    out << llvmConstantString("_mj2x_llvm_indy_TYPE_desc", "Ljava/lang/Class;");
    out << llvmConstantString("_mj2x_llvm_indy_Lookup", "java/lang/invoke/MethodHandles$Lookup");

    // new_object_array / set_object_array_element：Object[] 参数数组承载。
    out << "declare ptr @mj2x_llvm_new_object_array(ptr %env, i32 %count, ptr %cls)\n\n";
    out << "declare void @mj2x_llvm_set_object_array_element(ptr %env, ptr %arr, i32 %idx, ptr %val)\n\n";

    // MethodType.fromMethodDescriptorString(desc, owner.getClassLoader())。
    out << "define internal ptr @mj2x_llvm_indy_method_type(ptr %env, ptr %owner, ptr %desc) {\n"
           "entry:\n"
           "  %loader = call ptr @mj2x_llvm_call_object(ptr %env, ptr %owner, ptr "
           "@_mj2x_llvm_indy_java_lang_Class, ptr @_mj2x_llvm_indy_getClassLoader, ptr "
           "@_mj2x_llvm_indy_getClassLoader_desc, ptr null)\n"
           "  %descstr = call ptr @mj2x_llvm_string_const(ptr %env, ptr %desc)\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %mtcls = call ptr %find(ptr %env, ptr @_mj2x_llvm_indy_MethodType)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %gsm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetStaticMethodID << "\n"
        << "  %getsm = load ptr, ptr %gsm0\n"
           "  %mid = call ptr %getsm(ptr %env, ptr %mtcls, ptr "
           "@_mj2x_llvm_indy_fromMethodDescriptorString, ptr @_mj2x_llvm_indy_fds_desc)\n"
           "  %jv = alloca [2 x { i64 }]\n"
           "  %s0 = getelementptr [2 x { i64 }], ptr %jv, i64 0, i64 0\n"
           "  store ptr %descstr, ptr %s0\n"
           "  %s1 = getelementptr [2 x { i64 }], ptr %jv, i64 0, i64 1\n"
           "  store ptr %loader, ptr %s1\n"
           "  %mt = call ptr @mj2x_llvm_indy_call_static_mid(ptr %env, ptr %mtcls, ptr %mid, "
           "ptr %jv)\n"
           "  ret ptr %mt\n"
           "}\n\n";

    // 包装类 TYPE 静态字段 → 基本类型 Class 对象。
    out << "define internal ptr @mj2x_llvm_indy_wrapper_type(ptr %env, ptr %name) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr %name)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %gsf0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetStaticFieldID << "\n"
        << "  %getsf = load ptr, ptr %gsf0\n"
           "  %fid = call ptr %getsf(ptr %env, ptr %cls, ptr @_mj2x_llvm_indy_TYPE, ptr "
           "@_mj2x_llvm_indy_TYPE_desc)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %gso0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::GetStaticObjectField
        << "\n"
           "  %gsof = load ptr, ptr %gso0\n"
           "  %t = call ptr %gsof(ptr %env, ptr %cls, ptr %fid)\n"
           "  ret ptr %t\n"
           "}\n\n";

    // Class 对象：V → Void；基本类型 → 包装类 TYPE；其余 → FindClass。
    const struct { const char* label; const char* wrapper; } wrappers[] = {
        {"void_", "java/lang/Void"},  {"bool_", "java/lang/Boolean"},
        {"byte_", "java/lang/Byte"},  {"char_", "java/lang/Character"},
        {"short_", "java/lang/Short"}, {"int_", "java/lang/Integer"},
        {"long_", "java/lang/Long"},  {"float_", "java/lang/Float"},
        {"double_", "java/lang/Double"},
    };
    for (const auto& entry : wrappers) {
        std::string gname = "_mj2x_llvm_indy_";
        for (const char ch : std::string(entry.wrapper)) gname += ch == '/' ? '_' : ch;
        out << llvmConstantString(gname, entry.wrapper);
    }
    out << "define internal ptr @mj2x_llvm_indy_class_object(ptr %env, ptr %desc) {\n"
           "entry:\n"
           "  %head = load i8, ptr %desc\n"
           "  switch i8 %head, label %ref [\n"
           "    i8 86, label %void_\n"
           "    i8 90, label %bool_\n"
           "    i8 66, label %byte_\n"
           "    i8 67, label %char_\n"
           "    i8 83, label %short_\n"
           "    i8 73, label %int_\n"
           "    i8 74, label %long_\n"
           "    i8 70, label %float_\n"
           "    i8 68, label %double_\n"
           "  ]\n";
    for (const auto& entry : wrappers) {
        std::string gname = "_mj2x_llvm_indy_";
        for (const char ch : std::string(entry.wrapper)) gname += ch == '/' ? '_' : ch;
        out << entry.label << ":\n"
            << "  %" << entry.label << "c = call ptr @mj2x_llvm_indy_wrapper_type(ptr %env, ptr @"
            << gname << ")\n"
            << "  ret ptr %" << entry.label << "c\n";
    }
    out << "ref:\n"
           "  %r = call ptr @mj2x_llvm_class_const(ptr %env, ptr %desc)\n"
           "  ret ptr %r\n"
           "}\n\n";

    // Lookup.find* 专用直调（cls+mid 已解析）：静态/实例 MethodA。
    // call_static_mid/call_obj_mid:#894 mj2x-llvmrt 阶段四c 下沉 mj2x-rt。
    out << "declare ptr @mj2x_llvm_indy_call_static_mid(ptr %env, ptr %cls, ptr %mid, "
           "ptr %jv)\n\n";
    out << "declare ptr @mj2x_llvm_indy_call_obj_mid(ptr %env, ptr %obj, ptr %cls, "
           "ptr %mid, ptr %jv)\n\n";

    // 字段型 Handle（H_GETFIELD/H_GETSTATIC/H_PUTFIELD/H_PUTSTATIC）：
    // lookup.find[Static][Getter|Setter](owner_cls, name, Class)。
    out << "define internal ptr @mj2x_llvm_indy_field_handle(ptr %env, ptr %lkcls, ptr %lookup, "
           "ptr %ocls, ptr %name, ptr %desc, ptr %findName, ptr %findDesc) {\n"
           "entry:\n"
           "  %ftype = call ptr @mj2x_llvm_indy_class_object(ptr %env, ptr %desc)\n"
           "  %namestr = call ptr @mj2x_llvm_string_const(ptr %env, ptr %name)\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %gm0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetMethodID << "\n"
        << "  %getmid = load ptr, ptr %gm0\n"
           "  %mid = call ptr %getmid(ptr %env, ptr %lkcls, ptr %findName, ptr %findDesc)\n"
           "  %jv = alloca [3 x { i64 }]\n"
           "  %s0 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 0\n"
           "  store ptr %ocls, ptr %s0\n"
           "  %s1 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 1\n"
           "  store ptr %namestr, ptr %s1\n"
           "  %s2 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 2\n"
           "  store ptr %ftype, ptr %s2\n"
           "  %r = call ptr @mj2x_llvm_indy_call_obj_mid(ptr %env, ptr %lookup, ptr %lkcls, "
           "ptr %mid, ptr %jv)\n"
           "  ret ptr %r\n"
           "}\n\n";

    // 方法型 Handle 三/四/二参形态（findVirtual/findStatic/findSpecial/findConstructor）。
    out << "define internal ptr @mj2x_llvm_indy_method_handle3(ptr %env, ptr %lkcls, ptr "
           "%lookup, ptr %ocls, ptr %name, ptr %type, ptr %findName, ptr %findDesc) {\n"
           "entry:\n"
           "  %namestr = call ptr @mj2x_llvm_string_const(ptr %env, ptr %name)\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %gm0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetMethodID << "\n"
        << "  %getmid = load ptr, ptr %gm0\n"
           "  %mid = call ptr %getmid(ptr %env, ptr %lkcls, ptr %findName, ptr %findDesc)\n"
           "  %jv = alloca [3 x { i64 }]\n"
           "  %s0 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 0\n"
           "  store ptr %ocls, ptr %s0\n"
           "  %s1 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 1\n"
           "  store ptr %namestr, ptr %s1\n"
           "  %s2 = getelementptr [3 x { i64 }], ptr %jv, i64 0, i64 2\n"
           "  store ptr %type, ptr %s2\n"
           "  %r = call ptr @mj2x_llvm_indy_call_obj_mid(ptr %env, ptr %lookup, ptr %lkcls, "
           "ptr %mid, ptr %jv)\n"
           "  ret ptr %r\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_indy_method_handle4(ptr %env, ptr %lkcls, ptr "
           "%lookup, ptr %ocls, ptr %name, ptr %type, ptr %special_cls, ptr %findName, ptr "
           "%findDesc) {\n"
           "entry:\n"
           "  %namestr = call ptr @mj2x_llvm_string_const(ptr %env, ptr %name)\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %gm0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetMethodID << "\n"
        << "  %getmid = load ptr, ptr %gm0\n"
           "  %mid = call ptr %getmid(ptr %env, ptr %lkcls, ptr %findName, ptr %findDesc)\n"
           "  %jv = alloca [4 x { i64 }]\n"
           "  %s0 = getelementptr [4 x { i64 }], ptr %jv, i64 0, i64 0\n"
           "  store ptr %ocls, ptr %s0\n"
           "  %s1 = getelementptr [4 x { i64 }], ptr %jv, i64 0, i64 1\n"
           "  store ptr %namestr, ptr %s1\n"
           "  %s2 = getelementptr [4 x { i64 }], ptr %jv, i64 0, i64 2\n"
           "  store ptr %type, ptr %s2\n"
           "  %s3 = getelementptr [4 x { i64 }], ptr %jv, i64 0, i64 3\n"
           "  store ptr %special_cls, ptr %s3\n"
           "  %r = call ptr @mj2x_llvm_indy_call_obj_mid(ptr %env, ptr %lookup, ptr %lkcls, "
           "ptr %mid, ptr %jv)\n"
           "  ret ptr %r\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_indy_method_handle2(ptr %env, ptr %lkcls, ptr "
           "%lookup, ptr %ocls, ptr %type, ptr %findName, ptr %findDesc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %gm0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetMethodID << "\n"
        << "  %getmid = load ptr, ptr %gm0\n"
           "  %mid = call ptr %getmid(ptr %env, ptr %lkcls, ptr %findName, ptr %findDesc)\n"
           "  %jv = alloca [2 x { i64 }]\n"
           "  %s0 = getelementptr [2 x { i64 }], ptr %jv, i64 0, i64 0\n"
           "  store ptr %ocls, ptr %s0\n"
           "  %s1 = getelementptr [2 x { i64 }], ptr %jv, i64 0, i64 1\n"
           "  store ptr %type, ptr %s1\n"
           "  %r = call ptr @mj2x_llvm_indy_call_obj_mid(ptr %env, ptr %lookup, ptr %lkcls, "
           "ptr %mid, ptr %jv)\n"
           "  ret ptr %r\n"
           "}\n\n";

    // MethodHandle 常量（bsm Handle 参数）按 tag 分派到 Lookup.find*。
    // find* 方法名/描述符常量先于函数体提升为模块级全局（LLVM 禁止函数内
    // 常量定义）。
    const char* const handleFindNames[] = {
        "findGetter",   "findStaticGetter", "findSetter",     "findStaticSetter",
        "findVirtual",  "findStatic",       "findSpecial",    "findConstructor",
        "findVirtual",
    };
    const char* const handleFindDescs[] = {
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/"
        "MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/"
        "MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/"
        "MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/"
        "MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/"
        "invoke/MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/"
        "invoke/MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)"
        "Ljava/lang/invoke/MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/"
        "invoke/MethodHandle;",
    };
    static const char* const handleLabels[] = {"getfield", "getstatic", "putfield",
                                               "putstatic", "invokevirtual", "invokestatic",
                                               "invokespecial", "newinvokespecial",
                                               "invokeinterface"};
    for (int i = 0; i < 9; ++i) {
        out << llvmConstantString(std::string("_mj2x_llvm_indy_") + handleLabels[i] + "_fn",
                                  handleFindNames[i]);
        out << llvmConstantString(std::string("_mj2x_llvm_indy_") + handleLabels[i] + "_fd",
                                  handleFindDescs[i]);
    }
    out << "define internal ptr @mj2x_llvm_indy_handle(ptr %env, ptr %lookup, ptr "
           "%owner_class, i32 %tag, ptr %owner, ptr %name, ptr %desc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %lkcls = call ptr %find(ptr %env, ptr @_mj2x_llvm_indy_Lookup)\n"
           "  %ocls = call ptr %find(ptr %env, ptr %owner)\n"
           "  switch i32 %tag, label %fail [\n"
           "    i32 1, label %getfield\n"
           "    i32 2, label %getstatic\n"
           "    i32 3, label %putfield\n"
           "    i32 4, label %putstatic\n"
           "    i32 5, label %invokevirtual\n"
           "    i32 6, label %invokestatic\n"
           "    i32 7, label %invokespecial\n"
           "    i32 8, label %newinvokespecial\n"
           "    i32 9, label %invokeinterface\n"
           "  ]\n"
           "fail:\n"
           "  ret ptr null\n";
    static const int fieldCaseIdx[] = {0, 1, 2, 3};   // getfield/getstatic/putfield/putstatic
    for (const int idx : fieldCaseIdx) {
        out << handleLabels[idx] << ":\n"
            << "  %" << handleLabels[idx] << "r = call ptr @mj2x_llvm_indy_field_handle(ptr "
               "%env, ptr %lkcls, ptr %lookup, ptr %ocls, ptr %name, ptr %desc, ptr @_mj2x_llvm_"
            << "indy_" << handleLabels[idx] << "_fn, ptr @_mj2x_llvm_indy_" << handleLabels[idx]
            << "_fd)\n"
            << "  ret ptr %" << handleLabels[idx] << "r\n";
    }
    static const int method3CaseIdx[] = {4, 5, 8};   // invokevirtual/invokestatic/invokeinterface
    for (const int idx : method3CaseIdx) {
        out << handleLabels[idx] << ":\n"
            << "  %" << handleLabels[idx] << "_t = call ptr @mj2x_llvm_indy_method_type(ptr "
               "%env, ptr %owner_class, ptr %desc)\n"
            << "  %" << handleLabels[idx] << "r = call ptr @mj2x_llvm_indy_method_handle3(ptr "
               "%env, ptr %lkcls, ptr %lookup, ptr %ocls, ptr %name, ptr %" << handleLabels[idx]
            << "_t, ptr @_mj2x_llvm_indy_" << handleLabels[idx] << "_fn, ptr @_mj2x_llvm_indy_"
            << handleLabels[idx] << "_fd)\n"
            << "  ret ptr %" << handleLabels[idx] << "r\n";
    }
    // invokespecial：findSpecial(Class, String, MethodType, Class)。
    out << "invokespecial:\n"
           "  %invokespecial_t = call ptr @mj2x_llvm_indy_method_type(ptr %env, ptr "
           "%owner_class, ptr %desc)\n"
           "  %invokespecial_r = call ptr @mj2x_llvm_indy_method_handle4(ptr %env, ptr "
           "%lkcls, ptr %lookup, ptr %ocls, ptr %name, ptr %invokespecial_t, ptr "
           "%owner_class, ptr @_mj2x_llvm_indy_invokespecial_fn, ptr "
           "@_mj2x_llvm_indy_invokespecial_fd)\n"
           "  ret ptr %invokespecial_r\n";
    // newinvokespecial：findConstructor(Class, MethodType)。
    out << "newinvokespecial:\n"
           "  %newinvokespecial_t = call ptr @mj2x_llvm_indy_method_type(ptr %env, ptr "
           "%owner_class, ptr %desc)\n"
           "  %newinvokespecial_r = call ptr @mj2x_llvm_indy_method_handle2(ptr %env, ptr "
           "%lkcls, ptr %lookup, ptr %ocls, ptr %newinvokespecial_t, ptr "
           "@_mj2x_llvm_indy_newinvokespecial_fn, ptr @_mj2x_llvm_indy_newinvokespecial_fd)\n"
           "  ret ptr %newinvokespecial_r\n"
           "}\n\n";

    // 装箱/拆箱 helper（indy 参数/Object[] 承载；窄类型按包装类 valueOf
    // 语义）。box/unbox:#894 mj2x-llvmrt 阶段四c 下沉 mj2x-rt(valueOf/
    // XxxValue 方法名与描述符常量随实现走),模块只携带 declare。
    struct BoxKindInfo
    {
        const char* kind;     // helper 后缀
        const char* inType;   // 输入类型（计算位宽）
        const char* retType;  // 返回类型（窄提升后）
    };
    static const BoxKindInfo boxKinds[] = {
        {"boolean", "i32", "i32"},
        {"byte", "i32", "i32"},
        {"char", "i32", "i32"},
        {"short", "i32", "i32"},
        {"int", "i32", "i32"},
        {"long", "i64", "i64"},
        {"float", "float", "float"},
        {"double", "double", "double"},
    };
    for (const auto& k : boxKinds) {
        out << "declare ptr @mj2x_llvm_box_" << k.kind << "(ptr %env, " << k.inType << " %v)\n\n";
        out << "declare " << k.retType << " @mj2x_llvm_unbox_" << k.kind
            << "(ptr %env, ptr %obj)\n\n";
    }
    return out.str();
}

/// @brief 单个 invokedynamic 站点的 bootstrap 函数（对齐 RegC emitIndyBootstrapFunction）：
/// Lookup.lookup() + privateLookupIn（尽力而为）+ MethodType.fromMethodDescriptorString +
/// bsm 静态调用（常量参数物化：String/MethodType/Class/Handle/装箱原语/varargs 数组）+
/// CallSite.getTarget + 全局引用缓存。
std::string emitIndyBootstrapFunction(size_t index, const RegFunction::InvokeDynamicSite& site)
{
    const std::string  siteTag = std::to_string(index);
    std::ostringstream out;
    std::ostringstream consts;   // 站点级常量串全局（函数文本之后统一输出）
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_name", site.name);
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_desc", site.desc);
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_lookup_cls",
                                 "java/lang/invoke/MethodHandles");
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_lookup_m", "lookup");
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_lookup_d",
                                 "()Ljava/lang/invoke/MethodHandles$Lookup;");
    consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_pli_m", "privateLookupIn");
    consts << llvmConstantString(
        "_mj2x_llvm_indy_s" + siteTag + "_pli_d",
        "(Ljava/lang/Class;Ljava/lang/invoke/MethodHandles$Lookup;)Ljava/lang/invoke/"
        "MethodHandles$Lookup;");
    out << "@_mj2x_llvm_indy_cache_" << siteTag << " = internal global ptr null\n"
        << "define internal ptr @mj2x_llvm_indy_bsm_" << siteTag
        << "(ptr %env, ptr %owner_name) {\n"
           "entry:\n"
           "  %c0 = load ptr, ptr @_mj2x_llvm_indy_cache_" << siteTag << "\n"
        << "  %isnull0 = icmp eq ptr %c0, null\n"
           "  br i1 %isnull0, label %resolve, label %cached\n"
           "cached:\n"
           "  %ftc = load ptr, ptr %env\n"
        << "  %nlc = getelementptr ptr, ptr %ftc, i64 " << jni_slot::NewLocalRef << "\n"
           "  %nlf = load ptr, ptr %nlc\n"
           "  %local = call ptr %nlf(ptr %env, ptr %c0)\n"
           "  %isnullc = icmp eq ptr %local, null\n"
           "  br i1 %isnullc, label %invalidate, label %cached_ok\n"
           "invalidate:\n"
           "  store ptr null, ptr @_mj2x_llvm_indy_cache_" << siteTag << "\n"
        << "  br label %resolve\n"   // 缓存失效（≈OOM）：重走解析，而非返回已知 null 的 %local
           "cached_ok:\n"
           "  ret ptr %local\n"
           "resolve:\n"
           "  %owner = call ptr @mj2x_llvm_class_const(ptr %env, ptr %owner_name)\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find0 = load ptr, ptr %fc0\n"
           "  %mhcls = call ptr %find0(ptr %env, ptr @_mj2x_llvm_indy_s" << siteTag
        << "_lookup_cls)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %gsm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetStaticMethodID << "\n"
        << "  %getsm = load ptr, ptr %gsm0\n"
           "  %lookup_mid = call ptr %getsm(ptr %env, ptr %mhcls, ptr @_mj2x_llvm_indy_s"
        << siteTag << "_lookup_m, ptr @_mj2x_llvm_indy_s" << siteTag << "_lookup_d)\n"
           "  %lookup0 = call ptr @mj2x_llvm_indy_call_static_mid(ptr %env, ptr %mhcls, ptr "
           "%lookup_mid, ptr null)\n"
           "  %ec0 = call i1 @mj2x_llvm_exception_check(ptr %env)\n"
           "  br i1 %ec0, label %fail, label %lookup_ok\n"
           "lookup_ok:\n"
           "  %lookup_slot = alloca ptr\n"
           "  store ptr %lookup0, ptr %lookup_slot\n"
           "  %pli_mid = call ptr %getsm(ptr %env, ptr %mhcls, ptr @_mj2x_llvm_indy_s"
        << siteTag << "_pli_m, ptr @_mj2x_llvm_indy_s" << siteTag << "_pli_d)\n"
           "  %ec1 = call i1 @mj2x_llvm_exception_check(ptr %env)\n"
           "  br i1 %ec1, label %plifail, label %pli_try\n"
           "plifail:\n"
           "  call void @mj2x_llvm_exception_clear(ptr %env)\n"
           "  br label %pli_after\n"
           "pli_try:\n"
           "  %jv1 = alloca [2 x { i64 }]\n"
           "  %t0 = getelementptr [2 x { i64 }], ptr %jv1, i64 0, i64 0\n"
           "  store ptr %owner, ptr %t0\n"
           "  %t1 = getelementptr [2 x { i64 }], ptr %jv1, i64 0, i64 1\n"
           "  %lookup1 = load ptr, ptr %lookup_slot\n"
           "  store ptr %lookup1, ptr %t1\n"
           "  %pli = call ptr @mj2x_llvm_indy_call_static_mid(ptr %env, ptr %mhcls, ptr "
           "%pli_mid, ptr %jv1)\n"
           "  %ec2 = call i1 @mj2x_llvm_exception_check(ptr %env)\n"
           "  br i1 %ec2, label %plifail2, label %pli_after\n"
           "plifail2:\n"
           "  call void @mj2x_llvm_exception_clear(ptr %env)\n"
           "  br label %pli_after\n"
           "pli_after:\n"
           "  %lookup2 = load ptr, ptr %lookup_slot\n"
           "  %name = call ptr @mj2x_llvm_string_const(ptr %env, ptr @_mj2x_llvm_indy_s"
        << siteTag << "_name)\n"
           "  %type = call ptr @mj2x_llvm_indy_method_type(ptr %env, ptr %owner, ptr "
           "@_mj2x_llvm_indy_s" << siteTag << "_desc)\n";
    if (!site.bsm) {
        // 无 bootstrap 元数据：抛 RuntimeException 并返回 null（对齐 RegC）。
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_rt",
                                     "java/lang/RuntimeException");
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_rtmsg",
                                     "invokedynamic without bootstrap metadata");
        out << "  %ftb = load ptr, ptr %env\n"
               "  %fcb = getelementptr ptr, ptr %ftb, i64 " << jni_slot::FindClass << "\n"
            << "  %findb = load ptr, ptr %fcb\n"
               "  %rtcls = call ptr %findb(ptr %env, ptr @_mj2x_llvm_indy_s" << siteTag
            << "_rt)\n"
               "  %ftc2 = load ptr, ptr %env\n"
               "  %tnb = getelementptr ptr, ptr %ftc2, i64 " << jni_slot::ThrowNew << "\n"
            << "  %tnf = load ptr, ptr %tnb\n"
               "  call i32 %tnf(ptr %env, ptr %rtcls, ptr @_mj2x_llvm_indy_s" << siteTag
            << "_rtmsg)\n"
               "  ret ptr null\n"
               "fail:\n"
               "  ret ptr null\n";
    }
    else {
        const auto&       bsm      = *site.bsm;
        const std::string bsmOwner = bsm.getOwner().to_std_string();
        const std::string bsmName  = bsm.getName().to_std_string();
        const std::string bsmDesc  = bsm.getDesc().to_std_string();
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_bsm_owner", bsmOwner);
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_bsm_name", bsmName);
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_bsm_desc", bsmDesc);
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_cs_cls",
                                     "java/lang/invoke/CallSite");
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_cs_m", "getTarget");
        consts << llvmConstantString("_mj2x_llvm_indy_s" + siteTag + "_cs_d",
                                     "()Ljava/lang/invoke/MethodHandle;");
        out << "  %bsmcls = call ptr @mj2x_llvm_class_const(ptr %env, ptr @_mj2x_llvm_indy_s"
            << siteTag << "_bsm_owner)\n"
               "  %bsmmid = call ptr %getsm(ptr %env, ptr %bsmcls, ptr @_mj2x_llvm_indy_s"
            << siteTag << "_bsm_name, ptr @_mj2x_llvm_indy_s" << siteTag << "_bsm_desc)\n";
        const auto bsmArgTypes =
            Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(bsmDesc.c_str()));
        const size_t formalCount = bsmArgTypes.size();
        out << "  %jv2 = alloca [" << formalCount << " x { i64 }]\n";
        size_t bsmArgIndex = 0;
        for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
            const std::string slot = "  %u" + std::to_string(i) +
                                     " = getelementptr [" + std::to_string(formalCount) +
                                     " x { i64 }], ptr %jv2, i64 0, i64 " + std::to_string(i) +
                                     "\n";
            if (i == 0) {
                out << slot << "  %lookup3 = load ptr, ptr %lookup_slot\n"
                    << "  store ptr %lookup3, ptr %u" << i << "\n";
                continue;
            }
            if (i == 1) {
                out << slot << "  store ptr %name, ptr %u" << i << "\n";
                continue;
            }
            if (i == 2) {
                out << slot << "  store ptr %type, ptr %u" << i << "\n";
                continue;
            }
            const auto& formal = bsmArgTypes[i];
            const bool  isTrailingObjectArray =
                i + 1 == bsmArgTypes.size() && formal.getSort() == Type::ARRAY &&
                indyTypeText(formal) == "[Ljava/lang/Object;";
            if (isTrailingObjectArray) {
                const size_t remaining =
                    bsmArgIndex < site.bsmArgs.size() ? site.bsmArgs.size() - bsmArgIndex : 0;
                out << "  %objcls = call ptr @mj2x_llvm_class_const(ptr %env, ptr "
                       "@_mj2x_llvm_indy_java_lang_Object)\n";
                out << "  %varargs = call ptr @mj2x_llvm_new_object_array(ptr %env, i32 "
                    << remaining << ", ptr %objcls)\n";
                for (size_t j = 0; j < remaining; ++j) {
                    const std::string expr = indyBsmArgExpr(siteTag, site.bsmArgs[bsmArgIndex + j],
                                                            consts, bsmArgIndex + j);
                    if (expr.empty()) {
                        out << "  call void @mj2x_llvm_set_object_array_element(ptr %env, ptr "
                               "%varargs, i32 " << j << ", ptr null)\n";
                    }
                    else {
                        const std::string argVar = "%vararg_" + std::to_string(j);
                        out << "  " << argVar << " = " << expr << "\n";
                        out << "  call void @mj2x_llvm_set_object_array_element(ptr %env, ptr "
                               "%varargs, i32 " << j << ", ptr " << argVar << ")\n";
                    }
                }
                bsmArgIndex = site.bsmArgs.size();
                out << slot << "  store ptr %varargs, ptr %u" << i << "\n";
                continue;
            }
            if (bsmArgIndex >= site.bsmArgs.size()) {
                out << slot << "  store ptr null, ptr %u" << i << "\n";
                continue;
            }
            const std::string expr =
                indyBsmArgExpr(siteTag, site.bsmArgs[bsmArgIndex], consts, bsmArgIndex);
            if (expr.empty()) {
                out << slot << "  store ptr null, ptr %u" << i << "\n";
            }
            else {
                const std::string argVar = "%bsmarg_" + std::to_string(bsmArgIndex);
                out << "  " << argVar << " = " << expr << "\n";
                out << slot << "  store ptr " << argVar << ", ptr %u" << i << "\n";
            }
            ++bsmArgIndex;
        }
        out << "  %callsite = call ptr @mj2x_llvm_indy_call_static_mid(ptr %env, ptr %bsmcls, "
               "ptr %bsmmid, ptr "
            << (formalCount == 0 ? "null" : "%jv2") << ")\n"
               "  %ec3 = call i1 @mj2x_llvm_exception_check(ptr %env)\n"
               "  br i1 %ec3, label %fail, label %callsite_ok\n"
               "callsite_ok:\n"
               "  %target = call ptr @mj2x_llvm_call_object(ptr %env, ptr %callsite, ptr "
               "@_mj2x_llvm_indy_s" << siteTag << "_cs_cls, ptr @_mj2x_llvm_indy_s" << siteTag
            << "_cs_m, ptr @_mj2x_llvm_indy_s" << siteTag << "_cs_d, ptr null)\n"
               "  %ec4 = call i1 @mj2x_llvm_exception_check(ptr %env)\n"
               "  br i1 %ec4, label %fail, label %cache\n"
               "cache:\n"
               "  %ftg = load ptr, ptr %env\n"
            << "  %ngr = getelementptr ptr, ptr %ftg, i64 " << jni_slot::NewGlobalRef << "\n"
               "  %ngrf = load ptr, ptr %ngr\n"
               "  %gref = call ptr %ngrf(ptr %env, ptr %target)\n"
               "  %isnullg = icmp eq ptr %gref, null\n"
               "  br i1 %isnullg, label %cache_ok, label %cache_store\n"
               "cache_store:\n"
               "  store ptr %gref, ptr @_mj2x_llvm_indy_cache_" << siteTag << "\n"
            << "  br label %cache_ok\n"
               "cache_ok:\n"
               "  ret ptr %target\n"
               "fail:\n"
               "  ret ptr null\n";
    }
    out << "}\n\n";
    out << consts.str();
    return out.str();
}

std::string emitRuntimeProtectionDeclarations()
{
    return "%mj2x_rt_checkpoint = type { i32, i32, i32 }\n"
           "declare i32 @mj2x_rt_checkpoint_run(ptr, i32, i32, i32, i32)\n";
}

}   // namespace llvmir
}   // namespace mj2x
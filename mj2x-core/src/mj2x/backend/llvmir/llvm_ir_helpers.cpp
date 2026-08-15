/// @file llvm_ir_helpers.cpp
/// @brief LLVM IR 后端共享 JNI helper 发射：异常/对象/字段/数组/调用/monitor 辅助函数。
#include "llvm_ir_internal.hpp"

namespace mj2x {
namespace llvmir {

using namespace jbytecode::core;
using namespace jbytecode::tree;


std::string emitThrowArithmeticHelper()
{
    return "@_mj2x_llvm_str_arith_ex = private unnamed_addr constant [30 x i8] "
           "c\"java/lang/ArithmeticException\\00\"\n"
           "@_mj2x_llvm_str_div_by_zero = private unnamed_addr constant [10 x i8] "
           "c\"/ by zero\\00\"\n\n"
           "define internal void @mj2x_llvm_throw_arith(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 6\n"
           "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr @_mj2x_llvm_str_arith_ex)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft1, i64 14\n"
           "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_str_div_by_zero)\n"
           "  ret void\n"
           "}\n\n";
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
        int64_t     getSlot;    // Get*Field
        int64_t     getStaticSlot;
        int64_t     setSlot;    // Set*Field
        int64_t     setStaticSlot;
        const char* jniType;    // JNI 函数声明类型
        const char* retType;    // helper 返回类型（窄提升后）
        const char* ext;        // 窄扩展（sext/zext/空）
    };
    static const FieldKindInfo kinds[] = {
        {"bool", jni_slot::GetBooleanField, jni_slot::GetStaticBooleanField,
         jni_slot::SetBooleanField, jni_slot::SetStaticBooleanField, "i8", "i32", "zext"},
        {"byte", jni_slot::GetByteField, jni_slot::GetStaticByteField, jni_slot::SetByteField,
         jni_slot::SetStaticByteField, "i8", "i32", "sext"},
        {"char", jni_slot::GetCharField, jni_slot::GetStaticCharField, jni_slot::SetCharField,
         jni_slot::SetStaticCharField, "i16", "i32", "zext"},
        {"short", jni_slot::GetShortField, jni_slot::GetStaticShortField,
         jni_slot::SetShortField, jni_slot::SetStaticShortField, "i16", "i32", "sext"},
        {"int", jni_slot::GetIntField, jni_slot::GetStaticIntField, jni_slot::SetIntField,
         jni_slot::SetStaticIntField, "i32", "i32", ""},
        {"long", jni_slot::GetLongField, jni_slot::GetStaticLongField, jni_slot::SetLongField,
         jni_slot::SetStaticLongField, "i64", "i64", ""},
        {"float", jni_slot::GetFloatField, jni_slot::GetStaticFloatField,
         jni_slot::SetFloatField, jni_slot::SetStaticFloatField, "float", "float", ""},
        {"double", jni_slot::GetDoubleField, jni_slot::GetStaticDoubleField,
         jni_slot::SetDoubleField, jni_slot::SetStaticDoubleField, "double", "double", ""},
        {"ref", jni_slot::GetObjectField, jni_slot::GetStaticObjectField,
         jni_slot::SetObjectField, jni_slot::SetStaticObjectField, "ptr", "ptr", ""},
    };

    std::ostringstream out;
    const char* npeName = "java/lang/NullPointerException";
    const char* cceName = "java/lang/ClassCastException";
    const char* nullMsg = "null";
    const auto zeroFor = [](const char* retType) -> const char* {
        const std::string t(retType);
        if (t == "float" || t == "double") return "0.0";
        if (t == "ptr") return "null";
        return "0";
    };
    // NPE / CCE 类名与消息（固定名，避开调用点字符串表）。
    out << "@_mj2x_llvm_obj_npe = private unnamed_addr constant ["
        << (std::strlen(npeName) + 1) << " x i8] c\"" << npeName << "\\00\"\n";
    out << "@_mj2x_llvm_obj_cce = private unnamed_addr constant ["
        << (std::strlen(cceName) + 1) << " x i8] c\"" << cceName << "\\00\"\n";
    out << "@_mj2x_llvm_obj_null = private unnamed_addr constant ["
        << (std::strlen(nullMsg) + 1) << " x i8] c\"" << nullMsg << "\\00\"\n\n";

    // throw_null：NPE。
    out << "define internal void @mj2x_llvm_throw_null(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";

    // new_object：FindClass + AllocObject（不调用 <init>；构造器由 invokespecial 处理）。
    out << "define internal ptr @mj2x_llvm_new_object(ptr %env, ptr %name) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %name)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ao0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::AllocObject << "\n"
        << "  %allocobject = load ptr, ptr %ao0\n"
           "  %obj = call ptr %allocobject(ptr %env, ptr %cls)\n"
           "  ret ptr %obj\n"
           "}\n\n";

    // new_array：按元素描述符首字符分发 JNI NewXxxArray / NewObjectArray。
    out << "define internal ptr @mj2x_llvm_new_array(ptr %env, ptr %desc, i32 %count) {\n"
           "entry:\n"
           "  %c1 = getelementptr i8, ptr %desc, i64 1\n"
           "  %t = load i8, ptr %c1\n"
           "  switch i8 %t, label %obj [\n"
           "    i8 90, label %bool_\n"
           "    i8 66, label %byte_\n"
           "    i8 67, label %char_\n"
           "    i8 83, label %short_\n"
           "    i8 73, label %int_\n"
           "    i8 74, label %long_\n"
           "    i8 70, label %float_\n"
           "    i8 68, label %double_\n"
           "  ]\n";
    const struct { const char* label; int64_t slot; } primArrays[] = {
        {"bool_", jni_slot::NewBooleanArray}, {"byte_", jni_slot::NewByteArray},
        {"char_", jni_slot::NewCharArray},   {"short_", jni_slot::NewShortArray},
        {"int_", jni_slot::NewIntArray},      {"long_", jni_slot::NewLongArray},
        {"float_", jni_slot::NewFloatArray},  {"double_", jni_slot::NewDoubleArray},
    };
    for (const auto& entry : primArrays) {
        out << entry.label << ":\n"
            << "  %ft" << entry.label << " = load ptr, ptr %env\n"
            << "  %fn" << entry.label << " = getelementptr ptr, ptr %ft" << entry.label
            << ", i64 " << entry.slot << "\n"
            << "  %f" << entry.label << " = load ptr, ptr %fn" << entry.label << "\n"
            << "  %a" << entry.label << " = call ptr %f" << entry.label
            << "(ptr %env, i32 %count)\n"
            << "  ret ptr %a" << entry.label << "\n";
    }
    out << "obj:\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %fc2 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc2\n"
           "  %cls = call ptr %find(ptr %env, ptr %c1)\n"
           "  %ft3 = load ptr, ptr %env\n"
           "  %na0 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::NewObjectArray << "\n"
        << "  %naf = load ptr, ptr %na0\n"
           "  %arr = call ptr %naf(ptr %env, i32 %count, ptr %cls, ptr null)\n"
           "  ret ptr %arr\n"
           "}\n\n";

    // new_multi_array：NewObjectArray 递归（JVM EagerAlloc 语义）。
    out << "define internal ptr @mj2x_llvm_new_multi_array(ptr %env, ptr %desc, ptr %dims, "
           "i32 %ndims) {\n"
           "entry:\n"
           "  %d0p = getelementptr i32, ptr %dims, i64 0\n"
           "  %d0 = load i32, ptr %d0p\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %edesc = getelementptr i8, ptr %desc, i64 1\n"
           "  %cls = call ptr %find(ptr %env, ptr %edesc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %na0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::NewObjectArray << "\n"
        << "  %naf = load ptr, ptr %na0\n"
           "  %arr = call ptr %naf(ptr %env, i32 %d0, ptr %cls, ptr null)\n"
           "  %is1 = icmp eq i32 %ndims, 1\n"
           "  br i1 %is1, label %done, label %fill\n"
           "fill:\n"
           "  %dims1 = getelementptr i32, ptr %dims, i64 1\n"
           "  %nd1 = sub i32 %ndims, 1\n"
           "  br label %loop\n"
           "loop:\n"
           "  %i = phi i32 [ 0, %fill ], [ %inext, %loopnext ]\n"
           "  %c = icmp slt i32 %i, %d0\n"
           "  br i1 %c, label %body, label %done\n"
           "body:\n"
           "  %elem = call ptr @mj2x_llvm_new_multi_array(ptr %env, ptr %edesc, ptr %dims1, "
           "i32 %nd1)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %se0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::SetObjectArrayElement << "\n"
        << "  %sef = load ptr, ptr %se0\n"
           "  call void %sef(ptr %env, ptr %arr, i32 %i, ptr %elem)\n"
           "  %inext = add i32 %i, 1\n"
           "  br label %loopnext\n"
           "loopnext:\n"
           "  br label %loop\n"
           "done:\n"
           "  ret ptr %arr\n"
           "}\n\n";

    // 字段 load/store helpers（static/instance 合一 + null 检查）。
    for (const auto& k : kinds) {
        const std::string getFn = "get" + std::string(k.kind) + "field";
        const std::string getStaticFn = "getstatic" + std::string(k.kind) + "field";
        out << "define internal " << k.retType << " @mj2x_llvm_field_load_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %findclass = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %findclass(ptr %env, ptr %owner)\n"
            << "  %isst = icmp eq i32 %is_static, 0\n"
            << "  br i1 %isst, label %inst, label %stat\n"
            << "inst:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %inst_ok\n"
            << "inst_ok:\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gf1 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetFieldID << "\n"
            << "  %getfid = load ptr, ptr %gf1\n"
            << "  %fid = call ptr %getfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %gi1 = getelementptr ptr, ptr %ft2, i64 " << k.getSlot << "\n"
            << "  %gv = load ptr, ptr %gi1\n"
            << "  %v = call " << k.jniType << " %gv(ptr %env, ptr %rcv, ptr %fid)\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w = " << k.ext << " " << k.jniType << " %v to " << k.retType << "\n"
                << "  ret " << k.retType << " %w\n";
        }
        else {
            out << "  ret " << k.retType << " %v\n";
        }
        out << "stat:\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gs1 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::GetStaticFieldID << "\n"
            << "  %getstatfid = load ptr, ptr %gs1\n"
            << "  %fid2 = call ptr %getstatfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft4 = load ptr, ptr %env\n"
            << "  %gsi1 = getelementptr ptr, ptr %ft4, i64 " << k.getStaticSlot << "\n"
            << "  %gsv = load ptr, ptr %gsi1\n"
            << "  %v2 = call " << k.jniType << " %gsv(ptr %env, ptr %cls, ptr %fid2)\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w2 = " << k.ext << " " << k.jniType << " %v2 to " << k.retType << "\n"
                << "  ret " << k.retType << " %w2\n";
        }
        else {
            out << "  ret " << k.retType << " %v2\n";
        }
        out << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret " << k.retType << " " << zeroFor(k.retType) << "\n"
            << "}\n\n";
        // store。
        out << "define internal void @mj2x_llvm_field_store_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static, "
            << k.retType << " %val) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %findclass = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %findclass(ptr %env, ptr %owner)\n"
            << "  %isst = icmp eq i32 %is_static, 0\n"
            << "  br i1 %isst, label %inst, label %stat\n"
            << "inst:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %inst_ok\n"
            << "inst_ok:\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gf1 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetFieldID << "\n"
            << "  %getfid = load ptr, ptr %gf1\n"
            << "  %fid = call ptr %getfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %si1 = getelementptr ptr, ptr %ft2, i64 " << k.setSlot << "\n"
            << "  %sv = load ptr, ptr %si1\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  call void %sv(ptr %env, ptr %rcv, ptr %fid, " << k.jniType << " %tv)\n";
        }
        else {
            out << "  call void %sv(ptr %env, ptr %rcv, ptr %fid, " << k.jniType << " %val)\n";
        }
        out << "  ret void\n"
            << "stat:\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gs1 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::GetStaticFieldID << "\n"
            << "  %getstatfid = load ptr, ptr %gs1\n"
            << "  %fid2 = call ptr %getstatfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft4 = load ptr, ptr %env\n"
            << "  %ssi1 = getelementptr ptr, ptr %ft4, i64 " << k.setStaticSlot << "\n"
            << "  %ssv = load ptr, ptr %ssi1\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv2 = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  call void %ssv(ptr %env, ptr %cls, ptr %fid2, " << k.jniType << " %tv2)\n";
        }
        else {
            out << "  call void %ssv(ptr %env, ptr %cls, ptr %fid2, " << k.jniType << " %val)\n";
        }
        out << "  ret void\n"
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret void\n"
            << "}\n\n";
    }

    // 数组元素读写 helper：null → NPE；越界由 JNI Get/Set*ArrayRegion /
    // Get/SetObjectArrayElement 抛 ArrayIndexOutOfBoundsException / 存储违例抛
    // ArrayStoreException。byte 形态特判 boolean[]（JNI 要求 GetBooleanArrayRegion）。
    out << "@_mj2x_llvm_obj_zarr = private unnamed_addr constant [3 x i8] c\"[Z\\00\"\n\n";
    struct ArrayKindInfo
    {
        const char* kind;      // helper 后缀
        SSAType     ssaType;   // 元素 SSA 类型
        int64_t     getSlot;
        int64_t     setSlot;
        const char* jniType;   // region 元素类型
        const char* retType;   // helper 返回类型（窄提升后）
        const char* ext;       // 窄扩展（sext/zext/空）
    };
    static const ArrayKindInfo arrayKinds[] = {
        {"byte", SSAType::I8, jni_slot::GetByteArrayRegion, jni_slot::SetByteArrayRegion, "i8",
         "i32", "sext"},
        {"short", SSAType::I16, jni_slot::GetShortArrayRegion, jni_slot::SetShortArrayRegion,
         "i16", "i32", "sext"},
        {"char", SSAType::CHAR, jni_slot::GetCharArrayRegion, jni_slot::SetCharArrayRegion,
         "i16", "i32", "zext"},
        {"int", SSAType::I32, jni_slot::GetIntArrayRegion, jni_slot::SetIntArrayRegion, "i32",
         "i32", ""},
        {"long", SSAType::I64, jni_slot::GetLongArrayRegion, jni_slot::SetLongArrayRegion,
         "i64", "i64", ""},
        {"float", SSAType::F32, jni_slot::GetFloatArrayRegion, jni_slot::SetFloatArrayRegion,
         "float", "float", ""},
        {"double", SSAType::F64, jni_slot::GetDoubleArrayRegion, jni_slot::SetDoubleArrayRegion,
         "double", "double", ""},
        {"ref", SSAType::REF, jni_slot::GetObjectArrayElement, jni_slot::SetObjectArrayElement,
         "ptr", "ptr", ""},
    };
    for (const auto& k : arrayKinds) {
        const bool isByte = std::string(k.kind) == "byte";
        const bool isRef  = std::string(k.kind) == "ref";
        out << "define internal " << k.retType << " @mj2x_llvm_array_load_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %arr, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n";
        if (isByte) {
            // boolean[] 特判：GetBooleanArrayRegion（jboolean i8 0/1 → zext）。
            out << "  %ft0 = load ptr, ptr %env\n"
                << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
                << "  %find0 = load ptr, ptr %fc0\n"
                << "  %zcls = call ptr %find0(ptr %env, ptr @_mj2x_llvm_obj_zarr)\n"
                << "  %ft1 = load ptr, ptr %env\n"
                << "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
                << "  %isinst0 = load ptr, ptr %ii0\n"
                << "  %isbool = call i32 %isinst0(ptr %env, ptr %arr, ptr %zcls)\n"
                << "  %b = icmp ne i32 %isbool, 0\n"
                << "  br i1 %b, label %boolarr, label %bytearr\n"
                << "boolarr:\n"
                << "  %tmpb = alloca i8\n"
                << "  %ft2 = load ptr, ptr %env\n"
                << "  %gb0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::GetBooleanArrayRegion
                << "\n"
                << "  %gb = load ptr, ptr %gb0\n"
                << "  call void %gb(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmpb)\n"
                << "  %vb = load i8, ptr %tmpb\n"
                << "  %wb = zext i8 %vb to i32\n"
                << "  ret i32 %wb\n"
                << "bytearr:\n";
        }
        if (isRef) {
            // 引用数组：GetObjectArrayElement 直接返回元素。
            out << "  %ft3 = load ptr, ptr %env\n"
                << "  %gr0 = getelementptr ptr, ptr %ft3, i64 " << k.getSlot << "\n"
                << "  %gr = load ptr, ptr %gr0\n"
                << "  %v = call ptr %gr(ptr %env, ptr %arr, i32 %idx)\n"
                << "  ret ptr %v\n"
                << "npe:\n"
                << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
                << "  ret ptr null\n"
                << "}\n\n";
            // store 分支在下方循环体内（isRef 特判）。
        }
        else {
            out << "  %tmp = alloca " << k.jniType << "\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gr0 = getelementptr ptr, ptr %ft3, i64 " << k.getSlot << "\n"
            << "  %gr = load ptr, ptr %gr0\n"
            << "  call void %gr(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmp)\n"
            << "  %v = load " << k.jniType << ", ptr %tmp\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w = " << k.ext << " " << k.jniType << " %v to " << k.retType << "\n"
                << "  ret " << k.retType << " %w\n";
        }
        else {
            out << "  ret " << k.retType << " %v\n";
        }
        out << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret " << k.retType << " " << zeroFor(k.retType) << "\n"
            << "}\n\n";
        }
        // store。
        out << "define internal void @mj2x_llvm_array_store_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx, " << k.retType << " %val) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %arr, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n";
        if (isByte) {
            out << "  %ft0 = load ptr, ptr %env\n"
                << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
                << "  %find0 = load ptr, ptr %fc0\n"
                << "  %zcls = call ptr %find0(ptr %env, ptr @_mj2x_llvm_obj_zarr)\n"
                << "  %ft1 = load ptr, ptr %env\n"
                << "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
                << "  %isinst0 = load ptr, ptr %ii0\n"
                << "  %isbool = call i32 %isinst0(ptr %env, ptr %arr, ptr %zcls)\n"
                << "  %b = icmp ne i32 %isbool, 0\n"
                << "  br i1 %b, label %boolarr, label %bytearr\n"
                << "boolarr:\n"
                << "  %tvb = trunc i32 %val to i8\n"
                << "  %nzb = icmp ne i8 %tvb, 0\n"
                << "  %oneb = zext i1 %nzb to i8\n"
                << "  %tmpb = alloca i8\n"
                << "  store i8 %oneb, ptr %tmpb\n"
                << "  %ft2 = load ptr, ptr %env\n"
                << "  %sb0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::SetBooleanArrayRegion
                << "\n"
                << "  %sb = load ptr, ptr %sb0\n"
                << "  call void %sb(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmpb)\n"
                << "  ret void\n"
                << "bytearr:\n";
        }
        if (isRef) {
            // 引用数组：Get/SetObjectArrayElement 直接传元素（无 region 缓冲）。
            out << "  %ft3 = load ptr, ptr %env\n"
                << "  %sr0 = getelementptr ptr, ptr %ft3, i64 " << k.setSlot << "\n"
                << "  %sr = load ptr, ptr %sr0\n"
                << "  call void %sr(ptr %env, ptr %arr, i32 %idx, ptr %val)\n"
                << "  ret void\n"
                << "npe:\n"
                << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
                << "  ret void\n"
                << "}\n\n";
            continue;
        }
        out << "  %tmp = alloca " << k.jniType << "\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  store " << k.jniType << " %tv, ptr %tmp\n";
        }
        else {
            out << "  store " << k.jniType << " %val, ptr %tmp\n";
        }
        out << "  %ft3 = load ptr, ptr %env\n"
            << "  %sr0 = getelementptr ptr, ptr %ft3, i64 " << k.setSlot << "\n"
            << "  %sr = load ptr, ptr %sr0\n"
            << "  call void %sr(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmp)\n"
            << "  ret void\n"
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret void\n"
            << "}\n\n";
    }
    // array_length：null → NPE；GetArrayLength。
    out << "define internal i32 @mj2x_llvm_array_length(ptr %env, ptr %arr) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %arr, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %al0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetArrayLength << "\n"
        << "  %getlen = load ptr, ptr %al0\n"
           "  %len = call i32 %getlen(ptr %env, ptr %arr)\n"
           "  ret i32 %len\n"
           "npe:\n"
           "  call void @mj2x_llvm_throw_null(ptr %env)\n"
           "  ret i32 0\n"
           "}\n\n";

    // JNI 方法调用 helper：CallXxxMethodA（jvalue 数组）。
    // virtual/interface → Call<Type>MethodA；special → CallNonvirtual<Type>MethodA
    // （GetObjectClass + 显式类，用于 <init>/private/super）；static 外部 →
    // CallStatic<Type>MethodA。null receiver（实例）抛 NPE。
    struct CallKindInfo
    {
        const char* kind;
        int64_t     callSlot;       // Call*MethodA
        int64_t     nonvirtualSlot; // CallNonvirtual*MethodA
        int64_t     staticSlot;     // CallStatic*MethodA
        const char* boundary;       // JNI 返回类型
    };
    static const CallKindInfo callKinds[] = {
        {"object", 36, 66, 116, "ptr"},
        {"bool", 39, 69, 119, "i8"},
        {"byte", 42, 72, 122, "i8"},
        {"char", 45, 75, 125, "i16"},
        {"short", 48, 78, 128, "i16"},
        {"int", 51, 81, 131, "i32"},
        {"long", 54, 84, 134, "i64"},
        {"float", 57, 87, 137, "float"},
        {"double", 60, 90, 140, "double"},
        {"void", 63, 93, 143, "void"},
    };
    for (const auto& k : callKinds) {
        // virtual / interface。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %find = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %find(ptr %env, ptr %owner)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.callSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary << " (ptr, ptr, ptr, ptr) %call(ptr %env, ptr %rcv, "
               "ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << (std::string(k.boundary) == "void"
                    ? "  ret void\n"
                    : "  ret " + std::string(k.boundary) + " " + zeroFor(k.boundary) + "\n")
            << "}\n\n";
        // special（invokespecial：<init>/private/super）。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_nonvirtual_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %gc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetObjectClass << "\n"
            << "  %getcls = load ptr, ptr %gc0\n"
            << "  %rcls = call ptr %getcls(ptr %env, ptr %rcv)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %rcls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.nonvirtualSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary
            << " (ptr, ptr, ptr, ptr, ptr) %call(ptr %env, ptr %rcv, ptr %rcls, ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << (std::string(k.boundary) == "void"
                    ? "  ret void\n"
                    : "  ret " + std::string(k.boundary) + " " + zeroFor(k.boundary) + "\n")
            << "}\n\n";
        // static（外部类）。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_static_" << k.kind
            << "(ptr %env, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %find = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %find(ptr %env, ptr %owner)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetStaticMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.staticSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary
            << " (ptr, ptr, ptr, ptr) %call(ptr %env, ptr %cls, ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "}\n\n";
    }

    // check_cast：IsInstanceOf == 0 → 挂起 ClassCastException；返回原引用。
    out << "define internal ptr @mj2x_llvm_check_cast(ptr %env, ptr %obj, ptr %desc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %desc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinstanceof = load ptr, ptr %ii0\n"
           "  %ok = call i32 %isinstanceof(ptr %env, ptr %obj, ptr %cls)\n"
           "  %bad = icmp eq i32 %ok, 0\n"
           "  br i1 %bad, label %fail, label %done\n"
           "fail:\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc = call i32 %thrownew(ptr %env, ptr @_mj2x_llvm_obj_cce, ptr @_mj2x_llvm_obj_null)\n"
           "  br label %done\n"
           "done:\n"
           "  ret ptr %obj\n"
           "}\n\n";

    // instance_of：IsInstanceOf → i32 0/1（intrinsic "instanceof:<desc>"）。
    out << "define internal i32 @mj2x_llvm_instance_of(ptr %env, ptr %obj, ptr %desc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %desc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinstanceof = load ptr, ptr %ii0\n"
           "  %ok = call i32 %isinstanceof(ptr %env, ptr %obj, ptr %cls)\n"
           "  ret i32 %ok\n"
           "}\n\n";

    // 异常基础设施：ExceptionCheck/Occurred/Clear/Matches（catch 类型匹配，
    // 空串 = catch-all/finally）/Throw（athrow；null → NPE）。
    out << "define internal i1 @mj2x_llvm_exception_check(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %ec0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionCheck << "\n"
        << "  %check = load ptr, ptr %ec0\n"
           "  %c = call i32 %check(ptr %env)\n"
           "  %r = icmp ne i32 %c, 0\n"
           "  ret i1 %r\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_exception_occurred(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %eo0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionOccurred << "\n"
        << "  %occ = load ptr, ptr %eo0\n"
           "  %ex = call ptr %occ(ptr %env)\n"
           "  ret ptr %ex\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_exception_clear(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %cl0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionClear << "\n"
        << "  %clear = load ptr, ptr %cl0\n"
           "  call void %clear(ptr %env)\n"
           "  ret void\n"
           "}\n\n";
    out << "define internal i1 @mj2x_llvm_exception_matches(ptr %env, ptr %ex, ptr %catch) {\n"
           "entry:\n"
           "  %c0 = load i8, ptr %catch\n"
           "  %isempty = icmp eq i8 %c0, 0\n"
           "  br i1 %isempty, label %yes, label %cls\n"
           "cls:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %c = call ptr %find(ptr %env, ptr %catch)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinst = load ptr, ptr %ii0\n"
           "  %m = call i32 %isinst(ptr %env, ptr %ex, ptr %c)\n"
           "  %r = icmp ne i32 %m, 0\n"
           "  ret i1 %r\n"
           "yes:\n"
           "  ret i1 true\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_throw(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %th0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::Throw << "\n"
        << "  %throw = load ptr, ptr %th0\n"
           "  %rc = call i32 %throw(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";
    // String/Class 常量：NewStringUTF（modified UTF-8 字节）/ FindClass。
    out << "define internal ptr @mj2x_llvm_string_const(ptr %env, ptr %utf8) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %ns0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::NewStringUTF << "\n"
        << "  %newstr = load ptr, ptr %ns0\n"
           "  %s = call ptr %newstr(ptr %env, ptr %utf8)\n"
           "  ret ptr %s\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_class_const(ptr %env, ptr %name) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %c = call ptr %find(ptr %env, ptr %name)\n"
           "  ret ptr %c\n"
           "}\n\n";
    // MonitorEnter/Exit（null → NPE ThrowNew）。
    out << "define internal void @mj2x_llvm_monitor_enter(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %me0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::MonitorEnter << "\n"
        << "  %me = load ptr, ptr %me0\n"
           "  %rc = call i32 %me(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_monitor_exit(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %mx0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::MonitorExit << "\n"
        << "  %mx = load ptr, ptr %mx0\n"
           "  %rc = call i32 %mx(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";

    return out.str();
}

}   // namespace llvmir
}   // namespace mj2x

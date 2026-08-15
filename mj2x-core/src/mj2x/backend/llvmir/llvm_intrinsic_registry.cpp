#include "llvm_intrinsic_registry.hpp"

#include <cstring>
#include <string_view>

namespace mj2x {

namespace {

/// @brief Math 纯函数 intrinsic 切片（对齐 RegC regc_intrinsic_registry.cpp 的
/// RuntimeFunction 条目；符号实现见 mj2x-crt src/regc/mj2x_regc_intrinsic.c）。
///
/// @details 只收录 JNIEnv 无关的纯标量函数（直调 0 JNI；函数不抛异常，无需
/// ExceptionCheck——与 RegC isPureRuntimeIntrinsic 语义一致）。包装类/String/
/// StringBuilder/Random 等带 native 对象的切片保持 JNI 分派（待整类下沉）。
constexpr LlvmIntrinsicEntry kEntries[] = {
    {"java/lang/Math", "abs", "(I)I", "mj2x_math_abs_i32", "i32", {"i32"},
     "declare i32 @mj2x_math_abs_i32(i32)"},
    {"java/lang/Math", "abs", "(J)J", "mj2x_math_abs_i64", "i64", {"i64"},
     "declare i64 @mj2x_math_abs_i64(i64)"},
    {"java/lang/Math", "abs", "(F)F", "mj2x_math_abs_f32", "float", {"float"},
     "declare float @mj2x_math_abs_f32(float)"},
    {"java/lang/Math", "abs", "(D)D", "mj2x_math_abs_f64", "double", {"double"},
     "declare double @mj2x_math_abs_f64(double)"},
    {"java/lang/Math", "min", "(II)I", "mj2x_math_min_i32", "i32", {"i32", "i32"},
     "declare i32 @mj2x_math_min_i32(i32, i32)"},
    {"java/lang/Math", "max", "(II)I", "mj2x_math_max_i32", "i32", {"i32", "i32"},
     "declare i32 @mj2x_math_max_i32(i32, i32)"},
    {"java/lang/Math", "min", "(JJ)J", "mj2x_math_min_i64", "i64", {"i64", "i64"},
     "declare i64 @mj2x_math_min_i64(i64, i64)"},
    {"java/lang/Math", "max", "(JJ)J", "mj2x_math_max_i64", "i64", {"i64", "i64"},
     "declare i64 @mj2x_math_max_i64(i64, i64)"},
    {"java/lang/Math", "min", "(FF)F", "mj2x_math_min_f32", "float", {"float", "float"},
     "declare float @mj2x_math_min_f32(float, float)"},
    {"java/lang/Math", "max", "(FF)F", "mj2x_math_max_f32", "float", {"float", "float"},
     "declare float @mj2x_math_max_f32(float, float)"},
    {"java/lang/Math", "min", "(DD)D", "mj2x_math_min_f64", "double", {"double", "double"},
     "declare double @mj2x_math_min_f64(double, double)"},
    {"java/lang/Math", "max", "(DD)D", "mj2x_math_max_f64", "double", {"double", "double"},
     "declare double @mj2x_math_max_f64(double, double)"},
    {"java/lang/Math", "sin", "(D)D", "mj2x_math_sin", "double", {"double"},
     "declare double @mj2x_math_sin(double)"},
    {"java/lang/Math", "cos", "(D)D", "mj2x_math_cos", "double", {"double"},
     "declare double @mj2x_math_cos(double)"},
    {"java/lang/Math", "tan", "(D)D", "mj2x_math_tan", "double", {"double"},
     "declare double @mj2x_math_tan(double)"},
    {"java/lang/Math", "sqrt", "(D)D", "mj2x_math_sqrt", "double", {"double"},
     "declare double @mj2x_math_sqrt(double)"},
    {"java/lang/Math", "floor", "(D)D", "mj2x_math_floor", "double", {"double"},
     "declare double @mj2x_math_floor(double)"},
    {"java/lang/Math", "ceil", "(D)D", "mj2x_math_ceil", "double", {"double"},
     "declare double @mj2x_math_ceil(double)"},
    {"java/lang/Math", "pow", "(DD)D", "mj2x_math_pow", "double", {"double", "double"},
     "declare double @mj2x_math_pow(double, double)"},
    {"java/lang/Math", "round", "(D)J", "mj2x_math_round", "i64", {"double"},
     "declare i64 @mj2x_math_round(double)"},
    {"java/lang/Math", "random", "()D", "mj2x_math_random", "double", {""},
     "declare double @mj2x_math_random()"},
};

bool sameText(ca::str::Utf8StringRef ref, const char* text)
{
    return std::string_view(reinterpret_cast<const char*>(ref.data()), ref.byte_length()) ==
           std::string_view(text);
}

/// @brief 包装类装箱条目（valueOf）：shadow 结构在 mj2x-crt（mj2x_int 等单字段
/// 值结构），LLVM 侧以命名 struct 传值。窄类型装箱参数先在发射端 trunc。
///
/// @details 与 RegC regc_intrinsics.cpp WrapperValueOf 条目一一对应；仅非逃逸
/// 用法直调（逃逸根回退 JNI，见 llvm_ir_emitter 的 wrapper 分析）。
constexpr LlvmWrapperBoxEntry kBoxEntries[] = {
    {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", "int", "%mj2x_int",
     "%mj2x_int = type { i32 }", "mj2x_int_value_of", "i32",
     "declare %mj2x_int @mj2x_int_value_of(i32)"},
    {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", "long", "%mj2x_long",
     "%mj2x_long = type { i64 }", "mj2x_long_value_of", "i64",
     "declare %mj2x_long @mj2x_long_value_of(i64)"},
    {"java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", "short", "%mj2x_short",
     "%mj2x_short = type { i16 }", "mj2x_short_value_of", "i16",
     "declare %mj2x_short @mj2x_short_value_of(i16)"},
    {"java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", "byte", "%mj2x_byte",
     "%mj2x_byte = type { i8 }", "mj2x_byte_value_of", "i8",
     "declare %mj2x_byte @mj2x_byte_value_of(i8)"},
    {"java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", "float", "%mj2x_float",
     "%mj2x_float = type { float }", "mj2x_float_value_of", "float",
     "declare %mj2x_float @mj2x_float_value_of(float)"},
    {"java/lang/Double", "valueOf", "(D)Ljava/lang/Double;", "double", "%mj2x_double",
     "%mj2x_double = type { double }", "mj2x_double_value_of", "double",
     "declare %mj2x_double @mj2x_double_value_of(double)"},
    {"java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", "bool", "%mj2x_bool",
     "%mj2x_bool = type { i8 }", "mj2x_bool_value_of", "i8",
     "declare %mj2x_bool @mj2x_bool_value_of(i8)"},
    {"java/lang/Character", "valueOf", "(C)Ljava/lang/Character;", "char", "%mj2x_char",
     "%mj2x_char = type { i16 }", "mj2x_char_value_of", "i16",
     "declare %mj2x_char @mj2x_char_value_of(i16)"},
};

/// @brief 包装类拆箱条目（xxxValue）：窄类型返回值先在发射端 sext/zext 到 i32。
constexpr LlvmWrapperUnboxEntry kUnboxEntries[] = {
    {"java/lang/Integer", "intValue", "()I", "int", "%mj2x_int", "%mj2x_int = type { i32 }",
     "mj2x_int_int_value", "i32", "", "declare i32 @mj2x_int_int_value(%mj2x_int)"},
    {"java/lang/Long", "longValue", "()J", "long", "%mj2x_long", "%mj2x_long = type { i64 }",
     "mj2x_long_long_value", "i64", "", "declare i64 @mj2x_long_long_value(%mj2x_long)"},
    {"java/lang/Short", "shortValue", "()S", "short", "%mj2x_short",
     "%mj2x_short = type { i16 }", "mj2x_short_short_value", "i16", "sext",
     "declare i16 @mj2x_short_short_value(%mj2x_short)"},
    {"java/lang/Byte", "byteValue", "()B", "byte", "%mj2x_byte", "%mj2x_byte = type { i8 }",
     "mj2x_byte_byte_value", "i8", "sext", "declare i8 @mj2x_byte_byte_value(%mj2x_byte)"},
    {"java/lang/Float", "floatValue", "()F", "float", "%mj2x_float",
     "%mj2x_float = type { float }", "mj2x_float_float_value", "float", "",
     "declare float @mj2x_float_float_value(%mj2x_float)"},
    {"java/lang/Double", "doubleValue", "()D", "double", "%mj2x_double",
     "%mj2x_double = type { double }", "mj2x_double_double_value", "double", "",
     "declare double @mj2x_double_double_value(%mj2x_double)"},
    {"java/lang/Boolean", "booleanValue", "()Z", "bool", "%mj2x_bool",
     "%mj2x_bool = type { i8 }", "mj2x_bool_bool_value", "i8", "zext",
     "declare i8 @mj2x_bool_bool_value(%mj2x_bool)"},
    {"java/lang/Character", "charValue", "()C", "char", "%mj2x_char",
     "%mj2x_char = type { i16 }", "mj2x_char_char_value", "i16", "zext",
     "declare i16 @mj2x_char_char_value(%mj2x_char)"},
};

}   // namespace

const LlvmIntrinsicEntry* matchLlvmIntrinsic(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc)
{
    for (const auto& entry : kEntries) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmWrapperBoxEntry* matchLlvmWrapperBox(ca::str::Utf8StringRef owner,
                                               ca::str::Utf8StringRef name,
                                               ca::str::Utf8StringRef desc)
{
    for (const auto& entry : kBoxEntries) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmWrapperUnboxEntry* matchLlvmWrapperUnbox(ca::str::Utf8StringRef owner,
                                                   ca::str::Utf8StringRef name,
                                                   ca::str::Utf8StringRef desc)
{
    for (const auto& entry : kUnboxEntries) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

}   // namespace mj2x

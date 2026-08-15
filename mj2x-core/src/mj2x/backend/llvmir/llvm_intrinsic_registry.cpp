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

}   // namespace mj2x

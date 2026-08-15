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

/// @brief Random 状态机切片：%mj2x_random = { i64 }（crt mj2x_random 为
/// uint64_t seed；标准 Java LCG：seed = (seed ^ 0x5DEECE66D) & ((1<<48)-1)，
/// next(bits) 返回 seed >> (48-bits)，与 java.util.Random 逐位一致）。
constexpr LlvmRandomEntry kRandomEntries[] = {
    {"java/util/Random", "<init>", "(J)V", "mj2x_random_init", "void",
     "declare void @mj2x_random_init(ptr, i64)"},
    {"java/util/Random", "nextInt", "()I", "mj2x_random_next_int", "i32",
     "declare i32 @mj2x_random_next_int(ptr)"},
    {"java/util/Random", "nextInt", "(I)I", "mj2x_random_next_int_bound", "i32",
     "declare i32 @mj2x_random_next_int_bound(ptr, i32)"},
    {"java/util/Random", "nextLong", "()J", "mj2x_random_next_long", "i64",
     "declare i64 @mj2x_random_next_long(ptr)"},
    {"java/util/Random", "nextBoolean", "()Z", "mj2x_random_next_boolean", "i8",
     "declare i8 @mj2x_random_next_boolean(ptr)"},
    {"java/util/Random", "nextFloat", "()F", "mj2x_random_next_float", "float",
     "declare float @mj2x_random_next_float(ptr)"},
    {"java/util/Random", "nextDouble", "()D", "mj2x_random_next_double", "double",
     "declare double @mj2x_random_next_double(ptr)"},
};

/// @brief StringBuilder/StringBuffer 状态机切片：crt 堆分配状态
/// `mj2x_string_builder*`（init 的 out_state 即寄存器槽），append 返回同一
/// state，method 经 out 参数回写结果。全部调用返回 mj2x_regc_status。
///
/// @details kind：0=init（参数 + out_state**），1=append（参数 + state，
/// dest 槽回存 state 指针），2=method（参数 + out）。argLlvm 的 i8/i16 表示
/// crt 形参为 int8_t/uint16_t，发射端先 trunc。
constexpr LlvmBuilderEntry kBuilderEntries[] = {
    // init
    {"java/lang/StringBuilder", "<init>", "()V", "mj2x_string_builder_init", 0, "",
     {"", "", ""}},
    {"java/lang/StringBuilder", "<init>", "(I)V", "mj2x_string_builder_init_capacity", 0, "",
     {"i32", "", ""}},
    {"java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V",
     "mj2x_string_builder_init_jstring", 0, "", {"ptr", "", ""}},
    {"java/lang/StringBuilder", "<init>", "(Ljava/lang/CharSequence;)V",
     "mj2x_string_builder_init_char_sequence", 0, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "<init>", "()V", "mj2x_string_buffer_init", 0, "",
     {"", "", ""}},
    {"java/lang/StringBuffer", "<init>", "(I)V", "mj2x_string_buffer_init_capacity", 0, "",
     {"i32", "", ""}},
    {"java/lang/StringBuffer", "<init>", "(Ljava/lang/String;)V",
     "mj2x_string_buffer_init_jstring", 0, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "<init>", "(Ljava/lang/CharSequence;)V",
     "mj2x_string_buffer_init_char_sequence", 0, "", {"ptr", "", ""}},
    // append（全部返回同一 state 指针）
    {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_jstring", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_object", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_char_sequence", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuilder", "append",
     "(Ljava/lang/CharSequence;II)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_char_sequence_range", 1, "", {"ptr", "i32", "i32"}},
    {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_i32", 1, "", {"i32", "", ""}},
    {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_i64", 1, "", {"i64", "", ""}},
    {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_bool", 1, "", {"i8", "", ""}},
    {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_char", 1, "", {"i16", "", ""}},
    {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_f32", 1, "", {"float", "", ""}},
    {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_f64", 1, "", {"double", "", ""}},
    {"java/lang/StringBuilder", "append", "([C)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_char_array", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuilder", "append", "([CII)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_char_array_range", 1, "", {"ptr", "i32", "i32"}},
    {"java/lang/StringBuilder", "appendCodePoint", "(I)Ljava/lang/StringBuilder;",
     "mj2x_string_builder_append_code_point", 1, "", {"i32", "", ""}},
    {"java/lang/StringBuffer", "append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_jstring", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_object", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "append", "(Ljava/lang/CharSequence;)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_char_sequence", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "append",
     "(Ljava/lang/CharSequence;II)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_char_sequence_range", 1, "", {"ptr", "i32", "i32"}},
    {"java/lang/StringBuffer", "append", "(I)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_i32", 1, "", {"i32", "", ""}},
    {"java/lang/StringBuffer", "append", "(J)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_i64", 1, "", {"i64", "", ""}},
    {"java/lang/StringBuffer", "append", "(Z)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_bool", 1, "", {"i8", "", ""}},
    {"java/lang/StringBuffer", "append", "(C)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_char", 1, "", {"i16", "", ""}},
    {"java/lang/StringBuffer", "append", "(F)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_f32", 1, "", {"float", "", ""}},
    {"java/lang/StringBuffer", "append", "(D)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_f64", 1, "", {"double", "", ""}},
    {"java/lang/StringBuffer", "append", "([C)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_char_array", 1, "", {"ptr", "", ""}},
    {"java/lang/StringBuffer", "append", "([CII)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_char_array_range", 1, "", {"ptr", "i32", "i32"}},
    {"java/lang/StringBuffer", "appendCodePoint", "(I)Ljava/lang/StringBuffer;",
     "mj2x_string_buffer_append_code_point", 1, "", {"i32", "", ""}},
    // method（toString 物化为真 jstring；length/capacity/charAt 经 out 回写）
    {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;",
     "mj2x_string_builder_materialize", 2, "ptr", {"", "", ""}},
    {"java/lang/StringBuilder", "length", "()I", "mj2x_string_builder_length", 2, "i32",
     {"", "", ""}},
    {"java/lang/StringBuilder", "capacity", "()I", "mj2x_string_builder_capacity", 2, "i32",
     {"", "", ""}},
    {"java/lang/StringBuilder", "charAt", "(I)C", "mj2x_string_builder_char_at", 2, "i16",
     {"i32", "", ""}},
    {"java/lang/StringBuilder", "ensureCapacity", "(I)V",
     "mj2x_string_builder_ensure_capacity", 2, "", {"i32", "", ""}},
    {"java/lang/StringBuilder", "setLength", "(I)V", "mj2x_string_builder_set_length", 2, "",
     {"i32", "", ""}},
    {"java/lang/StringBuilder", "setCharAt", "(IC)V", "mj2x_string_builder_set_char_at", 2, "",
     {"i32", "i16", ""}},
    {"java/lang/StringBuffer", "toString", "()Ljava/lang/String;",
     "mj2x_string_buffer_materialize", 2, "ptr", {"", "", ""}},
    {"java/lang/StringBuffer", "length", "()I", "mj2x_string_buffer_length", 2, "i32",
     {"", "", ""}},
    {"java/lang/StringBuffer", "capacity", "()I", "mj2x_string_buffer_capacity", 2, "i32",
     {"", "", ""}},
    {"java/lang/StringBuffer", "charAt", "(I)C", "mj2x_string_buffer_char_at", 2, "i16",
     {"i32", "", ""}},
    {"java/lang/StringBuffer", "ensureCapacity", "(I)V",
     "mj2x_string_buffer_ensure_capacity", 2, "", {"i32", "", ""}},
    {"java/lang/StringBuffer", "setLength", "(I)V", "mj2x_string_buffer_set_length", 2, "",
     {"i32", "", ""}},
    {"java/lang/StringBuffer", "setCharAt", "(IC)V", "mj2x_string_buffer_set_char_at", 2, "",
     {"i32", "i16", ""}},
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

const LlvmRandomEntry* matchLlvmRandomInit(ca::str::Utf8StringRef owner,
                                           ca::str::Utf8StringRef name,
                                           ca::str::Utf8StringRef desc)
{
    if (sameText(owner, "java/util/Random") && sameText(name, "<init>") &&
        sameText(desc, "(J)V")) {
        return &kRandomEntries[0];
    }
    return nullptr;
}

const LlvmRandomEntry* matchLlvmRandomMethod(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc)
{
    for (size_t index = 1; index < sizeof(kRandomEntries) / sizeof(kRandomEntries[0]); ++index) {
        const auto& entry = kRandomEntries[index];
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmBuilderEntry* matchLlvmBuilder(ca::str::Utf8StringRef owner,
                                         ca::str::Utf8StringRef name,
                                         ca::str::Utf8StringRef desc)
{
    for (const auto& entry : kBuilderEntries) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

bool isLlvmBuilderAllocation(ca::str::Utf8StringRef typeDesc)
{
    return sameText(typeDesc, "java/lang/StringBuilder") ||
           sameText(typeDesc, "java/lang/StringBuffer");
}

}   // namespace mj2x

/// llvm_intrinsic_registry_test.cpp —— #1027 B2 单源切换对拍。
///
/// llvm_intrinsic_registry 的数据面从"92 条手工对齐副本"切换为按 RegC 权威表
///（builtinIntrinsicTable + llvmDirect 位）机械派生。本测试内嵌切换前的手工表
/// 全文作 golden（仅测试内保留），对派生表逐条逐字段对拍：
///   - 表规模全等（21 Math / 8 装箱 / 8 拆箱 / 7 Random / 48 Builder）；
///   - 每条 golden (owner,name,desc) 经 matcher 命中且全部字段逐串一致；
///   - 负探针：llvmDirect=false 的条目（Integer/Long 位操作、trimToSize、
///     Enum、String 方法）不产生任何 LLVM 直调命中；
///   - 派生面一致性：builtinIntrinsicTable 中 llvmDirect 计数 == 五表条目和。
///
/// golden 文本来源：切换前 llvm_intrinsic_registry.cpp 的 kEntries/kBoxEntries/
/// kUnboxEntries/kRandomEntries/kBuilderEntries（git 887ba390e 版本）。

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "mj2x/backend/llvmir/llvm_intrinsic_registry.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"

namespace {

using namespace mj2x;

ca::str::Utf8StringRef ref(const char* text)
{
    return ca::str::Utf8StringRef::from_cstr(text);
}

bool eq(const char* a, const char* b)
{
    // golden 聚合初始化的尾随槽为 nullptr，语义 = 空文本（与派生侧 "" 等价）。
    const char* x = a != nullptr ? a : "";
    const char* y = b != nullptr ? b : "";
    return std::strcmp(x, y) == 0;
}

// ---------------------------------------------------------------------------
// golden：切换前手工表全文
// ---------------------------------------------------------------------------

constexpr LlvmIntrinsicEntry kGoldenMath[] = {
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

constexpr LlvmWrapperBoxEntry kGoldenBox[] = {
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

constexpr LlvmWrapperUnboxEntry kGoldenUnbox[] = {
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

constexpr LlvmRandomEntry kGoldenRandom[] = {
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

constexpr LlvmBuilderEntry kGoldenBuilder[] = {
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
     "mj2x_string_builder_to_jstring", 2, "ptr", {"", "", ""}},
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
    {"java/lang/StringBuilder", "setCharAt", "(IC)V", "mj2x_string_builder_set_char_at", 2,
     "", {"i32", "i16", ""}},
    {"java/lang/StringBuffer", "toString", "()Ljava/lang/String;",
     "mj2x_string_buffer_to_jstring", 2, "ptr", {"", "", ""}},
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

// ---------------------------------------------------------------------------
// 对拍
// ---------------------------------------------------------------------------

template <size_t N>
size_t sizeOf(const LlvmIntrinsicEntry (&)[N])
{
    return N;
}
template <size_t N>
size_t sizeOf(const LlvmWrapperBoxEntry (&)[N])
{
    return N;
}
template <size_t N>
size_t sizeOf(const LlvmWrapperUnboxEntry (&)[N])
{
    return N;
}
template <size_t N>
size_t sizeOf(const LlvmRandomEntry (&)[N])
{
    return N;
}
template <size_t N>
size_t sizeOf(const LlvmBuilderEntry (&)[N])
{
    return N;
}

TEST(LlvmIntrinsicRegistryGoldenCross, MathTableFieldByField)
{
    ASSERT_EQ(sizeOf(kGoldenMath), 21u);
    for (const auto& golden : kGoldenMath) {
        const LlvmIntrinsicEntry* e =
            matchLlvmIntrinsic(ref(golden.owner), ref(golden.name), ref(golden.desc));
        ASSERT_NE(e, nullptr) << golden.owner << "." << golden.name << golden.desc;
        EXPECT_TRUE(eq(e->owner, golden.owner));
        EXPECT_TRUE(eq(e->name, golden.name));
        EXPECT_TRUE(eq(e->desc, golden.desc));
        EXPECT_TRUE(eq(e->symbol, golden.symbol)) << golden.symbol;
        EXPECT_TRUE(eq(e->returnLlvm, golden.returnLlvm)) << golden.symbol;
        for (int i = 0; i < 2; ++i) {
            EXPECT_TRUE(eq(e->argLlvm[i], golden.argLlvm[i])) << golden.symbol << " arg" << i;
        }
        EXPECT_TRUE(eq(e->declareText, golden.declareText)) << golden.declareText;
    }
}

TEST(LlvmIntrinsicRegistryGoldenCross, WrapperBoxTableFieldByField)
{
    ASSERT_EQ(sizeOf(kGoldenBox), 8u);
    for (const auto& golden : kGoldenBox) {
        const LlvmWrapperBoxEntry* e =
            matchLlvmWrapperBox(ref(golden.owner), ref(golden.name), ref(golden.desc));
        ASSERT_NE(e, nullptr) << golden.owner << "." << golden.name << golden.desc;
        EXPECT_TRUE(eq(e->owner, golden.owner));
        EXPECT_TRUE(eq(e->name, golden.name));
        EXPECT_TRUE(eq(e->desc, golden.desc));
        EXPECT_TRUE(eq(e->kind, golden.kind));
        EXPECT_TRUE(eq(e->shadowType, golden.shadowType));
        EXPECT_TRUE(eq(e->shadowDecl, golden.shadowDecl));
        EXPECT_TRUE(eq(e->symbol, golden.symbol));
        EXPECT_TRUE(eq(e->argLlvm, golden.argLlvm));
        EXPECT_TRUE(eq(e->declareText, golden.declareText)) << golden.declareText;
    }
}

TEST(LlvmIntrinsicRegistryGoldenCross, WrapperUnboxTableFieldByField)
{
    ASSERT_EQ(sizeOf(kGoldenUnbox), 8u);
    for (const auto& golden : kGoldenUnbox) {
        const LlvmWrapperUnboxEntry* e =
            matchLlvmWrapperUnbox(ref(golden.owner), ref(golden.name), ref(golden.desc));
        ASSERT_NE(e, nullptr) << golden.owner << "." << golden.name << golden.desc;
        EXPECT_TRUE(eq(e->owner, golden.owner));
        EXPECT_TRUE(eq(e->name, golden.name));
        EXPECT_TRUE(eq(e->desc, golden.desc));
        EXPECT_TRUE(eq(e->kind, golden.kind));
        EXPECT_TRUE(eq(e->shadowType, golden.shadowType));
        EXPECT_TRUE(eq(e->shadowDecl, golden.shadowDecl));
        EXPECT_TRUE(eq(e->symbol, golden.symbol));
        EXPECT_TRUE(eq(e->retLlvm, golden.retLlvm));
        EXPECT_TRUE(eq(e->retExt, golden.retExt)) << golden.symbol << " ext";
        EXPECT_TRUE(eq(e->declareText, golden.declareText)) << golden.declareText;
    }
}

TEST(LlvmIntrinsicRegistryGoldenCross, RandomTableFieldByField)
{
    ASSERT_EQ(sizeOf(kGoldenRandom), 7u);
    // init 面（表首条）
    const LlvmRandomEntry* init =
        matchLlvmRandomInit(ref(kGoldenRandom[0].owner), ref(kGoldenRandom[0].name),
                            ref(kGoldenRandom[0].desc));
    ASSERT_NE(init, nullptr);
    EXPECT_TRUE(eq(init->owner, kGoldenRandom[0].owner));
    EXPECT_TRUE(eq(init->name, kGoldenRandom[0].name));
    EXPECT_TRUE(eq(init->desc, kGoldenRandom[0].desc));
    EXPECT_TRUE(eq(init->symbol, kGoldenRandom[0].symbol));
    EXPECT_TRUE(eq(init->retLlvm, kGoldenRandom[0].retLlvm));
    EXPECT_TRUE(eq(init->declareText, kGoldenRandom[0].declareText));
    // method 面（其余条目；init 不得混入）
    for (size_t i = 1; i < sizeOf(kGoldenRandom); ++i) {
        const LlvmRandomEntry* e =
            matchLlvmRandomMethod(ref(kGoldenRandom[i].owner), ref(kGoldenRandom[i].name),
                                  ref(kGoldenRandom[i].desc));
        ASSERT_NE(e, nullptr) << kGoldenRandom[i].name << kGoldenRandom[i].desc;
        EXPECT_TRUE(eq(e->owner, kGoldenRandom[i].owner));
        EXPECT_TRUE(eq(e->name, kGoldenRandom[i].name));
        EXPECT_TRUE(eq(e->desc, kGoldenRandom[i].desc));
        EXPECT_TRUE(eq(e->symbol, kGoldenRandom[i].symbol));
        EXPECT_TRUE(eq(e->retLlvm, kGoldenRandom[i].retLlvm));
        EXPECT_TRUE(eq(e->declareText, kGoldenRandom[i].declareText))
            << kGoldenRandom[i].declareText;
    }
    EXPECT_EQ(matchLlvmRandomMethod(ref("java/util/Random"), ref("<init>"), ref("(J)V")),
              nullptr);
    EXPECT_EQ(matchLlvmRandomInit(ref("java/util/Random"), ref("nextInt"), ref("()I")),
              nullptr);
}

TEST(LlvmIntrinsicRegistryGoldenCross, RandomPrecheckBitDerivedFromAuthoritativeTable)
{
    // #879 Y4 解耦：nextInt(I)I 的 bound<=0 预检位随权威表（builtinIntrinsicTable
    // → deriveRandomEntries 两趟派生）透传——发射端改按位判定，registry 改符号名
    // /新增形态时预检不再静默失效。钉三点：bound 面位 = PositiveBound、无参面
    // 位 = None、init 面（RandomInit 趟）位 = None。
    const LlvmRandomEntry* init =
        matchLlvmRandomInit(ref("java/util/Random"), ref("<init>"), ref("(J)V"));
    ASSERT_NE(init, nullptr);
    EXPECT_EQ(init->precheck, RegCIntrinsicPrecheck::None);

    const LlvmRandomEntry* nextInt =
        matchLlvmRandomMethod(ref("java/util/Random"), ref("nextInt"), ref("()I"));
    ASSERT_NE(nextInt, nullptr);
    EXPECT_EQ(nextInt->precheck, RegCIntrinsicPrecheck::None);

    const LlvmRandomEntry* nextIntBound =
        matchLlvmRandomMethod(ref("java/util/Random"), ref("nextInt"), ref("(I)I"));
    ASSERT_NE(nextIntBound, nullptr);
    EXPECT_EQ(nextIntBound->precheck, RegCIntrinsicPrecheck::PositiveBound);

    const LlvmRandomEntry* nextLong =
        matchLlvmRandomMethod(ref("java/util/Random"), ref("nextLong"), ref("()J"));
    ASSERT_NE(nextLong, nullptr);
    EXPECT_EQ(nextLong->precheck, RegCIntrinsicPrecheck::None);

    // 权威表投影位同值（单源一致性：kEntries 行 → Identity 投影 → LLVM 派生）。
    size_t positiveBoundCount = 0;
    for (const auto& id : builtinIntrinsicTable()) {
        if (id.precheck == RegCIntrinsicPrecheck::PositiveBound) ++positiveBoundCount;
    }
    EXPECT_EQ(positiveBoundCount, 1u);
}

TEST(LlvmIntrinsicRegistryGoldenCross, BuilderTableFieldByField)
{
    ASSERT_EQ(sizeOf(kGoldenBuilder), 48u);
    for (const auto& golden : kGoldenBuilder) {
        const LlvmBuilderEntry* e =
            matchLlvmBuilder(ref(golden.owner), ref(golden.name), ref(golden.desc));
        ASSERT_NE(e, nullptr) << golden.owner << "." << golden.name << golden.desc;
        EXPECT_TRUE(eq(e->owner, golden.owner));
        EXPECT_TRUE(eq(e->name, golden.name));
        EXPECT_TRUE(eq(e->desc, golden.desc));
        EXPECT_TRUE(eq(e->symbol, golden.symbol));
        EXPECT_EQ(e->kind, golden.kind) << golden.symbol;
        EXPECT_TRUE(eq(e->outLlvm, golden.outLlvm)) << golden.symbol;
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(eq(e->argLlvm[i], golden.argLlvm[i])) << golden.symbol << " arg" << i;
        }
    }
}

TEST(LlvmIntrinsicRegistryGoldenCross, LlvmDirectFalseStaysAbsent)
{
    // 负探针：权威表中 llvmDirect=false 的代表条目不得在任何 LLVM 面命中。
    EXPECT_EQ(matchLlvmIntrinsic(ref("java/lang/Integer"), ref("rotateLeft"), ref("(II)I")),
              nullptr);
    EXPECT_EQ(matchLlvmIntrinsic(ref("java/lang/Long"), ref("bitCount"), ref("(J)I")),
              nullptr);
    EXPECT_EQ(matchLlvmBuilder(ref("java/lang/StringBuilder"), ref("trimToSize"), ref("()V")),
              nullptr);
    EXPECT_EQ(matchLlvmBuilder(ref("java/lang/StringBuffer"), ref("trimToSize"), ref("()V")),
              nullptr);
    EXPECT_EQ(matchLlvmRandomMethod(ref("java/lang/Enum"), ref("ordinal"), ref("()I")),
              nullptr);
    EXPECT_EQ(matchLlvmIntrinsic(ref("java/lang/Integer"), ref("toHexString"),
                                 ref("(I)Ljava/lang/String;")),
              nullptr);
    // 分派面负探针（防串表）。
    EXPECT_EQ(matchLlvmWrapperBox(ref("java/lang/Math"), ref("abs"), ref("(I)I")), nullptr);
    EXPECT_EQ(matchLlvmBuilder(ref("java/util/Random"), ref("nextInt"), ref("(I)I")),
              nullptr);
}

TEST(LlvmIntrinsicRegistryGoldenCross, DerivedSurfaceMatchesLlvmDirectBit)
{
    // 单源一致性：权威表 llvmDirect 计数 == 五张派生表条目和（21+8+8+7+48=92）。
    size_t direct = 0;
    for (const auto& id : builtinIntrinsicTable()) {
        if (id.llvmDirect) ++direct;
    }
    EXPECT_EQ(builtinIntrinsicTable().size(), 173u);
    EXPECT_EQ(direct, sizeOf(kGoldenMath) + sizeOf(kGoldenBox) + sizeOf(kGoldenUnbox) +
                          sizeOf(kGoldenRandom) + sizeOf(kGoldenBuilder));
}

TEST(LlvmIntrinsicRegistryGoldenCross, BuilderAllocationFaceUnchanged)
{
    EXPECT_TRUE(isLlvmBuilderAllocation(ref("java/lang/StringBuilder")));
    EXPECT_TRUE(isLlvmBuilderAllocation(ref("java/lang/StringBuffer")));
    EXPECT_FALSE(isLlvmBuilderAllocation(ref("java/lang/String")));
}

}   // namespace

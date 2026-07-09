#include "regc_intrinsic_registry.hpp"

#include <ostream>

namespace mj2x {

namespace {

struct RegCIntrinsicEntry {
    const char* owner;
    const char* name;
    const char* desc;
    const char* symbol;
    RegCIntrinsicEmitKind emitKind;
    RegCIntrinsicNativeObjectKind nativeObjectKind;
    bool passContext;
    std::size_t argCount;
    bool isStatic;
};

constexpr RegCIntrinsicEntry kEntries[] = {
    {"java/lang/Math", "abs", "(I)I", "mj2x_math_abs_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "abs", "(J)J", "mj2x_math_abs_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "abs", "(F)F", "mj2x_math_abs_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "abs", "(D)D", "mj2x_math_abs_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "min", "(II)I", "mj2x_math_min_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "max", "(II)I", "mj2x_math_max_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "min", "(JJ)J", "mj2x_math_min_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "max", "(JJ)J", "mj2x_math_max_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "min", "(FF)F", "mj2x_math_min_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "max", "(FF)F", "mj2x_math_max_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "min", "(DD)D", "mj2x_math_min_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "max", "(DD)D", "mj2x_math_max_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "sin", "(D)D", "mj2x_math_sin",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "cos", "(D)D", "mj2x_math_cos",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "tan", "(D)D", "mj2x_math_tan",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "sqrt", "(D)D", "mj2x_math_sqrt",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "floor", "(D)D", "mj2x_math_floor",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "ceil", "(D)D", "mj2x_math_ceil",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "pow", "(DD)D", "mj2x_math_pow",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Math", "round", "(D)J", "mj2x_math_round",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 1, true},
    {"java/lang/Math", "random", "()D", "mj2x_math_random",
     RegCIntrinsicEmitKind::RuntimeFunction, RegCIntrinsicNativeObjectKind::None, false, 0, true},
    {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", "_mj2x_intrinsic_parse_int",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Integer", "toString", "(I)Ljava/lang/String;", "_mj2x_intrinsic_int_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Integer", "compare", "(II)I", "_mj2x_intrinsic_int_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", "mj2x_int_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Int, false, 1, true},
    {"java/lang/Integer", "intValue", "()I", "mj2x_int_int_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Int, false, 1, false},
    {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", "mj2x_long_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Long, false, 1, true},
    {"java/lang/Long", "longValue", "()J", "mj2x_long_long_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Long, false, 1, false},
    {"java/lang/Long", "parseLong", "(Ljava/lang/String;)J", "_mj2x_intrinsic_parse_long",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Long", "toString", "(J)Ljava/lang/String;", "_mj2x_intrinsic_long_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Long", "compare", "(JJ)I", "_mj2x_intrinsic_long_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Short", "toString", "(S)Ljava/lang/String;", "_mj2x_intrinsic_int_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Short", "compare", "(SS)I", "_mj2x_intrinsic_short_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Byte", "toString", "(B)Ljava/lang/String;", "_mj2x_intrinsic_int_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Byte", "compare", "(BB)I", "_mj2x_intrinsic_byte_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Float", "toString", "(F)Ljava/lang/String;", "_mj2x_intrinsic_float_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Double", "toString", "(D)Ljava/lang/String;", "_mj2x_intrinsic_double_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Boolean", "toString", "(Z)Ljava/lang/String;", "_mj2x_intrinsic_bool_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Boolean", "compare", "(ZZ)I", "_mj2x_intrinsic_bool_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/Character", "toString", "(C)Ljava/lang/String;", "_mj2x_intrinsic_char_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Character", "compare", "(CC)I", "_mj2x_intrinsic_char_compare",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, false, 2, true},
    {"java/lang/String", "valueOf", "(I)Ljava/lang/String;", "_mj2x_intrinsic_int_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(J)Ljava/lang/String;", "_mj2x_intrinsic_long_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(Z)Ljava/lang/String;", "_mj2x_intrinsic_bool_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(C)Ljava/lang/String;", "_mj2x_intrinsic_char_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(F)Ljava/lang/String;", "_mj2x_intrinsic_float_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(D)Ljava/lang/String;", "_mj2x_intrinsic_double_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/String", "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;",
     "_mj2x_intrinsic_object_to_string",
     RegCIntrinsicEmitKind::RegCLocalHelper, RegCIntrinsicNativeObjectKind::None, true, 1, true},
    {"java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", "mj2x_short_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Short, false, 1, true},
    {"java/lang/Short", "shortValue", "()S", "mj2x_short_short_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Short, false, 1, false},
    {"java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", "mj2x_byte_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Byte, false, 1, true},
    {"java/lang/Byte", "byteValue", "()B", "mj2x_byte_byte_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Byte, false, 1, false},
    {"java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", "mj2x_float_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Float, false, 1, true},
    {"java/lang/Float", "floatValue", "()F", "mj2x_float_float_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Float, false, 1, false},
    {"java/lang/Double", "valueOf", "(D)Ljava/lang/Double;", "mj2x_double_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Double, false, 1, true},
    {"java/lang/Double", "doubleValue", "()D", "mj2x_double_double_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Double, false, 1, false},
    {"java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", "mj2x_bool_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Bool, false, 1, true},
    {"java/lang/Boolean", "booleanValue", "()Z", "mj2x_bool_bool_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Bool, false, 1, false},
    {"java/lang/Character", "valueOf", "(C)Ljava/lang/Character;", "mj2x_char_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Char, false, 1, true},
    {"java/lang/Character", "charValue", "()C", "mj2x_char_char_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Char, false, 1, false},
    {"java/util/Random", "<init>", "(J)V", "mj2x_random_init",
     RegCIntrinsicEmitKind::RandomInit, RegCIntrinsicNativeObjectKind::Random, false, 2, false},
    {"java/util/Random", "nextInt", "()I", "mj2x_random_next_int",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 1, false},
    {"java/util/Random", "nextInt", "(I)I", "mj2x_random_next_int_bound",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 2, false},
    {"java/util/Random", "nextLong", "()J", "mj2x_random_next_long",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 1, false},
    {"java/util/Random", "nextBoolean", "()Z", "mj2x_random_next_boolean",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 1, false},
    {"java/util/Random", "nextFloat", "()F", "mj2x_random_next_float",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 1, false},
    {"java/util/Random", "nextDouble", "()D", "mj2x_random_next_double",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::Random, false, 1, false},
    {"java/lang/StringBuilder", "<init>", "()V", "_mj2x_intrinsic_string_builder_init_empty",
     RegCIntrinsicEmitKind::StringBuilderInit, RegCIntrinsicNativeObjectKind::StringBuilder, false,
     1, false},
    {"java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V",
     "_mj2x_intrinsic_string_builder_init_string",
     RegCIntrinsicEmitKind::StringBuilderInit, RegCIntrinsicNativeObjectKind::StringBuilder, false,
     2, false},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_string",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_object",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append",
     "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_object",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_int",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_long",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_bool",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_char",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_float",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;",
     "_mj2x_intrinsic_string_builder_append_double",
     RegCIntrinsicEmitKind::StringBuilderAppend, RegCIntrinsicNativeObjectKind::StringBuilder,
     false, 2, false},
    {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;",
     "_mj2x_intrinsic_string_builder_to_string",
     RegCIntrinsicEmitKind::NativeObjectMethod, RegCIntrinsicNativeObjectKind::StringBuilder, false,
     1, false},
    {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", "_mj2x_intrinsic_string_equals",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
    {"java/lang/String", "length", "()I", "_mj2x_intrinsic_string_length",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 1, false},
    {"java/lang/String", "charAt", "(I)C", "_mj2x_intrinsic_string_char_at",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
    {"java/lang/String", "hashCode", "()I", "_mj2x_intrinsic_string_hash_code",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 1, false},
    {"java/lang/String", "isEmpty", "()Z", "_mj2x_intrinsic_string_is_empty",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 1, false},
    {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z",
     "_mj2x_intrinsic_string_starts_with",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
    {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z",
     "_mj2x_intrinsic_string_ends_with",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
    {"java/lang/String", "indexOf", "(Ljava/lang/String;)I", "_mj2x_intrinsic_string_index_of",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
    {"java/lang/String", "contains", "(Ljava/lang/CharSequence;)Z",
     "_mj2x_intrinsic_string_contains",
     RegCIntrinsicEmitKind::StringMethod, RegCIntrinsicNativeObjectKind::None, false, 2, false},
};

bool sameSignature(const RegCIntrinsicCall& call, const RegCIntrinsicEntry& entry) {
    return call.owner == entry.owner &&
           call.name == entry.name &&
           call.desc == entry.desc &&
           call.isStatic() == entry.isStatic;
}

bool sameIntrinsicFamily(const RegCIntrinsicCall& call, const RegCIntrinsicEntry& entry) {
    return call.owner == entry.owner && call.name == entry.name;
}

RegCIntrinsicDecision fallbackWithReason(const std::string& reason) {
    RegCIntrinsicDecision decision;
    decision.kind = RegCIntrinsicDecisionKind::Fallback;
    decision.reason = reason;
    return decision;
}

} // namespace

const RegCIntrinsicRegistry& RegCIntrinsicRegistry::builtins() {
    static const RegCIntrinsicRegistry registry;
    return registry;
}

RegCIntrinsicDecision RegCIntrinsicRegistry::matchCall(const RegInstruction& ins) const {
    return matchNormalizedCall(RegCIntrinsicCallMatcher::fromInstruction(ins));
}

RegCIntrinsicDecision RegCIntrinsicRegistry::matchNormalizedCall(const RegCIntrinsicCall& call) const {
    bool knownFamily = false;
    for (const auto& entry : kEntries) {
        if (!sameIntrinsicFamily(call, entry)) continue;
        knownFamily = true;
        if (!sameSignature(call, entry)) continue;

        RegCIntrinsicDecision decision;
        if (call.argCount != entry.argCount) {
            decision.kind = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "malformed RegIR argument count for intrinsic " + call.displayName();
            return decision;
        }
        decision.kind = RegCIntrinsicDecisionKind::Emit;
        decision.emitKind = entry.emitKind;
        decision.nativeObjectKind = entry.nativeObjectKind;
        decision.symbol = entry.symbol;
        decision.passContext = entry.passContext;
        return decision;
    }

    if (knownFamily) {
        return fallbackWithReason("unsupported intrinsic overload: " + call.displayName());
    }
    return RegCIntrinsicDecision{};
}

void RegCIntrinsicRegistry::emitDeclarations(std::ostream& out) const {
    out << "#ifndef MJ2X_REGC_INTRINSIC_ABI_DECLARED\n";
    out << "#define MJ2X_REGC_INTRINSIC_ABI_DECLARED\n";
    out << "static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8);\n";
    out << "static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr);\n";
    out << "static int64_t _mj2x_intrinsic_parse_long(mj2x_regc_context* context, mj2x_ref jstr);\n";
    out << "static mj2x_ref _mj2x_intrinsic_int_to_string(mj2x_regc_context* context, int32_t value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_long_to_string(mj2x_regc_context* context, int64_t value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_bool_to_string(mj2x_regc_context* context, int8_t value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_char_to_string(mj2x_regc_context* context, uint16_t value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_float_to_string(mj2x_regc_context* context, float value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_double_to_string(mj2x_regc_context* context, double value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_object_to_string(mj2x_regc_context* context, mj2x_ref value);\n";
    out << "static int32_t _mj2x_intrinsic_int_compare(int32_t lhs, int32_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_long_compare(int64_t lhs, int64_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_byte_compare(int8_t lhs, int8_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_short_compare(int16_t lhs, int16_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_bool_compare(int8_t lhs, int8_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_char_compare(uint16_t lhs, uint16_t rhs);\n";
    out << "static int32_t _mj2x_intrinsic_string_equals(mj2x_ref lhs, mj2x_ref rhs);\n";
    out << "static int32_t _mj2x_intrinsic_string_length(mj2x_ref value);\n";
    out << "static uint16_t _mj2x_intrinsic_string_char_at(mj2x_ref value, int32_t index);\n";
    out << "static int32_t _mj2x_intrinsic_string_hash_code(mj2x_ref value);\n";
    out << "static int32_t _mj2x_intrinsic_string_is_empty(mj2x_ref value);\n";
    out << "static int32_t _mj2x_intrinsic_string_starts_with(mj2x_ref value, mj2x_ref prefix);\n";
    out << "static int32_t _mj2x_intrinsic_string_ends_with(mj2x_ref value, mj2x_ref suffix);\n";
    out << "static int32_t _mj2x_intrinsic_string_index_of(mj2x_ref value, mj2x_ref needle);\n";
    out << "static int32_t _mj2x_intrinsic_string_contains(mj2x_ref value, mj2x_ref seq);\n";
    out << "typedef struct mj2x_int { int32_t value; } mj2x_int;\n";
    out << "typedef struct mj2x_long { int64_t value; } mj2x_long;\n";
    out << "typedef struct mj2x_short { int16_t value; } mj2x_short;\n";
    out << "typedef struct mj2x_byte { int8_t value; } mj2x_byte;\n";
    out << "typedef struct mj2x_float { float value; } mj2x_float;\n";
    out << "typedef struct mj2x_double { double value; } mj2x_double;\n";
    out << "typedef struct mj2x_bool { int8_t value; } mj2x_bool;\n";
    out << "typedef struct mj2x_char { uint16_t value; } mj2x_char;\n";
    out << "typedef struct mj2x_random { uint64_t seed; } mj2x_random;\n";
    out << "#ifndef MJ2X_REGC_STRING_BUILDER_DECLARED\n";
    out << "#define MJ2X_REGC_STRING_BUILDER_DECLARED\n";
    out << "typedef struct mj2x_string_builder { char data[1024]; int32_t length; int32_t overflow; } mj2x_string_builder;\n";
    out << "#endif\n";
    out << "mj2x_int mj2x_int_value_of(int32_t value);\n";
    out << "int32_t mj2x_int_int_value(mj2x_int box);\n";
    out << "mj2x_long mj2x_long_value_of(int64_t value);\n";
    out << "int64_t mj2x_long_long_value(mj2x_long box);\n";
    out << "mj2x_short mj2x_short_value_of(int16_t value);\n";
    out << "int16_t mj2x_short_short_value(mj2x_short box);\n";
    out << "mj2x_byte mj2x_byte_value_of(int8_t value);\n";
    out << "int8_t mj2x_byte_byte_value(mj2x_byte box);\n";
    out << "mj2x_float mj2x_float_value_of(float value);\n";
    out << "float mj2x_float_float_value(mj2x_float box);\n";
    out << "mj2x_double mj2x_double_value_of(double value);\n";
    out << "double mj2x_double_double_value(mj2x_double box);\n";
    out << "mj2x_bool mj2x_bool_value_of(int8_t value);\n";
    out << "int8_t mj2x_bool_bool_value(mj2x_bool box);\n";
    out << "mj2x_char mj2x_char_value_of(uint16_t value);\n";
    out << "uint16_t mj2x_char_char_value(mj2x_char box);\n";
    out << "void mj2x_random_init(mj2x_random* state, int64_t seed);\n";
    out << "int32_t mj2x_random_next_int(mj2x_random* state);\n";
    out << "int32_t mj2x_random_next_int_bound(mj2x_random* state, int32_t bound);\n";
    out << "int64_t mj2x_random_next_long(mj2x_random* state);\n";
    out << "int8_t mj2x_random_next_boolean(mj2x_random* state);\n";
    out << "float mj2x_random_next_float(mj2x_random* state);\n";
    out << "double mj2x_random_next_double(mj2x_random* state);\n";
    out << "static void _mj2x_intrinsic_string_builder_init_empty(mj2x_string_builder* state);\n";
    out << "static void _mj2x_intrinsic_string_builder_init_string(mj2x_string_builder* state, mj2x_ref value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_string(mj2x_string_builder* state, mj2x_ref value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_object(mj2x_string_builder* state, mj2x_ref value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_int(mj2x_string_builder* state, int32_t value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_long(mj2x_string_builder* state, int64_t value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_bool(mj2x_string_builder* state, bool value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_char(mj2x_string_builder* state, uint16_t value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_float(mj2x_string_builder* state, float value);\n";
    out << "static void _mj2x_intrinsic_string_builder_append_double(mj2x_string_builder* state, double value);\n";
    out << "static mj2x_ref _mj2x_intrinsic_string_builder_to_string(mj2x_string_builder* state);\n";
    out << "int32_t mj2x_math_abs_i32(int32_t value);\n";
    out << "int64_t mj2x_math_abs_i64(int64_t value);\n";
    out << "float mj2x_math_abs_f32(float value);\n";
    out << "double mj2x_math_abs_f64(double value);\n";
    out << "int32_t mj2x_math_min_i32(int32_t lhs, int32_t rhs);\n";
    out << "int32_t mj2x_math_max_i32(int32_t lhs, int32_t rhs);\n";
    out << "int64_t mj2x_math_min_i64(int64_t lhs, int64_t rhs);\n";
    out << "int64_t mj2x_math_max_i64(int64_t lhs, int64_t rhs);\n";
    out << "float mj2x_math_min_f32(float lhs, float rhs);\n";
    out << "float mj2x_math_max_f32(float lhs, float rhs);\n";
    out << "double mj2x_math_min_f64(double lhs, double rhs);\n";
    out << "double mj2x_math_max_f64(double lhs, double rhs);\n";
    out << "double mj2x_math_sin(double value);\n";
    out << "double mj2x_math_cos(double value);\n";
    out << "double mj2x_math_tan(double value);\n";
    out << "double mj2x_math_sqrt(double value);\n";
    out << "double mj2x_math_pow(double base, double exponent);\n";
    out << "double mj2x_math_floor(double value);\n";
    out << "double mj2x_math_ceil(double value);\n";
    out << "int64_t mj2x_math_round(double value);\n";
    out << "double mj2x_math_random();\n";
    out << "#endif\n\n";
}

} // namespace mj2x

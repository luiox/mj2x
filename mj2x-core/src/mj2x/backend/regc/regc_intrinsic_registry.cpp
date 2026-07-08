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
    {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", "mj2x_int_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Int, false, 1, true},
    {"java/lang/Integer", "intValue", "()I", "mj2x_int_int_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Int, false, 1, false},
    {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", "mj2x_long_value_of",
     RegCIntrinsicEmitKind::WrapperValueOf, RegCIntrinsicNativeObjectKind::Long, false, 1, true},
    {"java/lang/Long", "longValue", "()J", "mj2x_long_long_value",
     RegCIntrinsicEmitKind::WrapperUnbox, RegCIntrinsicNativeObjectKind::Long, false, 1, false},
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
    out << "static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8);\n";
    out << "static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr);\n";
    out << "typedef struct mj2x_int { int32_t value; } mj2x_int;\n";
    out << "typedef struct mj2x_long { int64_t value; } mj2x_long;\n";
    out << "typedef struct mj2x_short { int16_t value; } mj2x_short;\n";
    out << "typedef struct mj2x_byte { int8_t value; } mj2x_byte;\n";
    out << "typedef struct mj2x_float { float value; } mj2x_float;\n";
    out << "typedef struct mj2x_double { double value; } mj2x_double;\n";
    out << "typedef struct mj2x_bool { int8_t value; } mj2x_bool;\n";
    out << "typedef struct mj2x_char { uint16_t value; } mj2x_char;\n";
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
    out << "double mj2x_math_random();\n\n";
}

} // namespace mj2x

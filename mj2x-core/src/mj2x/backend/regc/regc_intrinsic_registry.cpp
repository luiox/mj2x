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
    bool passContext;
    std::size_t argCount;
};

constexpr RegCIntrinsicEntry kEntries[] = {
    {"java/lang/Math", "abs", "(I)I", "mj2x_math_abs_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "abs", "(J)J", "mj2x_math_abs_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "abs", "(F)F", "mj2x_math_abs_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "abs", "(D)D", "mj2x_math_abs_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "min", "(II)I", "mj2x_math_min_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "max", "(II)I", "mj2x_math_max_i32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "min", "(JJ)J", "mj2x_math_min_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "max", "(JJ)J", "mj2x_math_max_i64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "min", "(FF)F", "mj2x_math_min_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "max", "(FF)F", "mj2x_math_max_f32",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "min", "(DD)D", "mj2x_math_min_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "max", "(DD)D", "mj2x_math_max_f64",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "sin", "(D)D", "mj2x_math_sin",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "cos", "(D)D", "mj2x_math_cos",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "tan", "(D)D", "mj2x_math_tan",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "sqrt", "(D)D", "mj2x_math_sqrt",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "floor", "(D)D", "mj2x_math_floor",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "ceil", "(D)D", "mj2x_math_ceil",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "pow", "(DD)D", "mj2x_math_pow",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 2},
    {"java/lang/Math", "round", "(D)J", "mj2x_math_round",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 1},
    {"java/lang/Math", "random", "()D", "mj2x_math_random",
     RegCIntrinsicEmitKind::RuntimeFunction, false, 0},
    {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", "_mj2x_intrinsic_parse_int",
     RegCIntrinsicEmitKind::RegCLocalHelper, true, 1},
};

bool sameSignature(const RegCIntrinsicCall& call, const RegCIntrinsicEntry& entry) {
    return call.owner == entry.owner &&
           call.name == entry.name &&
           call.desc == entry.desc;
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
    if (!call.isStatic()) {
        return RegCIntrinsicDecision{};
    }

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

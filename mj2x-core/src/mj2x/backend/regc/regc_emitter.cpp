#include "regc_emitter.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace mj2x {

using namespace jbytecode::core;

namespace {

// cStringLiteral is shared with CBackend via codegen::cStringLiteral; alias it
// here so the existing call sites stay one-line.
using codegen::cStringLiteral;

const char* cType(SSAType type) {
    switch (type) {
        case SSAType::VOID: return "void";
        case SSAType::BOOL: return "bool";
        case SSAType::I8: return "int8_t";
        case SSAType::I16: return "int16_t";
        case SSAType::CHAR: return "uint16_t";
        case SSAType::I32: return "int32_t";
        case SSAType::I64: return "int64_t";
        case SSAType::F32: return "float";
        case SSAType::F64: return "double";
        case SSAType::REF: return "void*";
        default: return "int32_t";
    }
}

const char* unsignedCType(SSAType type) {
    switch (type) {
        case SSAType::I64: return "uint64_t";
        default: return "uint32_t";
    }
}

bool isIntegerLike(SSAType type) {
    return ssaIsIntLike(type) || type == SSAType::I64;
}

std::string sanitizeIdent(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string functionName(const SSAFunctionSymbol& symbol) {
    return "regc_" + sanitizeIdent(symbol.owner) + "_" +
           sanitizeIdent(symbol.name) + "_" + sanitizeIdent(symbol.desc);
}

std::string regName(RegId id) {
    return "r" + std::to_string(id.value);
}

const char* defaultValue(SSAType type) {
    switch (type) {
        case SSAType::VOID: return "";
        case SSAType::F32: return "0.0f";
        case SSAType::F64: return "0.0";
        case SSAType::REF: return "NULL";
        case SSAType::BOOL: return "false";
        default: return "0";
    }
}

const char* helperSuffix(SSAType type) {
    switch (type) {
        case SSAType::VOID: return "void";
        case SSAType::BOOL:
        case SSAType::I8:
        case SSAType::I16:
        case SSAType::CHAR:
        case SSAType::I32: return "i32";
        case SSAType::I64: return "i64";
        case SSAType::F32: return "f32";
        case SSAType::F64: return "f64";
        case SSAType::REF: return "ref";
        default: return "i32";
    }
}

std::string operandExpr(const RegOperand& operand) {
    std::ostringstream os;
    switch (operand.kind) {
        case RegOperandKind::Register:
            return regName(operand.reg);
        case RegOperandKind::IntLiteral:
            if (operand.type == SSAType::I64) {
                os << "(int64_t)" << operand.intValue << "LL";
            } else {
                os << operand.intValue;
            }
            return os.str();
        case RegOperandKind::FloatLiteral:
            os << operand.floatValue;
            if (operand.type == SSAType::F32) os << "f";
            return os.str();
        case RegOperandKind::NullLiteral:
            return "NULL";
        case RegOperandKind::Opaque:
            // A String LDC carries its UTF-8 content after "ldc_string:" so the
            // String intrinsic path can materialize the jstring without going
            // through JNI call dispatch.  Other opaques stay as annotated NULL.
            if (operand.tag.rfind("ldc_string:", 0) == 0) {
                return "_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), "
                       + cStringLiteral(operand.tag.substr(std::string("ldc_string:").size())) + ")";
            }
            return "NULL /* opaque:" + operand.tag + " */";
    }
    return "0";
}

const char* binaryOp(SSABinaryOp op) {
    switch (op) {
        case SSABinaryOp::Add: return "+";
        case SSABinaryOp::Sub: return "-";
        case SSABinaryOp::Mul: return "*";
        case SSABinaryOp::Div: return "/";
        case SSABinaryOp::Rem: return "%";
        case SSABinaryOp::And: return "&";
        case SSABinaryOp::Or: return "|";
        case SSABinaryOp::Xor: return "^";
        case SSABinaryOp::Shl: return "<<";
        case SSABinaryOp::Shr: return ">>";
        case SSABinaryOp::UShr: return ">>";
    }
    return "+";
}

const char* compareOp(SSAComparePredicate predicate) {
    switch (predicate) {
        case SSAComparePredicate::Eq: return "==";
        case SSAComparePredicate::Ne: return "!=";
        case SSAComparePredicate::Lt: return "<";
        case SSAComparePredicate::Le: return "<=";
        case SSAComparePredicate::Gt: return ">";
        case SSAComparePredicate::Ge: return ">=";
    }
    return "==";
}

bool collectRegister(std::map<uint32_t, SSAType>& regs, RegValue value) {
    auto [it, inserted] = regs.emplace(value.id.value, value.type);
    return inserted || it->second == value.type;
}

void collectCopies(std::map<uint32_t, SSAType>& regs, const std::vector<RegCopy>& copies) {
    for (const auto& copy : copies) collectRegister(regs, copy.dest);
}

const char* fieldHelperSuffix(const std::string& desc, SSAType fallback) {
    // JVM narrow fields are int-like on the operand stack but require their
    // descriptor-specific JNI accessors at the native boundary.
    if (!desc.empty()) {
        switch (desc[0]) {
            case 'Z': return "bool";
            case 'B': return "i8";
            case 'C': return "char";
            case 'S': return "i16";
            case 'I': return "i32";
            case 'J': return "i64";
            case 'F': return "f32";
            case 'D': return "f64";
            default: return "ref";
        }
    }
    return helperSuffix(fallback);
}

const char* arrayHelperSuffix(SSAType type) {
    // BALOAD/BASTORE cover both byte[] and boolean[]; the i8 runtime helper
    // performs the final array-class check when it sees the actual reference.
    switch (type) {
        case SSAType::BOOL:
        case SSAType::I8: return "i8";
        case SSAType::I16: return "i16";
        case SSAType::CHAR: return "char";
        default: return helperSuffix(type);
    }
}

bool isStaticCall(const RegInstruction& ins) {
    return ins.method.invokeOpcode == Opcodes::INVOKESTATIC;
}

void collectExceptionRegisters(std::map<uint32_t, SSAType>& regs,
                               const std::vector<RegExceptionRegion>& regions) {
    for (const auto& region : regions) collectRegister(regs, region.exceptionValue);
}

void collectTerminatorRegisters(std::map<uint32_t, SSAType>& regs, const RegTerminator& terminator) {
    std::visit([&](const auto& t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, RegJumpTerminator>) {
            collectCopies(regs, t.target.copies);
        } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
            collectCopies(regs, t.trueTarget.copies);
            collectCopies(regs, t.falseTarget.copies);
        } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
            for (const auto& c : t.cases) collectCopies(regs, c.target.copies);
            collectCopies(regs, t.defaultTarget.copies);
        }
    }, terminator);
}

bool collectRegisters(const RegFunction& fn, std::map<uint32_t, SSAType>& regs, std::string& reason) {
    for (const auto& parameter : fn.parameters) collectRegister(regs, parameter);
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && !collectRegister(regs, ins.dest)) {
                reason = "conflicting RegIR destination type";
                return false;
            }
            if (ins.kind == RegInstructionKind::Copy) collectRegister(regs, ins.copy.dest);
        }
        collectTerminatorRegisters(regs, block->terminator);
    }
    collectExceptionRegisters(regs, fn.exceptionRegions);
    return true;
}

bool isSupportedInstruction(const RegInstruction& ins, std::string& reason) {
    switch (ins.kind) {
        case RegInstructionKind::Copy:
        case RegInstructionKind::Unary:
        case RegInstructionKind::Binary:
        case RegInstructionKind::Compare:
        case RegInstructionKind::Convert:
        case RegInstructionKind::LoadField:
        case RegInstructionKind::StoreField:
        case RegInstructionKind::ArrayLoad:
        case RegInstructionKind::ArrayStore:
        case RegInstructionKind::Call:
        case RegInstructionKind::Allocate:
        case RegInstructionKind::Intrinsic:
        case RegInstructionKind::Barrier:
            return true;
    }
    reason = "unknown RegIR instruction kind";
    return false;
}

std::string binaryExpr(const RegInstruction& ins) {
    const std::string lhs = operandExpr(ins.lhs);
    const std::string rhs = operandExpr(ins.rhs);
    const SSAType type = ins.dest.type;

    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Div || ins.binaryOp == SSABinaryOp::Rem)) {
        const char* op = ins.binaryOp == SSABinaryOp::Div ? "div" : "rem";
        const char* width = type == SSAType::I64 ? "i64" : "i32";
        return "mj2x_regc_" + std::string(op) + "_" + width +
               "(_mj2x_regc_current_context(), " + lhs + ", " + rhs + ")";
    }
    if (ins.binaryOp == SSABinaryOp::UShr) {
        return "(" + std::string(cType(type)) + ")((" + unsignedCType(type) + ")" +
               lhs + " >> " + rhs + ")";
    }
    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub ||
         ins.binaryOp == SSABinaryOp::Mul)) {
        return "(" + std::string(cType(type)) + ")((" + unsignedCType(type) + ")" +
               lhs + " " + binaryOp(ins.binaryOp) + " (" + unsignedCType(type) + ")" + rhs + ")";
    }
    if ((type == SSAType::F32 || type == SSAType::F64) && ins.binaryOp == SSABinaryOp::Rem) {
        return std::string(type == SSAType::F32 ? "fmodf" : "fmod") + "(" + lhs + ", " + rhs + ")";
    }
    return "(" + std::string(cType(type)) + ")(" + lhs + " " + binaryOp(ins.binaryOp) + " " + rhs + ")";
}

void emitHelperPrelude(std::ostringstream& out) {
    out << "typedef void* mj2x_ref;\n";
    out << "#ifndef MJ2X_REGC_RUNTIME_ABI_DECLARED\n";
    out << "#define MJ2X_REGC_RUNTIME_ABI_DECLARED\n";
    out << "typedef struct mj2x_regc_context mj2x_regc_context;\n";
    out << "int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);\n";
    out << "int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);\n";
    out << "int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);\n";
    out << "int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);\n";
    out << "void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);\n";
    out << "#endif\n";
    out << "static mj2x_regc_context* _mj2x_regc_current_context(void);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, int32_t* dims);\n";
    out << "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref);\n";
    out << "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc);\n";
    out << "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc);\n";
    out << "static void _mj2x_regc_monitor_enter(mj2x_ref value);\n";
    out << "static void _mj2x_regc_monitor_exit(mj2x_ref value);\n";
    out << "static int32_t _mj2x_regc_exception_check(void);\n";
    out << "static mj2x_ref _mj2x_regc_exception_occurred(void);\n";
    out << "static void _mj2x_regc_exception_clear(void);\n";
    out << "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type);\n";
    out << "static void _mj2x_regc_throw(mj2x_ref value);\n";
    out << "static bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc);\n";
    out << "static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, bool value);\n";
    out << "static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int8_t value);\n";
    out << "static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, uint16_t value);\n";
    out << "static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int16_t value);\n";
    out << "static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int32_t value);\n";
    out << "static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, int64_t value);\n";
    out << "static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, float value);\n";
    out << "static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, double value);\n";
    out << "static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const char* owner, const char* name, const char* desc, mj2x_ref value);\n";
    out << "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t value);\n";
    out << "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t value);\n";
    out << "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t value);\n";
    out << "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t value);\n";
    out << "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t value);\n";
    out << "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float value);\n";
    out << "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double value);\n";
    out << "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref value);\n";
    out << "static int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static float _mj2x_regc_call_f32(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static double _mj2x_regc_call_f64(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static void _mj2x_regc_call_void(const char* owner, const char* name, const char* desc, int32_t is_static, int32_t arg_count, ...);\n";
    // String LDC intrinsic: materializes a jstring from UTF-8 so String-typed
    // intrinsics (parseInt/literal/toString) avoid JNI call dispatch.  The
    // body is emitted by the JNI TU prelude where _mj2x_regc_env is in scope.
    out << "static mj2x_ref _mj2x_intrinsic_string_literal(mj2x_regc_context* context, const char* utf8);\n";
    // String-typed JDK intrinsic whose body needs JNI (GetStringUTFChars) to
    // reach the literal content.  Declared here and defined in the JNI TU.
    out << "static int32_t _mj2x_intrinsic_parse_int(mj2x_regc_context* context, mj2x_ref jstr);\n";
    // Pure-computation Math intrinsics live in the runtime lib (no JNI); these
    // declarations resolve at link time against mj2x-runtime-core the same way
    // mj2x_regc_div_i32 does.
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

std::string callArgs(const std::vector<RegOperand>& args) {
    std::ostringstream os;
    os << static_cast<int32_t>(args.size());
    for (const auto& arg : args) {
        os << ", (" << cType(arg.type) << ")(" << operandExpr(arg) << ")";
    }
    return os.str();
}

// Emit a unary or binary runtime-math intrinsic call.  Returns true when the
// signature matched and the call was written.  Kept narrow: only static
// java.lang.Math methods whose args are pure primitives, so no receiver and no
// JNI boundary is involved.
bool tryEmitMathIntrinsic(std::ostringstream& out, const RegInstruction& ins) {
    if (!isStaticCall(ins)) return false;
    if (ins.method.owner != "java/lang/Math") return false;

    // Each entry: descriptor suffix -> C runtime symbol.  The descriptor is
    // matched against ins.method.desc so overload resolution (abs(I)I vs
    // abs(D)D) picks the right width.
    struct MathEntry {
        const char* method;
        const char* desc;
        const char* runtimeSymbol;
        bool unary;
    };
    static constexpr MathEntry entries[] = {
        {"abs", "(I)I", "mj2x_math_abs_i32", true},
        {"abs", "(J)J", "mj2x_math_abs_i64", true},
        {"abs", "(F)F", "mj2x_math_abs_f32", true},
        {"abs", "(D)D", "mj2x_math_abs_f64", true},
        {"min", "(II)I", "mj2x_math_min_i32", false},
        {"max", "(II)I", "mj2x_math_max_i32", false},
        {"min", "(JJ)J", "mj2x_math_min_i64", false},
        {"max", "(JJ)J", "mj2x_math_max_i64", false},
        {"min", "(FF)F", "mj2x_math_min_f32", false},
        {"max", "(FF)F", "mj2x_math_max_f32", false},
        {"min", "(DD)D", "mj2x_math_min_f64", false},
        {"max", "(DD)D", "mj2x_math_max_f64", false},
        {"sin", "(D)D", "mj2x_math_sin", true},
        {"cos", "(D)D", "mj2x_math_cos", true},
        {"tan", "(D)D", "mj2x_math_tan", true},
        {"sqrt", "(D)D", "mj2x_math_sqrt", true},
        {"floor", "(D)D", "mj2x_math_floor", true},
        {"ceil", "(D)D", "mj2x_math_ceil", true},
        {"pow", "(DD)D", "mj2x_math_pow", false},
        {"round", "(D)J", "mj2x_math_round", true},
        {"random", "()D", "mj2x_math_random", true},
    };

    for (const auto& e : entries) {
        if (ins.method.name != e.method || ins.method.desc != e.desc) continue;

        const bool isRandom = ins.method.name == "random";
        if (isRandom && !ins.args.empty()) return false;
        if (!isRandom && ins.args.empty()) return false;

        out << "    ";
        if (ins.hasDest) out << regName(ins.dest.id) << " = ";
        out << e.runtimeSymbol << "(";
        if (isRandom) {
            out << ")";
        } else if (e.unary) {
            out << operandExpr(ins.args[0]) << ")";
        } else {
            out << operandExpr(ins.args[0]) << ", " << operandExpr(ins.args[1]) << ")";
        }
        out << ";\n";
        return true;
    }
    return false;
}

// Emit Integer.parseInt(String) as a native intrinsic instead of a JNI static
// call.  The body reads the jstring UTF-8 bytes and parses them, so the parse
// never dispatches through CallStaticIntMethod.  Returns true when matched.
bool tryEmitStringParseIntrinsic(std::ostringstream& out, const RegInstruction& ins) {
    if (!isStaticCall(ins)) return false;
    if (ins.method.owner == "java/lang/Integer" &&
        ins.method.name == "parseInt" &&
        ins.method.desc == "(Ljava/lang/String;)I" &&
        ins.args.size() == 1) {
        out << "    ";
        if (ins.hasDest) out << regName(ins.dest.id) << " = ";
        out << "_mj2x_intrinsic_parse_int(_mj2x_regc_current_context(), "
            << operandExpr(ins.args[0]) << ");\n";
        return true;
    }
    return false;
}

// Dispatcher: returns true if a JDK method call was lowered to an intrinsic
// (math or String), in which case the generic JNI call helper is skipped.
bool tryEmitIntrinsicCall(std::ostringstream& out, const RegInstruction& ins) {
    if (tryEmitMathIntrinsic(out, ins)) return true;
    if (tryEmitStringParseIntrinsic(out, ins)) return true;
    return false;
}

void emitEdgeCopies(std::ostringstream& out, const std::vector<RegCopy>& copies, int edgeIndex) {
    if (copies.empty()) return;

    // Edge copies are parallel phi transfers.  Stage sources in temporaries so
    // cycles such as r1<-r2 and r2<-r1 remain well-defined for later RegC work.
    for (size_t i = 0; i < copies.size(); ++i) {
        out << "    " << cType(copies[i].dest.type) << " edge_" << edgeIndex << "_" << i
            << " = (" << cType(copies[i].dest.type) << ")(" << operandExpr(copies[i].source) << ");\n";
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        out << "    " << regName(copies[i].dest.id) << " = edge_" << edgeIndex << "_" << i << ";\n";
    }
}

bool protectsBlock(const RegExceptionRegion& region, const RegBlock* block) {
    return std::find(region.protectedBlocks.begin(), region.protectedBlocks.end(), block) !=
           region.protectedBlocks.end();
}

std::vector<const RegExceptionRegion*> activeExceptionRegions(const RegFunction& fn,
                                                              const RegBlock* block) {
    std::vector<const RegExceptionRegion*> active;
    for (const auto& region : fn.exceptionRegions) {
        if (protectsBlock(region, block)) active.push_back(&region);
    }
    return active;
}

void emitDefaultReturnAfterException(std::ostringstream& out, SSAType returnType) {
    if (returnType == SSAType::VOID) {
        out << "        return;\n";
    } else {
        out << "        return " << defaultValue(returnType) << ";\n";
    }
}

void emitExceptionDispatch(std::ostringstream& out,
                           const std::vector<const RegExceptionRegion*>& regions,
                           SSAType returnType,
                           int& dispatchIndex) {
    if (regions.empty()) return;

    const int index = dispatchIndex++;
    out << "    if (_mj2x_regc_exception_check()) {\n";
    out << "        mj2x_ref ex_" << index << " = _mj2x_regc_exception_occurred();\n";
    out << "        _mj2x_regc_exception_clear();\n";
    // Match in exception-table order.  A matched handler receives the caught
    // object through its synthetic exception register before the block jump.
    for (const auto* region : regions) {
        out << "        if (_mj2x_regc_exception_matches(ex_" << index << ", "
            << cStringLiteral(region->caughtType) << ")) {\n";
        out << "            " << regName(region->exceptionValue.id) << " = ex_" << index << ";\n";
        out << "            goto block_b" << region->handler->id.value << ";\n";
        out << "        }\n";
    }
    // Preserve Java propagation when no handler accepts the pending exception.
    out << "        _mj2x_regc_throw(ex_" << index << ");\n";
    emitDefaultReturnAfterException(out, returnType);
    out << "    }\n";
}

bool emitInstruction(std::ostringstream& out, const RegInstruction& ins, std::string& reason) {
    if (!isSupportedInstruction(ins, reason)) return false;

    switch (ins.kind) {
        case RegInstructionKind::Copy:
            out << "    " << regName(ins.copy.dest.id) << " = ("
                << cType(ins.copy.dest.type) << ")(" << operandExpr(ins.copy.source) << ");\n";
            return true;
        case RegInstructionKind::Unary:
            out << "    " << regName(ins.dest.id) << " = (" << cType(ins.dest.type)
                << ")(-" << operandExpr(ins.value) << ");\n";
            return true;
        case RegInstructionKind::Binary:
            out << "    " << regName(ins.dest.id) << " = " << binaryExpr(ins) << ";\n";
            return true;
        case RegInstructionKind::Compare:
            out << "    " << regName(ins.dest.id) << " = ("
                << operandExpr(ins.lhs) << " " << compareOp(ins.comparePredicate)
                << " " << operandExpr(ins.rhs) << ");\n";
            return true;
        case RegInstructionKind::Convert:
            if (ins.convertKind == SSAConvertKind::ReferenceCast) {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_checkcast("
                    << operandExpr(ins.value) << ", " << cStringLiteral(ins.referenceTypeDesc) << ");\n";
            } else {
                out << "    " << regName(ins.dest.id) << " = (" << cType(ins.targetType)
                    << ")(" << operandExpr(ins.value) << ");\n";
            }
            return true;
        case RegInstructionKind::LoadField:
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_load_"
                << fieldHelperSuffix(ins.field.desc, ins.dest.type) << "_field("
                << (ins.field.isStatic ? "1, NULL" : "0, " + operandExpr(ins.receiver)) << ", "
                << cStringLiteral(ins.field.owner) << ", "
                << cStringLiteral(ins.field.name) << ", "
                << cStringLiteral(ins.field.desc) << ");\n";
            return true;
        case RegInstructionKind::StoreField:
            out << "    _mj2x_regc_store_" << fieldHelperSuffix(ins.field.desc, ins.value.type) << "_field("
                << (ins.field.isStatic ? "1, NULL" : "0, " + operandExpr(ins.receiver)) << ", "
                << cStringLiteral(ins.field.owner) << ", "
                << cStringLiteral(ins.field.name) << ", "
                << cStringLiteral(ins.field.desc) << ", "
                << operandExpr(ins.value) << ");\n";
            return true;
        case RegInstructionKind::ArrayLoad:
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_load_"
                << arrayHelperSuffix(ins.elementType) << "_array("
                << operandExpr(ins.receiver) << ", " << operandExpr(ins.index) << ");\n";
            return true;
        case RegInstructionKind::ArrayStore:
            out << "    _mj2x_regc_store_" << arrayHelperSuffix(ins.elementType) << "_array("
                << operandExpr(ins.receiver) << ", " << operandExpr(ins.index)
                << ", " << operandExpr(ins.value) << ");\n";
            return true;
        case RegInstructionKind::Call:
            // JDK intrinsic recognition: Math.* and Integer.parseInt are lowered
            // to direct runtime calls so no JNI method dispatch is emitted.  If
            // no intrinsic matches, fall through to the generic JNI call helper.
            if (tryEmitIntrinsicCall(out, ins)) return true;
            if (ins.hasDest) {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_call_"
                    << helperSuffix(ins.dest.type) << "("
                    << cStringLiteral(ins.method.owner) << ", "
                    << cStringLiteral(ins.method.name) << ", "
                    << cStringLiteral(ins.method.desc) << ", "
                    << (isStaticCall(ins) ? "1" : "0") << ", "
                    << callArgs(ins.args) << ");\n";
            } else {
                out << "    _mj2x_regc_call_void("
                    << cStringLiteral(ins.method.owner) << ", "
                    << cStringLiteral(ins.method.name) << ", "
                    << cStringLiteral(ins.method.desc) << ", "
                    << (isStaticCall(ins) ? "1" : "0") << ", "
                    << callArgs(ins.args) << ");\n";
            }
            return true;
        case RegInstructionKind::Allocate:
            if (ins.allocationKind == SSAAllocationKind::Object) {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_alloc_object("
                    << cStringLiteral(ins.typeDesc) << ");\n";
            } else if (ins.allocationKind == SSAAllocationKind::Array) {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_alloc_array("
                    << cStringLiteral(ins.typeDesc) << ", " << operandExpr(ins.args.empty() ? RegOperand::fromInt(0, SSAType::I32) : ins.args[0]) << ");\n";
            } else {
                out << "    {\n";
                out << "        int32_t dims[" << ins.args.size() << "];\n";
                for (size_t i = 0; i < ins.args.size(); ++i) {
                    out << "        dims[" << i << "] = " << operandExpr(ins.args[i]) << ";\n";
                }
                out << "        " << regName(ins.dest.id) << " = _mj2x_regc_alloc_multi_array("
                    << cStringLiteral(ins.typeDesc) << ", " << ins.args.size() << ", dims);\n";
                out << "    }\n";
            }
            return true;
        case RegInstructionKind::Intrinsic:
            if (ins.intrinsicName == "array.length") {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_array_length("
                    << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
                return true;
            }
            if (ins.intrinsicName.rfind("instanceof:", 0) == 0) {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_instanceof("
                    << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ", "
                    << cStringLiteral(ins.intrinsicName.substr(11)) << ");\n";
                return true;
            }
            if (ins.intrinsicName == "monitor.enter") {
                out << "    _mj2x_regc_monitor_enter("
                    << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
                return true;
            }
            if (ins.intrinsicName == "monitor.exit") {
                out << "    _mj2x_regc_monitor_exit("
                    << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
                return true;
            }
            reason = "unsupported RegC intrinsic: " + ins.intrinsicName;
            return false;
        case RegInstructionKind::Barrier:
            out << "    mj2x_regc_barrier(_mj2x_regc_current_context(), "
                << cStringLiteral(ins.barrierReason) << ");\n";
            return true;
        default:
            break;
    }

    reason = "unreachable RegC instruction path";
    return false;
}

bool emitTerminator(std::ostringstream& out,
                    const RegTerminator& terminator,
                    SSAType returnType,
                    const std::vector<const RegExceptionRegion*>& activeRegions,
                    int& dispatchIndex,
                    std::string& reason) {
    int edgeIndex = 0;
    return std::visit([&](const auto& t) -> bool {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, RegJumpTerminator>) {
            out << "    {\n";
            emitEdgeCopies(out, t.target.copies, edgeIndex++);
            out << "    goto block_b" << t.target.block->id.value << ";\n";
            out << "    }\n";
            return true;
        } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
            out << "    if (" << operandExpr(t.condition) << ") {\n";
            emitEdgeCopies(out, t.trueTarget.copies, edgeIndex++);
            out << "        goto block_b" << t.trueTarget.block->id.value << ";\n";
            out << "    } else {\n";
            emitEdgeCopies(out, t.falseTarget.copies, edgeIndex++);
            out << "        goto block_b" << t.falseTarget.block->id.value << ";\n";
            out << "    }\n";
            return true;
        } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
            out << "    switch (" << operandExpr(t.value) << ") {\n";
            for (const auto& c : t.cases) {
                out << "    case " << c.key << ": {\n";
                emitEdgeCopies(out, c.target.copies, edgeIndex++);
                out << "        goto block_b" << c.target.block->id.value << ";\n";
                out << "    }\n";
            }
            out << "    default: {\n";
            emitEdgeCopies(out, t.defaultTarget.copies, edgeIndex++);
            out << "        goto block_b" << t.defaultTarget.block->id.value << ";\n";
            out << "    }\n";
            out << "    }\n";
            return true;
        } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
            if (t.hasValue) {
                out << "    return " << operandExpr(t.value) << ";\n";
            } else {
                out << "    return;\n";
            }
            return true;
        } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
            out << "    _mj2x_regc_throw(" << operandExpr(t.value) << ");\n";
            emitExceptionDispatch(out, activeRegions, returnType, dispatchIndex);
            if (returnType == SSAType::VOID) out << "    return;\n";
            else out << "    return " << defaultValue(returnType) << ";\n";
            return true;
        } else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
            if (returnType == SSAType::VOID) {
                out << "    return;\n";
            } else {
                out << "    return " << defaultValue(returnType) << ";\n";
            }
            return true;
        } else {
            reason = "unknown RegIR terminator";
            return false;
        }
    }, terminator);
}

} // namespace

RegCEmitResult RegCEmitter::emitFunction(const RegFunction& fn) const {
    RegCEmitResult result;
    result.functionName = functionName(fn.symbol);
    if (!fn.entry) {
        result.reason = "RegC requires an entry block";
        return result;
    }

    std::string validationReason;
    if (!validateRegIRNoStackSlots(fn, validationReason)) {
        result.reason = validationReason;
        return result;
    }

    std::map<uint32_t, SSAType> regs;
    if (!collectRegisters(fn, regs, result.reason)) return result;

    std::ostringstream out;
    out << "/* Generated by mj2x RegC MVP */\n";
    out << "#include <stdbool.h>\n";
    out << "#include <stdint.h>\n";
    out << "#include <stddef.h>\n";
    out << "#include <math.h>\n\n";
    emitHelperPrelude(out);

    out << "static " << cType(fn.symbol.returnType) << " " << result.functionName << "(";
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out << ", ";
        out << cType(fn.parameters[i].type) << " " << regName(fn.parameters[i].id);
    }
    out << ") {\n";

    for (const auto& [id, type] : regs) {
        bool isParameter = false;
        for (const auto& param : fn.parameters) {
            if (param.id.value == id) {
                isParameter = true;
                break;
            }
        }
        if (!isParameter) {
            out << "    " << cType(type) << " " << regName(RegId{id}) << " = 0;\n";
        }
    }
    if (!regs.empty()) out << "\n";
    out << "    goto block_b" << fn.entry->id.value << ";\n\n";

    int dispatchIndex = 0;
    for (const auto& block : fn.blocks) {
        auto activeRegions = activeExceptionRegions(fn, block.get());
        out << "block_b" << block->id.value << ":\n";
        for (const auto& ins : block->instructions) {
            if (!emitInstruction(out, ins, result.reason)) return result;
            if (ins.effect.mayThrow) {
                emitExceptionDispatch(out, activeRegions, fn.symbol.returnType, dispatchIndex);
            }
        }
        if (!emitTerminator(out, block->terminator, fn.symbol.returnType,
                            activeRegions, dispatchIndex, result.reason)) {
            return result;
        }
        out << "\n";
    }

    out << "}\n";

    result.source = out.str();
    if (!validateRegCNoStackAnchors(result.source, result.reason)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.reason.clear();
    return result;
}

bool validateRegCNoStackAnchors(const std::string& source, std::string& reason) {
    const char* anchors[] = {
        "cstack[",
        "cstack",
        "stack[",
        "union jvalue",
        " sp ",
        "sp--",
        "sp++",
        "Cannot invoke",
        "cclasses[",
        "cmethods[",
    };
    for (const char* anchor : anchors) {
        if (source.find(anchor) != std::string::npos) {
            reason = std::string("RegC output contains stack-machine anchor: ") + anchor;
            return false;
        }
    }
    reason.clear();
    return true;
}

} // namespace mj2x

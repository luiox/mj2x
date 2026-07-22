#include "llvm_ir_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

using codegen::mangleMethod;
using RegisterTypes = std::map<uint32_t, SSAType>;

const char* llvmType(SSAType type)
{
    switch (type) {
    case SSAType::BOOL: return "i1";
    case SSAType::I32: return "i32";
    case SSAType::I64: return "i64";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::VOID: return "void";
    default: return nullptr;
    }
}

bool isSupportedValueType(SSAType type)
{
    return llvmType(type) != nullptr && type != SSAType::VOID;
}

bool hasObservableEffect(const SSAEffect& effect)
{
    return effect.mayThrow || effect.readsMemory || effect.writesMemory ||
           effect.readsExternalState || effect.writesExternalState || effect.callsExternal ||
           effect.resolvesExternal || effect.isBarrier;
}

PrimitiveKind returnKind(const std::string& desc)
{
    return jvmDescToPrimitive(Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str()))
                                  .getDescriptor()
                                  .to_std_string());
}

std::vector<PrimitiveKind> argumentKinds(const std::string& desc)
{
    std::vector<PrimitiveKind> kinds;
    auto methodType = Type::getMethodType(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    for (const auto& argument : methodType.getArgumentTypes()) {
        kinds.push_back(jvmSortToPrimitive(argument.getSort()));
    }
    return kinds;
}

SSAType primitiveToSsaType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::INT: return SSAType::I32;
    case PrimitiveKind::LONG: return SSAType::I64;
    case PrimitiveKind::FLOAT: return SSAType::F32;
    case PrimitiveKind::DOUBLE: return SSAType::F64;
    case PrimitiveKind::VOID: return SSAType::VOID;
    default: return SSAType::UNKNOWN;
    }
}

bool recordValue(RegisterTypes& types, RegValue value, std::string& reason)
{
    if (!isSupportedValueType(value.type)) {
        reason = "LLVM IR only supports scalar i32/i64/f32/f64 values";
        return false;
    }
    const auto [it, inserted] = types.emplace(value.id.value, value.type);
    if (!inserted && it->second != value.type) {
        reason = "LLVM IR encountered a register with conflicting types";
        return false;
    }
    return true;
}

bool recordOperand(RegisterTypes& types, const RegOperand& operand, std::string& reason)
{
    switch (operand.kind) {
    case RegOperandKind::Register: return recordValue(types, RegValue{operand.reg, operand.type}, reason);
    case RegOperandKind::IntLiteral:
    case RegOperandKind::FloatLiteral:
        if (!isSupportedValueType(operand.type)) {
            reason = "LLVM IR literal has an unsupported scalar type";
            return false;
        }
        return true;
    case RegOperandKind::NullLiteral:
    case RegOperandKind::Opaque:
        reason = "LLVM IR first slice rejects reference and opaque operands";
        return false;
    }
    reason = "LLVM IR encountered an unknown operand kind";
    return false;
}

bool recordCopies(RegisterTypes& types, const std::vector<RegCopy>& copies, std::string& reason)
{
    for (const auto& copy : copies) {
        if (!recordValue(types, copy.dest, reason) || !recordOperand(types, copy.source, reason))
            return false;
        if (copy.dest.type != copy.source.type) {
            reason = "LLVM IR edge copy changes value type";
            return false;
        }
    }
    return true;
}

bool recordSuccessor(RegisterTypes& types, const RegSuccessor& successor, std::string& reason)
{
    if (successor.block == nullptr) {
        reason = "LLVM IR encountered a CFG successor without a target block";
        return false;
    }
    return recordCopies(types, successor.copies, reason);
}

bool isSupportedBinary(const RegInstruction& instruction, std::string& reason)
{
    const SSAType type = instruction.dest.type;
    if (type == SSAType::F32 || type == SSAType::F64) {
        switch (instruction.binaryOp) {
        case SSABinaryOp::Add:
        case SSABinaryOp::Sub:
        case SSABinaryOp::Mul:
        case SSABinaryOp::Div:
        case SSABinaryOp::Rem: return true;
        default:
            reason = "LLVM IR floating-point bitwise and shift operations are unsupported";
            return false;
        }
    }
    if (type == SSAType::I32 || type == SSAType::I64) {
        switch (instruction.binaryOp) {
        case SSABinaryOp::Add:
        case SSABinaryOp::Sub:
        case SSABinaryOp::Mul:
        case SSABinaryOp::And:
        case SSABinaryOp::Or:
        case SSABinaryOp::Xor: return true;
        case SSABinaryOp::Div:
        case SSABinaryOp::Rem:
            reason = "LLVM IR first slice rejects integer div/rem because they may throw";
            return false;
        case SSABinaryOp::Shl:
        case SSABinaryOp::Shr:
        case SSABinaryOp::UShr:
            reason = "LLVM IR first slice rejects Java shift masking semantics";
            return false;
        }
    }
    reason = "LLVM IR binary operation has an unsupported value type";
    return false;
}

bool recordInstruction(RegisterTypes& types, const RegInstruction& instruction, std::string& reason)
{
    if (hasObservableEffect(instruction.effect)) {
        reason = "LLVM IR first slice rejects instructions with observable effects";
        return false;
    }
    switch (instruction.kind) {
    case RegInstructionKind::Copy:
        return recordValue(types, instruction.copy.dest, reason) &&
               recordOperand(types, instruction.copy.source, reason) &&
               instruction.copy.dest.type == instruction.copy.source.type;
    case RegInstructionKind::Unary:
        if (instruction.unaryOp != SSAUnaryOp::Neg) {
            reason = "LLVM IR first slice only supports arithmetic negation";
            return false;
        }
        return recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.value, reason);
    case RegInstructionKind::Binary:
        return isSupportedBinary(instruction, reason) && recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.lhs, reason) &&
               recordOperand(types, instruction.rhs, reason);
    case RegInstructionKind::Compare:
        if (instruction.dest.type != SSAType::BOOL) {
            reason = "LLVM IR comparison result must be boolean";
            return false;
        }
        return recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.lhs, reason) &&
               recordOperand(types, instruction.rhs, reason);
    case RegInstructionKind::Convert:
        if (instruction.convertKind != SSAConvertKind::Numeric) {
            reason = "LLVM IR first slice rejects reference casts";
            return false;
        }
        return recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.value, reason);
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier:
        reason = "LLVM IR first slice rejects object, JNI, memory and monitor operations";
        return false;
    }
    reason = "LLVM IR encountered an unknown RegIR instruction";
    return false;
}

bool collectRegisterTypes(const RegFunction& function, RegisterTypes& types, std::string& reason)
{
    if (!function.exceptionRegions.empty()) {
        reason = "LLVM IR first slice rejects exception regions";
        return false;
    }
    for (const auto& parameter : function.parameters) {
        if (!recordValue(types, parameter, reason)) return false;
    }
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block->instructions) {
            if (!recordInstruction(types, instruction, reason)) return false;
        }
        const bool terminatorOk = std::visit(
            [&](const auto& terminator) -> bool {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    return recordSuccessor(types, terminator.target, reason);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    return recordOperand(types, terminator.condition, reason) &&
                           recordSuccessor(types, terminator.trueTarget, reason) &&
                           recordSuccessor(types, terminator.falseTarget, reason);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    if (!recordOperand(types, terminator.value, reason) ||
                        !recordSuccessor(types, terminator.defaultTarget, reason)) {
                        return false;
                    }
                    for (const auto& entry : terminator.cases) {
                        if (!recordSuccessor(types, entry.target, reason)) return false;
                    }
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    return !terminator.hasValue || recordOperand(types, terminator.value, reason);
                }
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    reason = "LLVM IR first slice rejects throw terminators";
                    return false;
                }
                else {
                    reason = "LLVM IR first slice rejects unreachable terminators";
                    return false;
                }
            },
            block->terminator);
        if (!terminatorOk) return false;
    }
    return true;
}

struct PendingEdge
{
    std::string           label;
    const RegSuccessor*   target = nullptr;
};

class LlvmFunctionEmitter
{
public:
    LlvmFunctionEmitter(const RegFunction& function, const RegisterTypes& registerTypes,
                        const std::string& functionName, SSAType returnType)
        : function_(function)
        , registerTypes_(registerTypes)
        , functionName_(functionName)
        , returnType_(returnType)
    {}

    bool emit(std::string& source, std::string& reason)
    {
        const char* returnLlvmType = llvmType(returnType_);
        if (returnLlvmType == nullptr || function_.entry == nullptr) {
            reason = "LLVM IR function has no supported return type or entry block";
            return false;
        }

        out_ << "define internal " << returnLlvmType << " @" << functionName_ << "(";
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            if (index != 0) out_ << ", ";
            out_ << llvmType(function_.parameters[index].type) << " %p" << index;
        }
        out_ << ") {\nentry:\n";
        for (const auto& [id, type] : registerTypes_) {
            out_ << "  %slot_r" << id << " = alloca " << llvmType(type) << "\n";
        }
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            const auto& parameter = function_.parameters[index];
            out_ << "  store " << llvmType(parameter.type) << " %p" << index << ", ptr %slot_r"
                 << parameter.id.value << "\n";
        }
        out_ << "  br label %b" << function_.entry->id.value << "\n\n";

        for (const auto& block : function_.blocks) {
            if (!emitBlock(*block, reason)) return false;
        }
        for (const auto& edge : pendingEdges_) {
            out_ << edge.label << ":\n";
            if (!emitCopies(edge.target->copies, reason)) return false;
            out_ << "  br label %b" << edge.target->block->id.value << "\n\n";
        }
        out_ << "}\n\n";
        source = out_.str();
        return true;
    }

private:
    std::string nextTemporary()
    {
        return "%v" + std::to_string(temporaryIndex_++);
    }

    std::string slotName(RegId id) const { return "%slot_r" + std::to_string(id.value); }

    bool emitOperand(const RegOperand& operand, std::string& expression, std::string& reason)
    {
        const char* type = llvmType(operand.type);
        if (type == nullptr) {
            reason = "LLVM IR encountered an unsupported operand type";
            return false;
        }
        if (operand.kind == RegOperandKind::Register) {
            const std::string temporary = nextTemporary();
            out_ << "  " << temporary << " = load " << type << ", ptr " << slotName(operand.reg)
                 << "\n";
            expression = temporary;
            return true;
        }
        if (operand.kind == RegOperandKind::IntLiteral) {
            expression = std::to_string(operand.intValue);
            if (operand.type == SSAType::BOOL) expression = operand.intValue == 0 ? "0" : "1";
            return true;
        }
        if (operand.kind == RegOperandKind::FloatLiteral) {
            if (!std::isfinite(operand.floatValue) ||
                (operand.floatValue == 0.0 && std::signbit(operand.floatValue))) {
                reason = "LLVM IR first slice rejects NaN, infinity and negative-zero literals";
                return false;
            }
            std::ostringstream literal;
            literal << std::scientific << std::showpoint;
            if (operand.type == SSAType::F32) {
                literal << std::setprecision(9) << static_cast<float>(operand.floatValue);
            }
            else if (operand.type == SSAType::F64) {
                literal << std::setprecision(17) << operand.floatValue;
            }
            else {
                reason = "LLVM IR floating literal has a non-floating type";
                return false;
            }
            expression = literal.str();
            return true;
        }
        reason = "LLVM IR first slice rejects reference and opaque operands";
        return false;
    }

    bool emitStore(RegValue destination, const std::string& expression, std::string& reason)
    {
        const char* type = llvmType(destination.type);
        if (type == nullptr) {
            reason = "LLVM IR destination has an unsupported type";
            return false;
        }
        out_ << "  store " << type << " " << expression << ", ptr " << slotName(destination.id)
             << "\n";
        return true;
    }

    bool emitCopies(const std::vector<RegCopy>& copies, std::string& reason)
    {
        std::vector<std::pair<RegValue, std::string>> values;
        values.reserve(copies.size());
        for (const auto& copy : copies) {
            std::string expression;
            if (!emitOperand(copy.source, expression, reason)) return false;
            values.emplace_back(copy.dest, std::move(expression));
        }
        for (const auto& [destination, expression] : values) {
            if (!emitStore(destination, expression, reason)) return false;
        }
        return true;
    }

    std::string edgeTarget(const RegSuccessor& successor)
    {
        if (successor.copies.empty()) return "%b" + std::to_string(successor.block->id.value);
        PendingEdge edge;
        edge.label = "edge_" + std::to_string(edgeIndex_++);
        edge.target = &successor;
        pendingEdges_.push_back(edge);
        return "%" + edge.label;
    }

    bool emitInstruction(const RegInstruction& instruction, std::string& reason)
    {
        std::string lhs;
        std::string rhs;
        std::string value;
        if (instruction.kind == RegInstructionKind::Copy) {
            return emitOperand(instruction.copy.source, value, reason) &&
                   emitStore(instruction.copy.dest, value, reason);
        }
        if (instruction.kind == RegInstructionKind::Unary) {
            if (!emitOperand(instruction.value, value, reason)) return false;
            const char* type = llvmType(instruction.dest.type);
            const std::string result = nextTemporary();
            if (instruction.dest.type == SSAType::F32 || instruction.dest.type == SSAType::F64) {
                out_ << "  " << result << " = fneg " << type << " " << value << "\n";
            }
            else if (instruction.dest.type == SSAType::I32 || instruction.dest.type == SSAType::I64) {
                out_ << "  " << result << " = sub " << type << " 0, " << value << "\n";
            }
            else {
                reason = "LLVM IR negation has an unsupported type";
                return false;
            }
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Binary) {
            if (!emitOperand(instruction.lhs, lhs, reason) ||
                !emitOperand(instruction.rhs, rhs, reason)) {
                return false;
            }
            const bool floating = instruction.dest.type == SSAType::F32 ||
                                  instruction.dest.type == SSAType::F64;
            const char* opcode = nullptr;
            switch (instruction.binaryOp) {
            case SSABinaryOp::Add: opcode = floating ? "fadd" : "add"; break;
            case SSABinaryOp::Sub: opcode = floating ? "fsub" : "sub"; break;
            case SSABinaryOp::Mul: opcode = floating ? "fmul" : "mul"; break;
            case SSABinaryOp::Div: opcode = floating ? "fdiv" : nullptr; break;
            case SSABinaryOp::Rem: opcode = floating ? "frem" : nullptr; break;
            case SSABinaryOp::And: opcode = floating ? nullptr : "and"; break;
            case SSABinaryOp::Or: opcode = floating ? nullptr : "or"; break;
            case SSABinaryOp::Xor: opcode = floating ? nullptr : "xor"; break;
            case SSABinaryOp::Shl:
            case SSABinaryOp::Shr:
            case SSABinaryOp::UShr: break;
            }
            if (opcode == nullptr) {
                reason = "LLVM IR binary operation is outside the pure scalar subset";
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << opcode << " " << llvmType(instruction.dest.type)
                 << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Compare) {
            if (!emitOperand(instruction.lhs, lhs, reason) ||
                !emitOperand(instruction.rhs, rhs, reason)) {
                return false;
            }
            const bool floating = instruction.lhs.type == SSAType::F32 ||
                                  instruction.lhs.type == SSAType::F64;
            const char* predicate = nullptr;
            switch (instruction.comparePredicate) {
            case SSAComparePredicate::Eq: predicate = floating ? "oeq" : "eq"; break;
            case SSAComparePredicate::Ne: predicate = floating ? "une" : "ne"; break;
            case SSAComparePredicate::Lt: predicate = floating ? "olt" : "slt"; break;
            case SSAComparePredicate::Le: predicate = floating ? "ole" : "sle"; break;
            case SSAComparePredicate::Gt: predicate = floating ? "ogt" : "sgt"; break;
            case SSAComparePredicate::Ge: predicate = floating ? "oge" : "sge"; break;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << (floating ? "fcmp " : "icmp ") << predicate
                 << " " << llvmType(instruction.lhs.type) << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Convert) {
            if (!emitOperand(instruction.value, value, reason)) return false;
            const SSAType sourceType = instruction.value.type;
            const SSAType targetType = instruction.targetType;
            if (!isSupportedValueType(targetType)) {
                reason = "LLVM IR numeric conversion has an unsupported target type";
                return false;
            }
            if (sourceType == targetType) return emitStore(instruction.dest, value, reason);
            const bool sourceInt = sourceType == SSAType::I32 || sourceType == SSAType::I64;
            const bool targetInt = targetType == SSAType::I32 || targetType == SSAType::I64;
            const bool sourceFloat = sourceType == SSAType::F32 || sourceType == SSAType::F64;
            const bool targetFloat = targetType == SSAType::F32 || targetType == SSAType::F64;
            const char* opcode = nullptr;
            if (sourceInt && targetInt) {
                opcode = sourceType == SSAType::I32 ? "sext" : "trunc";
            }
            else if (sourceInt && targetFloat) {
                opcode = "sitofp";
            }
            else if (sourceFloat && targetFloat) {
                opcode = sourceType == SSAType::F32 ? "fpext" : "fptrunc";
            }
            else {
                reason = "LLVM IR first slice rejects floating-to-integer conversion semantics";
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << opcode << " " << llvmType(sourceType) << " "
                 << value << " to " << llvmType(targetType) << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        reason = "LLVM IR encountered an unsupported instruction after validation";
        return false;
    }

    bool emitBlock(const RegBlock& block, std::string& reason)
    {
        out_ << "b" << block.id.value << ":\n";
        for (const auto& instruction : block.instructions) {
            if (!emitInstruction(instruction, reason)) return false;
        }
        return std::visit(
            [&](const auto& terminator) -> bool {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    out_ << "  br label " << edgeTarget(terminator.target) << "\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    std::string condition;
                    if (!emitOperand(terminator.condition, condition, reason)) return false;
                    if (terminator.condition.type != SSAType::BOOL) {
                        if (terminator.condition.type != SSAType::I32 &&
                            terminator.condition.type != SSAType::I64) {
                            reason = "LLVM IR branch condition must be boolean or integer";
                            return false;
                        }
                        const std::string normalized = nextTemporary();
                        out_ << "  " << normalized << " = icmp ne " << llvmType(terminator.condition.type)
                             << " " << condition << ", 0\n";
                        condition = normalized;
                    }
                    out_ << "  br i1 " << condition << ", label " << edgeTarget(terminator.trueTarget)
                         << ", label " << edgeTarget(terminator.falseTarget) << "\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    if (terminator.value.type != SSAType::I32) {
                        reason = "LLVM IR first slice only supports i32 switch values";
                        return false;
                    }
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  switch i32 " << value << ", label "
                         << edgeTarget(terminator.defaultTarget) << " [\n";
                    for (const auto& entry : terminator.cases) {
                        out_ << "    i32 " << entry.key << ", label " << edgeTarget(entry.target)
                             << "\n";
                    }
                    out_ << "  ]\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (returnType_ == SSAType::VOID) {
                        if (terminator.hasValue) {
                            reason = "LLVM IR void method returned a value";
                            return false;
                        }
                        out_ << "  ret void\n\n";
                        return true;
                    }
                    if (!terminator.hasValue || terminator.value.type != returnType_) {
                        reason = "LLVM IR return type does not match method descriptor";
                        return false;
                    }
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  ret " << llvmType(returnType_) << " " << value << "\n\n";
                    return true;
                }
                else {
                    reason = "LLVM IR encountered an unsupported terminator after validation";
                    return false;
                }
            },
            block.terminator);
    }

    const RegFunction&       function_;
    const RegisterTypes&     registerTypes_;
    const std::string&       functionName_;
    SSAType                  returnType_;
    std::ostringstream       out_;
    uint32_t                 temporaryIndex_ = 0;
    uint32_t                 edgeIndex_ = 0;
    std::vector<PendingEdge> pendingEdges_;
};

bool supportsMethodDescriptor(const MethodNode& method, SSAType& resultType, std::string& reason)
{
    if ((method.access & Opcodes::ACC_STATIC) == 0) {
        reason = "LLVM IR first slice supports static methods only";
        return false;
    }
    const std::string descriptor = method.desc.to_std_string();
    resultType = primitiveToSsaType(returnKind(descriptor));
    if (resultType == SSAType::UNKNOWN) {
        reason = "LLVM IR first slice only supports int/long/float/double/void returns";
        return false;
    }
    for (const PrimitiveKind kind : argumentKinds(descriptor)) {
        if (primitiveToSsaType(kind) == SSAType::UNKNOWN) {
            reason = "LLVM IR first slice only supports int/long/float/double parameters";
            return false;
        }
    }
    return true;
}

std::string helperName(const std::string& className, const MethodNode& method)
{
    return "mj2x_llvm_" + mangleMethod(className,
                                        method.name.to_std_string(),
                                        method.desc.to_std_string());
}

std::string emitJniWrapper(const std::string& className, const MethodNode& method,
                           const std::string& helper, SSAType returnType)
{
    const std::string descriptor = method.desc.to_std_string();
    const auto        arguments  = argumentKinds(descriptor);
    std::ostringstream out;
    out << "define dllexport " << llvmType(returnType) << " @"
        << mangleMethod(className, method.name.to_std_string(), descriptor) << "(ptr %env, ptr %clazz";
    for (size_t index = 0; index < arguments.size(); ++index) {
        out << ", " << llvmType(primitiveToSsaType(arguments[index])) << " %p" << index;
    }
    out << ") {\nentry:\n";
    if (returnType == SSAType::VOID) {
        out << "  call void @" << helper << "(";
    }
    else {
        out << "  %result = call " << llvmType(returnType) << " @" << helper << "(";
    }
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) out << ", ";
        out << llvmType(primitiveToSsaType(arguments[index])) << " %p" << index;
    }
    out << ")\n";
    if (returnType == SSAType::VOID) {
        out << "  ret void\n";
    }
    else {
        out << "  ret " << llvmType(returnType) << " %result\n";
    }
    out << "}\n\n";
    return out.str();
}

}   // namespace

struct LlvmIrBackend::Impl
{
    std::ostringstream bodies;
};

LlvmIrBackend::LlvmIrBackend()
    : impl_(std::make_unique<Impl>())
{}

LlvmIrBackend::~LlvmIrBackend() = default;

BackendResult LlvmIrBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                           const std::vector<Block*>& blocks)
{
    BackendResult result;
    if (method.methodNode == nullptr) {
        result.text = "LLVM IR lowering failed: missing method node";
        return result;
    }

    SSAType     resultType = SSAType::UNKNOWN;
    std::string reason;
    if (!supportsMethodDescriptor(*method.methodNode, resultType, reason)) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    SSABuilder builder;
    auto ssa = builder.build(blocks,
                             className,
                             method.methodNode->name.to_std_string(),
                             method.methodNode->desc.to_std_string(),
                             true,
                             method.methodNode->maxLocals,
                             reason);
    if (!ssa) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    RegisterTypes registerTypes;
    if (!collectRegisterTypes(*reg, registerTypes, reason)) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    const std::string helper = helperName(className, *method.methodNode);
    std::string       helperSource;
    LlvmFunctionEmitter emitter(*reg, registerTypes, helper, resultType);
    if (!emitter.emit(helperSource, reason)) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = helperSource + emitJniWrapper(className, *method.methodNode, helper, resultType);
    impl_->bodies << result.text;
    return result;
}

void LlvmIrBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                    const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::C_SOURCE || method.methodNode == nullptr) return;
    method.methodNode->access |= Opcodes::ACC_NATIVE;
    method.methodNode->instructions.clear();
    clazz.modified = true;
}

void LlvmIrBackend::finalize(const CompilerIndex&)
{
    std::ostringstream full;
    full << "; ModuleID = 'mj2x_llvm'\n";
    full << "source_filename = \"mj2x_llvm\"\n\n";
    full << impl_->bodies.str();
    sourceCode_ = full.str();
    if (llvmOutputPath_.empty()) return;

    std::ofstream output(llvmOutputPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        fprintf(stderr, "[LlvmIrBackend] ERROR: cannot open output: %s\n", llvmOutputPath_.c_str());
        return;
    }
    output.write(sourceCode_.data(), static_cast<std::streamsize>(sourceCode_.size()));
    if (!output) {
        fprintf(stderr, "[LlvmIrBackend] ERROR: cannot write output: %s\n", llvmOutputPath_.c_str());
    }
}

}   // namespace mj2x

#include "llvm_ir_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/frontend/code_converter.hpp"
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
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include "libca/log/log_macros.hpp"

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

/// @brief JVM 窄类型统一为 i32 计算位宽（boolean/byte/short/char 栈语义）。
SSAType widenNarrowType(SSAType type)
{
    return (type == SSAType::BOOL || type == SSAType::I8 || type == SSAType::I16 ||
            type == SSAType::CHAR)
               ? SSAType::I32
               : type;
}

const char* llvmTypeWide(SSAType type)
{
    return llvmType(widenNarrowType(type));
}

bool hasObservableEffect(const SSAEffect& effect)
{
    return effect.mayThrow || effect.readsMemory || effect.writesMemory ||
           effect.readsExternalState || effect.writesExternalState || effect.callsExternal ||
           effect.resolvesExternal || effect.isBarrier;
}

PrimitiveKind returnKind(const std::string& desc)
{
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str())).getDescriptor());
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
    case PrimitiveKind::BOOLEAN:
    case PrimitiveKind::BYTE:
    case PrimitiveKind::SHORT:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::INT: return SSAType::I32;   // JVM 窄类型栈位宽 = int
    case PrimitiveKind::LONG: return SSAType::I64;
    case PrimitiveKind::FLOAT: return SSAType::F32;
    case PrimitiveKind::DOUBLE: return SSAType::F64;
    case PrimitiveKind::VOID: return SSAType::VOID;
    default: return SSAType::UNKNOWN;
    }
}

/// @brief JNI 边界类型（native 方法签名；窄类型为 jboolean/jbyte/jchar/jshort）。
const char* jniBoundaryType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN:
    case PrimitiveKind::BYTE: return "i8";
    case PrimitiveKind::CHAR:
    case PrimitiveKind::SHORT: return "i16";
    case PrimitiveKind::INT: return "i32";
    case PrimitiveKind::LONG: return "i64";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    default: return nullptr;
    }
}

bool recordValue(RegisterTypes& types, RegValue value, std::string& reason)
{
    value.type = widenNarrowType(value.type);
    if (!isSupportedValueType(value.type)) {
        reason = "LLVM IR only supports scalar i32/i64/f32/f64 values";
        return false;
    }
    const auto [it, inserted] = types.emplace(value.id.value, value.type);
    // 冲突检测按计算位宽：BOOL（i1 谓词槽）与 I32 视为同一计算类型。
    if (!inserted && widenNarrowType(it->second) != value.type) {
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
        if (!isSupportedValueType(widenNarrowType(operand.type))) {
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
        case SSABinaryOp::Shl:
        case SSABinaryOp::Shr:
        case SSABinaryOp::UShr:
            // LLVM shl/ashr/lshr treat the shift amount as unsigned modulo the
            // type width, which matches Java's mask-to-5/6-bits semantics.
            return true;
        case SSABinaryOp::Div:
        case SSABinaryOp::Rem:
            // 发射端生成除零检查（抛 ArithmeticException）+ udiv/urem 位模式
            // 等价（规避 sdiv INT_MIN / -1 的溢出 poison）。
            return true;
        }
    }
    reason = "LLVM IR binary operation has an unsupported value type";
    return false;
}

/// @brief Resolved intra-class static call target for the LLVM IR slice.
struct CallTarget
{
    std::string          symbol;   ///< internal helper symbol（@mj2x_llvm_Java_...）
    SSAType              returnType = SSAType::UNKNOWN;
    std::vector<SSAType> argTypes;
};

/// @brief 解析并预检 LLVM IR 切片内的静态互调目标（成员定义见文件尾部）。
///
/// @details 目标必须满足：INVOKESTATIC、目标类在编译索引内（非 library）、
/// 签名纯标量、方法体可转换（递归预检，含嵌套调用）；结果按 MethodID 缓存。
/// 跨类与递归/互递归均允许（符号确定性生成，不悬空）。
class InternalCallResolver
{
public:
    InternalCallResolver(CompilerIndex& index, const std::string& className)
        : index_(index)
        , className_(className)
    {}

    bool resolve(const SSAMethodRef& method, CallTarget& target, std::string& reason);

private:
    CompilerMethod* findTarget(const MethodID& id, std::string& reason);
    bool            fillTargetFromDescriptor(const std::string& owner, MethodNode& node,
                                             CallTarget& target, std::string& reason);
    bool            analyze(const std::string& owner, MethodNode& node, CallTarget& target,
                            std::string& reason);

    CompilerIndex&                     index_;
    const std::string&                 className_;
    std::unordered_map<MethodID, CallTarget> cache_;
    std::unordered_set<MethodID>       visiting_;
};

bool recordInstruction(RegisterTypes& types, const RegInstruction& instruction,
                       InternalCallResolver* resolver, std::string& reason)
{
    // 内部静态调用经 resolver 单独验证；整数 div/rem 的 mayThrow 由发射端的
    // 除零检查（throw-arithmetic helper）处理。其余指令保持 fail-closed 检查。
    const bool throwHandledByEmitter =
        instruction.kind == RegInstructionKind::Call ||
        (instruction.kind == RegInstructionKind::Binary &&
         (instruction.binaryOp == SSABinaryOp::Div || instruction.binaryOp == SSABinaryOp::Rem));
    if (!throwHandledByEmitter && hasObservableEffect(instruction.effect)) {
        reason = "LLVM IR first slice rejects instructions with observable effects";
        return false;
    }
    switch (instruction.kind) {
    case RegInstructionKind::Copy:
        return recordValue(types, instruction.copy.dest, reason) &&
               recordOperand(types, instruction.copy.source, reason) &&
               instruction.copy.dest.type == instruction.copy.source.type;
    case RegInstructionKind::Unary:
        if (instruction.unaryOp == SSAUnaryOp::BitNot) {
            if (instruction.dest.type != SSAType::I32 && instruction.dest.type != SSAType::I64) {
                reason = "LLVM IR bitwise negation requires an integer type";
                return false;
            }
            return recordValue(types, instruction.dest, reason) &&
                   recordOperand(types, instruction.value, reason);
        }
        if (instruction.unaryOp != SSAUnaryOp::Neg) {
            reason = "LLVM IR first slice only supports arithmetic negation and bitwise not";
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
        if (!recordValue(types, instruction.dest, reason)) return false;
        // Compare 结果是 i1 谓词（分支条件），覆盖宽化后的 i32 槽类型。
        types[instruction.dest.id.value] = SSAType::BOOL;
        return recordOperand(types, instruction.lhs, reason) &&
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
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier:
        reason = "LLVM IR first slice rejects object, JNI, memory and monitor operations";
        return false;
    case RegInstructionKind::Call:
        if (resolver == nullptr) {
            reason = "LLVM IR static calls require a compiler index";
            return false;
        }
        {
            CallTarget target;
            if (!resolver->resolve(instruction.method, target, reason)) return false;
            if (instruction.hasDest) {
                if (!recordValue(types, instruction.dest, reason)) return false;
                if (instruction.dest.type != target.returnType) {
                    reason = "LLVM IR call result type does not match the target descriptor";
                    return false;
                }
            }
            else if (target.returnType != SSAType::VOID) {
                reason = "LLVM IR call discards a non-void result";
                return false;
            }
            if (instruction.args.size() != target.argTypes.size()) {
                reason = "LLVM IR call argument count does not match the target descriptor";
                return false;
            }
            for (size_t index = 0; index < instruction.args.size(); ++index) {
                if (!recordOperand(types, instruction.args[index], reason)) return false;
                if (instruction.args[index].type != target.argTypes[index]) {
                    reason = "LLVM IR call argument type does not match the target descriptor";
                    return false;
                }
            }
        }
        return true;
    }
    reason = "LLVM IR encountered an unknown RegIR instruction";
    return false;
}

bool collectRegisterTypes(const RegFunction& function, RegisterTypes& types,
                          InternalCallResolver* resolver, std::string& reason)
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
            if (!recordInstruction(types, instruction, resolver, reason)) return false;
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
                        const std::string& functionName, SSAType returnType,
                        InternalCallResolver* resolver)
        : function_(function)
        , registerTypes_(registerTypes)
        , functionName_(functionName)
        , returnType_(returnType)
        , resolver_(resolver)
    {}

    bool emit(std::string& source, std::string& reason)
    {
        const char* returnLlvmType = llvmType(returnType_);
        if (returnLlvmType == nullptr || function_.entry == nullptr) {
            reason = "LLVM IR function has no supported return type or entry block";
            return false;
        }

        out_ << "define internal " << returnLlvmType << " @" << functionName_ << "(ptr %env";
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            out_ << ", " << llvmTypeWide(function_.parameters[index].type) << " %p" << index;
        }
        out_ << ") {\nentry:\n";
        for (const auto& [id, type] : registerTypes_) {
            out_ << "  %slot_r" << id << " = alloca " << llvmType(type) << "\n";
        }
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            const auto& parameter = function_.parameters[index];
            out_ << "  store " << llvmTypeWide(parameter.type) << " %p" << index << ", ptr %slot_r"
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
        if (operand.kind == RegOperandKind::Register) {
            // 槽类型是权威（窄参数宽化为 i32；Compare 结果为 i1 谓词）。
            const auto it = registerTypes_.find(operand.reg.value);
            const char* type = it == registerTypes_.end() ? nullptr : llvmType(it->second);
            if (type == nullptr) {
                reason = "LLVM IR encountered an unsupported operand type";
                return false;
            }
            const std::string temporary = nextTemporary();
            out_ << "  " << temporary << " = load " << type << ", ptr " << slotName(operand.reg)
                 << "\n";
            expression = temporary;
            return true;
        }
        const char* type = llvmTypeWide(operand.type);
        if (type == nullptr) {
            reason = "LLVM IR encountered an unsupported operand type";
            return false;
        }
        if (operand.kind == RegOperandKind::IntLiteral) {
            expression = std::to_string(operand.intValue);
            if (operand.type == SSAType::BOOL) expression = operand.intValue == 0 ? "0" : "1";
            return true;
        }
        if (operand.kind == RegOperandKind::FloatLiteral) {
            // NaN / ±Inf / -0.0 用 LLVM hex 字面量（十进制形式无法表示）；
            // 正常值保持十进制 scientific。
            const double value = operand.floatValue;
            std::string  bits;
            if (operand.type == SSAType::F32) {
                if (std::isnan(value)) bits = "0x7FC00000";
                else if (std::isinf(value)) bits = std::signbit(value) ? "0xFF800000" : "0x7F800000";
                else if (value == 0.0 && std::signbit(value)) bits = "0x80000000";
            }
            else if (operand.type == SSAType::F64) {
                if (std::isnan(value)) bits = "0x7FF8000000000000";
                else if (std::isinf(value))
                    bits = std::signbit(value) ? "0xFFF0000000000000" : "0x7FF0000000000000";
                else if (value == 0.0 && std::signbit(value)) bits = "0x8000000000000000";
            }
            else {
                reason = "LLVM IR floating literal has a non-floating type";
                return false;
            }
            if (!bits.empty()) {
                // 操作数位置不带类型前缀（LLVM 按指令上下文推断 float 类型）。
                expression = bits;
                return true;
            }
            std::ostringstream literal;
            literal << std::scientific << std::showpoint;
            if (operand.type == SSAType::F32) {
                literal << std::setprecision(9) << static_cast<float>(operand.floatValue);
            }
            else {
                literal << std::setprecision(17) << operand.floatValue;
            }
            expression = literal.str();
            return true;
        }
        reason = "LLVM IR first slice rejects reference and opaque operands";
        return false;
    }

    bool emitStore(RegValue destination, const std::string& expression, std::string& reason)
    {
        // 槽类型是权威（窄结果 i32；Compare 结果 i1 谓词）。
        const auto it = registerTypes_.find(destination.id.value);
        const char* type = it == registerTypes_.end() ? nullptr : llvmType(it->second);
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
            const char* type = llvmTypeWide(instruction.dest.type);
            const std::string result = nextTemporary();
            if (instruction.unaryOp == SSAUnaryOp::BitNot) {
                out_ << "  " << result << " = xor " << type << " " << value << ", -1\n";
            }
            else if (instruction.dest.type == SSAType::F32 || instruction.dest.type == SSAType::F64) {
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
            if (!floating &&
                (instruction.binaryOp == SSABinaryOp::Div ||
                 instruction.binaryOp == SSABinaryOp::Rem)) {
                // 整数 div/rem：udiv/urem（位模式等价于有符号除法，且无
                // INT_MIN/-1 溢出 poison）；除零走共享 throw-arithmetic helper
                // 抛 ArithmeticException 后返回 0（JVM 检查 pending exception）。
                const std::string divIndex = std::to_string(divIndex_++);
                const std::string isZero   = nextTemporary();
                const std::string result   = nextTemporary();
                const char*       type     = llvmTypeWide(instruction.dest.type);
                out_ << "  " << isZero << " = icmp eq " << type << " " << rhs << ", 0\n";
                out_ << "  br i1 " << isZero << ", label %b_divz_" << divIndex
                     << ", label %b_divok_" << divIndex << "\n";
                out_ << "b_divok_" << divIndex << ":\n";
                out_ << "  " << result << " = "
                     << (instruction.binaryOp == SSABinaryOp::Div ? "udiv" : "urem") << " "
                     << type << " " << lhs << ", " << rhs << "\n";
                if (!emitStore(instruction.dest, result, reason)) return false;
                out_ << "  br label %b_divafter_" << divIndex << "\n";
                out_ << "b_divz_" << divIndex << ":\n";
                out_ << "  call void @mj2x_llvm_throw_arith(ptr %env)\n";
                out_ << "  ret " << type << " 0\n";
                out_ << "b_divafter_" << divIndex << ":\n";
                return true;
            }
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
            case SSABinaryOp::Shl: opcode = floating ? nullptr : "shl"; break;
            case SSABinaryOp::Shr: opcode = floating ? nullptr : "ashr"; break;
            case SSABinaryOp::UShr: opcode = floating ? nullptr : "lshr"; break;
            }
            if (opcode == nullptr) {
                reason = "LLVM IR binary operation is outside the pure scalar subset";
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << opcode << " " << llvmTypeWide(instruction.dest.type)
                 << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Call) {
            CallTarget target;
            if (resolver_ == nullptr || !resolver_->resolve(instruction.method, target, reason)) {
                reason = "LLVM IR call target was not validated before emission";
                return false;
            }
            // helper 首参为 JNIEnv*（%env），后续为 Java 参数。
            std::string args = "ptr %env";
            for (size_t index = 0; index < instruction.args.size(); ++index) {
                std::string expression;
                if (!emitOperand(instruction.args[index], expression, reason)) return false;
                args += ", " + std::string(llvmType(target.argTypes[index])) + " " + expression;
            }
            if (target.returnType == SSAType::VOID) {
                out_ << "  call void @" << target.symbol << "(" << args << ")\n";
                return true;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call " << llvmType(target.returnType) << " @"
                 << target.symbol << "(" << args << ")\n";
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
                 << " " << llvmTypeWide(instruction.lhs.type) << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Convert) {
            if (!emitOperand(instruction.value, value, reason)) return false;
            const SSAType sourceType = widenNarrowType(instruction.value.type);
            const SSAType targetType = instruction.targetType;
            // i2b/i2s/i2c：目标为窄类型，trunc 后按符号/零扩展回 i32。
            if (targetType == SSAType::I8 || targetType == SSAType::I16 ||
                targetType == SSAType::CHAR) {
                if (sourceType != SSAType::I32) {
                    reason = "LLVM IR narrowing conversion requires an i32 source";
                    return false;
                }
                const char*       narrow = targetType == SSAType::CHAR ? "i16" : "i8";
                const std::string trunc  = nextTemporary();
                const std::string ext    = nextTemporary();
                const bool        signed_ = targetType != SSAType::CHAR;
                out_ << "  " << trunc << " = trunc i32 " << value << " to " << narrow << "\n";
                out_ << "  " << ext << " = " << (signed_ ? "sext" : "zext") << " " << narrow
                     << " " << trunc << " to i32\n";
                return emitStore(instruction.dest, ext, reason);
            }
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
                // 浮点 → 整数：llvm.fptosi.sat（NaN→0、溢出饱和，匹配 Java f2i/f2l
                // 语义；LLVM fptosi 对 NaN/溢出是 poison，不可直接用）。
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call "
                     << (targetType == SSAType::I32 ? "i32" : "i64")
                     << " @llvm.fptosi.sat."
                     << (targetType == SSAType::I32 ? "i32" : "i64") << "."
                     << (sourceType == SSAType::F32 ? "f32" : "f64") << "("
                     << llvmType(sourceType) << " " << value << ")\n";
                return emitStore(instruction.dest, result, reason);
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
                        out_ << "  " << normalized << " = icmp ne " << llvmTypeWide(terminator.condition.type)
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
                    if (!terminator.hasValue ||
                        widenNarrowType(terminator.value.type) != returnType_) {
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
    InternalCallResolver*    resolver_;
    std::ostringstream       out_;
    uint32_t                 temporaryIndex_ = 0;
    uint32_t                 edgeIndex_ = 0;
    uint32_t                 divIndex_ = 0;
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

/// @brief 解析并预检 LLVM IR 切片内的静态互调目标（类声明见前部）。
///
/// @details 目标须 INVOKESTATIC + 目标类在编译索引内（非 library）+ markedForCompile
/// + 纯标量签名 + 方法体可转换。跨类与递归（互递归）均允许：符号由 owner/name/desc
/// 确定性生成，环上各方法的非递归部分仍各自验证。
bool InternalCallResolver::resolve(const SSAMethodRef& method, CallTarget& target,
                                   std::string& reason)
{
    if (method.invokeOpcode != Opcodes::INVOKESTATIC) {
        reason = "LLVM IR first slice only supports static method calls";
        return false;
    }
    const std::string owner = method.owner.to_std_string();
    const MethodID id{owner, method.name.to_std_string(), method.desc.to_std_string()};
    const auto     cached = cache_.find(id);
    if (cached != cache_.end()) {
        target = cached->second;
        return true;
    }
    CompilerMethod* targetMethod = findTarget(id, reason);
    if (targetMethod == nullptr || targetMethod->methodNode == nullptr) return false;

    if (targetMethod->backendResult.kind == BackendResult::Kind::C_SOURCE) {
        // 已编译成功：方法体已被 injectIntoClass 清空，不能再预检；成功即已通过
        // 完整验证，签名直接从描述解析。
        CallTarget parsed;
        if (!fillTargetFromDescriptor(owner, *targetMethod->methodNode, parsed, reason))
            return false;
        cache_.emplace(id, parsed);
        target = parsed;
        return true;
    }
    if (!targetMethod->backendResult.text.empty()) {
        // 已尝试编译且 fail-closed：调用方必须保持字节码。
        reason = "LLVM IR call target failed to compile";
        return false;
    }

    // 递归/互递归：目标仍在预检中（visiting）。符号确定性生成不会悬空，按描述
    // 接受即可；环上各方法的非递归部分由各自的 analyze 负责验证。
    if (!visiting_.insert(id).second) {
        CallTarget parsed;
        if (!fillTargetFromDescriptor(owner, *targetMethod->methodNode, parsed, reason))
            return false;
        cache_.emplace(id, parsed);
        target = parsed;
        return true;
    }

    // 未编译且非递归：字节码完好，完整预检（含嵌套调用）。
    if (!analyze(owner, *targetMethod->methodNode, target, reason)) {
        visiting_.erase(id);
        return false;
    }
    visiting_.erase(id);
    cache_.emplace(id, target);
    return true;
}

CompilerMethod* InternalCallResolver::findTarget(const MethodID& id, std::string& reason)
{
    CompilerClass* clazz = index_.getClass(id.owner);
    if (clazz == nullptr) {
        reason = "LLVM IR call target class is not in the compilation index";
        return nullptr;
    }
    if (clazz->isLibrary) {
        reason = "LLVM IR call target class is a library class and is not compiled";
        return nullptr;
    }
    for (auto& candidate : clazz->methods) {
        if (!candidate.markedForCompile || candidate.methodNode == nullptr) continue;
        if (candidate.methodNode->name.to_std_string() == id.name &&
            candidate.methodNode->desc.to_std_string() == id.desc) {
            return &candidate;
        }
    }
    reason = "LLVM IR call target is not a marked-for-compile method in this class";
    return nullptr;
}

bool InternalCallResolver::fillTargetFromDescriptor(const std::string& owner, MethodNode& node,
                                                    CallTarget& target, std::string& reason)
{
    SSAType resultType = SSAType::UNKNOWN;
    if (!supportsMethodDescriptor(node, resultType, reason)) return false;
    target.symbol     = helperName(owner, node);
    target.returnType = resultType;
    for (const PrimitiveKind kind : argumentKinds(node.desc.to_std_string())) {
        target.argTypes.push_back(primitiveToSsaType(kind));
    }
    return true;
}

/// @brief 目标方法体级预检（不发射）：复用主编译链的 SSA/RegIR 验证。
/// @param owner 目标方法所属类（跨类调用时符号与 SSA 上下文使用目标类名）。
bool InternalCallResolver::analyze(const std::string& owner, MethodNode& node, CallTarget& target,
                                   std::string& reason)
{
    SSAType resultType = SSAType::UNKNOWN;
    if (!supportsMethodDescriptor(node, resultType, reason)) return false;
    CodeConverter converter(index_.arena());
    auto          blocks = converter.convert(node);
    SSABuilder    builder;
    ca::str::Utf8StringArena ssaArena;   // SSA 派生串入池，活到 ssa 消费完（本函数内同步）
    auto ssa = builder.build(blocks,
                             owner,
                             node.name.to_std_string(),
                             node.desc.to_std_string(),
                             true,
                             node.maxLocals,
                             ssaArena,
                             reason);
    if (!ssa) return false;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return false;
    RegisterTypes types;
    if (!collectRegisterTypes(*reg, types, this, reason)) return false;
    target.symbol     = helperName(owner, node);
    target.returnType = resultType;
    for (const PrimitiveKind kind : argumentKinds(node.desc.to_std_string())) {
        target.argTypes.push_back(primitiveToSsaType(kind));
    }
    return true;
}

/// @brief helper 参数类型：JVM 栈位宽（窄类型提升为 i32）。
const char* calcArgType(PrimitiveKind kind)
{
    return llvmType(primitiveToSsaType(kind));
}

std::string emitJniWrapper(const std::string& className, const MethodNode& method,
                           const std::string& helper, SSAType /*returnType*/)
{
    const std::string             descriptor = method.desc.to_std_string();
    const std::vector<PrimitiveKind> arguments = argumentKinds(descriptor);
    const PrimitiveKind          retKind   = returnKind(descriptor);
    const char*                  boundaryRet = jniBoundaryType(retKind);
    const char*                  calcRet    = llvmType(primitiveToSsaType(retKind));
    std::ostringstream           out;
    out << "define dllexport " << boundaryRet << " @"
        << mangleMethod(className, method.name.to_std_string(), descriptor) << "(ptr %env, ptr %clazz";
    for (size_t index = 0; index < arguments.size(); ++index) {
        out << ", " << jniBoundaryType(arguments[index]) << " %p" << index;
    }
    out << ") {\nentry:\n";
    // 窄参数提升到 i32 计算位宽（byte/short 符号扩展，boolean/char 零扩展）。
    for (size_t index = 0; index < arguments.size(); ++index) {
        const PrimitiveKind kind  = arguments[index];
        const std::string   btype = jniBoundaryType(kind);
        if (btype == "i8" || btype == "i16") {
            const std::string ext = (kind == PrimitiveKind::BYTE || kind == PrimitiveKind::SHORT)
                                        ? "sext"
                                        : "zext";
            out << "  %w" << index << " = " << ext << " " << btype << " %p" << index
                << " to i32\n";
        }
    }
    if (retKind == PrimitiveKind::VOID) {
        out << "  call void @" << helper << "(ptr %env";
    }
    else {
        out << "  %result = call " << calcRet << " @" << helper << "(ptr %env";
    }
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::string btype = jniBoundaryType(arguments[index]);
        const bool        narrow = btype == "i8" || btype == "i16";
        out << ", " << calcArgType(arguments[index]) << " "
            << (narrow ? ("%w" + std::to_string(index)) : ("%p" + std::to_string(index)));
    }
    out << ")\n";
    if (retKind == PrimitiveKind::VOID) {
        out << "  ret void\n";
    }
    else if (std::string(boundaryRet) == "i8" || std::string(boundaryRet) == "i16") {
        out << "  %t = trunc i32 %result to " << boundaryRet << "\n";
        out << "  ret " << boundaryRet << " %t\n";
    }
    else {
        out << "  ret " << boundaryRet << " %result\n";
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
                                           const std::vector<Block*>& blocks,
                                           CompilerIndex* index)
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

    SSABuilder             builder;
    ca::str::Utf8StringArena ssaArena;   // SSA 派生串入池，活到 ssa 消费完（本函数内同步）
    auto ssa = builder.build(blocks,
                             className,
                             method.methodNode->name.to_std_string(),
                             method.methodNode->desc.to_std_string(),
                             true,
                             method.methodNode->maxLocals,
                             ssaArena,
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
    // 互调需要编译器索引（单元测试不传 index 时行为与以前一致：静态调用 fail-closed）。
    std::unique_ptr<InternalCallResolver> callResolver;
    if (index != nullptr) {
        callResolver = std::make_unique<InternalCallResolver>(*index, className);
    }
    if (!collectRegisterTypes(*reg, registerTypes, callResolver.get(), reason)) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    const std::string helper = helperName(className, *method.methodNode);
    std::string       helperSource;
    LlvmFunctionEmitter emitter(*reg, registerTypes, helper, resultType, callResolver.get());
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

/// @brief 共享整数除零异常 helper：JNI FindClass + ThrowNew(ArithmeticException)。
///
/// @details JNIEnv 函数表槽位（jni.h 布局，0 基）：FindClass=6、ThrowNew=14。
/// 除零路径调用后返回 0，JVM 侧检查 pending exception 抛给调用者。
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

void LlvmIrBackend::finalize(const CompilerIndex&)
{
    std::ostringstream full;
    full << "; ModuleID = 'mj2x_llvm'\n";
    full << "source_filename = \"mj2x_llvm\"\n\n";
    full << impl_->bodies.str();
    if (impl_->bodies.str().find("mj2x_llvm_throw_arith") != std::string::npos) {
        full << emitThrowArithmeticHelper();
    }
    sourceCode_ = full.str();
    if (llvmOutputPath_.empty()) return;

    std::ofstream output(llvmOutputPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        CA_LOGT_ERROR("llvm", "cannot open output: {}\n", llvmOutputPath_.c_str());
        return;
    }
    output.write(sourceCode_.data(), static_cast<std::streamsize>(sourceCode_.size()));
    if (!output) {
        CA_LOGT_ERROR("llvm", "cannot write output: {}\n", llvmOutputPath_.c_str());
    }
}

}   // namespace mj2x

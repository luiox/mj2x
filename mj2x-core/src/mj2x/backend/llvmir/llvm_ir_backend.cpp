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
    case SSAType::REF: return "ptr";   // JNI 引用（jobject/jclass/jstring/数组）
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
    case PrimitiveKind::OBJECT: return SSAType::REF;
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
    case PrimitiveKind::OBJECT: return "ptr";
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
        return isSupportedValueType(operand.type);   // ptr null（引用类型）
    case RegOperandKind::Opaque:
        // 空操作数（static 字段无 receiver 等）跳过；ldc_string:/ldc_class:
        // 前缀的 Opaque（String/Class 常量）由发射端构造；其余拒绝。
        if (operand.type == SSAType::UNKNOWN) return true;
        if (operand.tag.rfind("ldc_string:", 0) == 0 ||
            operand.tag.rfind("ldc_class:", 0) == 0) {
            return isSupportedValueType(operand.type);
        }
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
    // 除零检查（throw-arithmetic helper）处理；字段/数组/分配/类型检查/监控
    // 的 mayThrow 与内存效应由对应 JNI helper 处理（发射端）。其余指令保持
    // fail-closed 检查。
    const bool throwHandledByEmitter =
        instruction.kind == RegInstructionKind::Call ||
        instruction.kind == RegInstructionKind::LoadField ||
        instruction.kind == RegInstructionKind::StoreField ||
        instruction.kind == RegInstructionKind::ArrayLoad ||
        instruction.kind == RegInstructionKind::ArrayStore ||
        instruction.kind == RegInstructionKind::Allocate ||
        instruction.kind == RegInstructionKind::Intrinsic ||
        (instruction.kind == RegInstructionKind::Convert &&
         instruction.convertKind == SSAConvertKind::ReferenceCast) ||
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
               widenNarrowType(instruction.copy.dest.type) ==
                   widenNarrowType(instruction.copy.source.type);
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
        if (instruction.convertKind != SSAConvertKind::Numeric &&
            instruction.convertKind != SSAConvertKind::ReferenceCast) {
            reason = "LLVM IR first slice rejects reference casts";
            return false;
        }
        return recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.value, reason);
    case RegInstructionKind::LoadField:
        if (!recordValue(types, instruction.dest, reason)) return false;
        if (instruction.receiver.kind != RegOperandKind::Opaque ||
            instruction.receiver.type != SSAType::UNKNOWN) {
            return recordOperand(types, instruction.receiver, reason);
        }
        return true;
    case RegInstructionKind::StoreField:
        if (instruction.receiver.kind != RegOperandKind::Opaque ||
            instruction.receiver.type != SSAType::UNKNOWN) {
            if (!recordOperand(types, instruction.receiver, reason)) return false;
        }
        return recordOperand(types, instruction.value, reason);
    case RegInstructionKind::ArrayLoad:
        return recordValue(types, instruction.dest, reason) &&
               recordOperand(types, instruction.receiver, reason) &&
               recordOperand(types, instruction.index, reason);
    case RegInstructionKind::ArrayStore:
        return recordOperand(types, instruction.receiver, reason) &&
               recordOperand(types, instruction.index, reason) &&
               recordOperand(types, instruction.value, reason);
    case RegInstructionKind::Allocate:
        if (!recordValue(types, instruction.dest, reason)) return false;
        for (const auto& arg : instruction.args) {
            if (!recordOperand(types, arg, reason)) return false;
        }
        return true;
    case RegInstructionKind::Intrinsic:
        // 与发射端一致的按名过滤：array.length / instanceof:<desc> /
        // monitor.enter / monitor.exit 放行；其余保持 fail-closed。
        if (instruction.intrinsicName != "array.length" &&
            instruction.intrinsicName.rfind("instanceof:", 0) != 0 &&
            instruction.intrinsicName != "monitor.enter" &&
            instruction.intrinsicName != "monitor.exit") {
            reason = "LLVM IR first slice rejects this intrinsic";
            return false;
        }
        if (instruction.hasDest && !recordValue(types, instruction.dest, reason)) return false;
        for (const auto& arg : instruction.args) {
            if (!recordOperand(types, arg, reason)) return false;
        }
        return true;
    case RegInstructionKind::Barrier:
        reason = "LLVM IR first slice rejects object, JNI, memory and monitor operations";
        return false;
    case RegInstructionKind::Call:
        {
            // 参数/返回类型按方法描述符解析；实例调用 args[0] 为 receiver。
            // 同模块 direct call 判定仅在 resolver 可用时进行（JNI 路径不依赖）。
            const std::string              desc = instruction.method.desc.to_std_string();
            const std::vector<PrimitiveKind> argKinds = argumentKinds(desc);
            const size_t receiverOffset =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC ? 0 : 1;
            if (instruction.args.size() != argKinds.size() + receiverOffset) {
                reason = "LLVM IR call argument count does not match the target descriptor";
                return false;
            }
            if (instruction.hasDest) {
                if (!recordValue(types, instruction.dest, reason)) return false;
                const SSAType expected = widenNarrowType(primitiveToSsaType(returnKind(desc)));
                if (widenNarrowType(instruction.dest.type) != expected) {
                    reason = "LLVM IR call result type does not match the target descriptor";
                    return false;
                }
            }
            else if (returnKind(desc) != PrimitiveKind::VOID) {
                reason = "LLVM IR call discards a non-void result";
                return false;
            }
            if (receiverOffset == 1) {
                if (!recordOperand(types, instruction.args[0], reason)) return false;
                if (instruction.args[0].type != SSAType::REF) {
                    reason = "LLVM IR instance call receiver must be a reference";
                    return false;
                }
            }
            for (size_t index = 0; index < argKinds.size(); ++index) {
                const auto& arg = instruction.args[index + receiverOffset];
                if (!recordOperand(types, arg, reason)) return false;
                if (widenNarrowType(arg.type) !=
                    widenNarrowType(primitiveToSsaType(argKinds[index]))) {
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
    // 异常区域自 Phase 5 起支持：异常边拷贝/exceptionValue 由发射端处理；
    // handler 的块参数照常经 copies 校验。
    for (const auto& parameter : function.parameters) {
        if (!recordValue(types, parameter, reason)) return false;
    }
    // 异常区域的 exceptionValue（handler 的合成异常寄存器）需占槽（REF）。
    for (const auto& region : function.exceptionRegions) {
        if (!recordValue(types, region.exceptionValue, reason)) return false;
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
                    return recordOperand(types, terminator.value, reason);
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

/// @brief 字段描述符 → JNI 访问种类（field_load_*/field_store_* helper 后缀）。
const char* fieldKindFor(const std::string& desc)
{
    if (desc.empty()) return nullptr;
    switch (desc[0]) {
    case 'Z': return "bool";
    case 'B': return "byte";
    case 'C': return "char";
    case 'S': return "short";
    case 'I': return "int";
    case 'J': return "long";
    case 'F': return "float";
    case 'D': return "double";
    case 'L':
    case '[': return "ref";
    default: return nullptr;
    }
}

/// @brief 字段 helper 的返回值 LLVM 类型（窄字段在 helper 内提升到 i32）。
const char* fieldHelperReturnType(const char* kind)
{
    if (kind == nullptr) return nullptr;
    const std::string k(kind);
    if (k == "long") return "i64";
    if (k == "float") return "float";
    if (k == "double") return "double";
    if (k == "ref") return "ptr";
    return "i32";   // bool/byte/char/short/int
}

/// @brief 数组元素 SSAType → helper 后缀（BALOAD 组件类型为 BYTE；无 BOOL 形态）。
const char* arrayKindFor(SSAType elementType)
{
    switch (elementType) {
    case SSAType::BOOL:
    case SSAType::I8: return "byte";   // BALOAD/BASTORE 组件类型固定 BYTE
    case SSAType::I16: return "short";
    case SSAType::CHAR: return "char";
    case SSAType::I32: return "int";
    case SSAType::I64: return "long";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::REF: return "ref";
    default: return nullptr;
    }
}

/// @brief 方法返回类型 → JNI 调用 helper 后缀（CallXxxMethodA 分派）。
const char* callKindFor(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "bool";
    case PrimitiveKind::BYTE: return "byte";
    case PrimitiveKind::CHAR: return "char";
    case PrimitiveKind::SHORT: return "short";
    case PrimitiveKind::INT: return "int";
    case PrimitiveKind::LONG: return "long";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::OBJECT: return "object";
    case PrimitiveKind::VOID: return "void";
    default: return nullptr;
    }
}

struct PendingEdge
{
    std::string           label;
    const RegSuccessor*   target = nullptr;
};

/// @brief 共享全局字符串表：发射端 intern 得到 @_mj2x_llvm_str_<N>，
/// finalize 时统一输出常量定义。
class GlobalStringTable
{
public:
    std::string intern(const std::string& text)
    {
        for (size_t index = 0; index < strings_.size(); ++index) {
            if (strings_[index] == text) return "@_mj2x_llvm_str_" + std::to_string(index);
        }
        const size_t index = strings_.size();
        strings_.push_back(text);
        return "@_mj2x_llvm_str_" + std::to_string(index);
    }

    std::string emitDefinitions() const
    {
        std::ostringstream out;
        for (size_t index = 0; index < strings_.size(); ++index) {
            out << "@_mj2x_llvm_str_" << index << " = private unnamed_addr constant ["
                << (strings_[index].size() + 1) << " x i8] c\"";
            for (const unsigned char ch : strings_[index]) {
                if (ch == '\\' || ch == '"') {
                    out << '\\' << static_cast<char>(ch);
                }
                else if (ch < 32 || ch > 126) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%02X", ch);
                    out << buf;
                }
                else {
                    out << static_cast<char>(ch);
                }
            }
            out << "\\00\"\n";
        }
        return out.str();
    }

private:
    std::vector<std::string> strings_;
};

class LlvmFunctionEmitter
{
public:
    LlvmFunctionEmitter(const RegFunction& function, const RegisterTypes& registerTypes,
                        const std::string& functionName, SSAType returnType,
                        InternalCallResolver* resolver, GlobalStringTable& strings)
        : function_(function)
        , registerTypes_(registerTypes)
        , functionName_(functionName)
        , returnType_(returnType)
        , resolver_(resolver)
        , strings_(strings)
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
        if (operand.kind == RegOperandKind::NullLiteral) {
            expression = "null";
            return true;
        }
        if (operand.kind == RegOperandKind::Opaque) {
            // String/Class 常量：JNI NewStringUTF / FindClass 构造（物化为临时）。
            if (operand.tag.rfind("ldc_string:", 0) == 0) {
                const std::string temporary = nextTemporary();
                out_ << "  " << temporary
                     << " = call ptr @mj2x_llvm_string_const(ptr %env, ptr "
                     << strings_.intern(operand.tag.substr(11)) << ")\n";
                expression = temporary;
                return true;
            }
            if (operand.tag.rfind("ldc_class:", 0) == 0) {
                const std::string temporary = nextTemporary();
                out_ << "  " << temporary << " = call ptr @mj2x_llvm_class_const(ptr %env, ptr "
                     << strings_.intern(operand.tag.substr(10)) << ")\n";
                expression = temporary;
                return true;
            }
        }
        reason = "LLVM IR first slice rejects reference and opaque operands";
        return false;
    }

    bool emitStore(RegValue destination, const std::string& expression, std::string& reason)    {
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

    bool emitInstruction(const RegBlock* block, const RegInstruction& instruction,
                         const std::vector<const RegExceptionRegion*>& regions,
                         std::string& reason)
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
                // 除零在 try 块内时：抛出的 ArithmeticException 按异常表分发
                // （无匹配/无异常区域则直接返回，pending 异常由 JVM 处理）。
                if (!regions.empty()) {
                    if (!emitExceptionDispatch(block, regions, dispatchIndex_, false, reason)) {
                        return false;
                    }
                }
                else {
                    out_ << "  ret " << type << " 0\n";
                }
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
            // 同模块内已转换的 invokestatic → 直接调用（0 JNI，保持确定性）；
            // 其余（实例调用、外部类、未转换目标）→ JNI CallXxxMethodA 分派。
            CallTarget target;
            const bool direct =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC && resolver_ != nullptr &&
                resolver_->resolve(instruction.method, target, reason);
            if (direct) {
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
            // JNI 分派：jvalue 数组（[N x { i64 }] union 槽）承载全部参数。
            const std::string              desc       = instruction.method.desc.to_std_string();
            const PrimitiveKind            retKind    = returnKind(desc);
            const char*                    callKind   = callKindFor(retKind);
            const char*                    mode       =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC
                    ? "call_static_"
                    : (instruction.method.invokeOpcode == Opcodes::INVOKESPECIAL
                           ? "call_nonvirtual_"
                           : "call_");
            const size_t                   receiverOffset =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC ? 0 : 1;
            if (callKind == nullptr) {
                reason = "LLVM IR call has an unsupported return type";
                return false;
            }
            const std::string jv = nextTemporary();
            out_ << "  " << jv << " = alloca [" << (instruction.args.size() - receiverOffset)
                 << " x { i64 }]\n";
            // jvalue 数组只承载方法描述符参数（实例调用的 receiver 单独传）。
            for (size_t index = receiverOffset; index < instruction.args.size(); ++index) {
                std::string expression;
                if (!emitOperand(instruction.args[index], expression, reason)) return false;
                const std::string slot = nextTemporary();
                out_ << "  " << slot << " = getelementptr ["
                     << (instruction.args.size() - receiverOffset) << " x { i64 }], ptr " << jv
                     << ", i64 0, i64 " << (index - receiverOffset) << "\n";
                const SSAType argType = widenNarrowType(instruction.args[index].type);
                out_ << "  store " << llvmType(argType) << " " << expression << ", ptr " << slot
                     << "\n";
            }
            std::string callArgs = "ptr %env";
            if (receiverOffset == 1) {
                std::string receiver;
                if (!emitOperand(instruction.args[0], receiver, reason)) return false;
                callArgs += ", ptr " + receiver;
            }
            callArgs += ", ptr " + strings_.intern(instruction.method.owner.to_std_string()) +
                        ", ptr " + strings_.intern(instruction.method.name.to_std_string()) +
                        ", ptr " + strings_.intern(desc) + ", ptr " + jv;
            if (retKind == PrimitiveKind::VOID) {
                out_ << "  call void @mj2x_llvm_" << mode << callKind << "(" << callArgs << ")\n";
                return true;
            }
            const std::string result = nextTemporary();
            const char*       boundaryRet = jniBoundaryType(retKind);
            out_ << "  " << result << " = call " << boundaryRet << " @mj2x_llvm_" << mode
                 << callKind << "(" << callArgs << ")\n";
            if (std::string(boundaryRet) == "i8" || std::string(boundaryRet) == "i16") {
                const bool  signed_ = retKind == PrimitiveKind::BYTE ||
                                     retKind == PrimitiveKind::SHORT;
                const std::string ext = nextTemporary();
                out_ << "  " << ext << " = " << (signed_ ? "sext" : "zext") << " "
                     << boundaryRet << " " << result << " to i32\n";
                return emitStore(instruction.dest, ext, reason);
            }
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
            if (instruction.convertKind == SSAConvertKind::ReferenceCast) {
                // checkcast：IsInstanceOf 失败挂起 ClassCastException，返回原引用。
                std::string value;
                if (!emitOperand(instruction.value, value, reason)) return false;
                const std::string desc = instruction.referenceTypeDesc.to_std_string();
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_check_cast(ptr %env, " << value
                     << ", ptr " << strings_.intern(desc) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
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
        if (instruction.kind == RegInstructionKind::LoadField) {
            const std::string fieldDesc = instruction.field.desc.to_std_string();
            const char*       kind      = fieldKindFor(fieldDesc);
            if (kind == nullptr) {
                reason = "LLVM IR field access has an unsupported descriptor";
                return false;
            }
            std::string receiver;
            if (instruction.receiver.kind == RegOperandKind::Opaque &&
                instruction.receiver.type == SSAType::UNKNOWN) {
                receiver = "null";
            }
            else if (!emitOperand(instruction.receiver, receiver, reason)) {
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call " << fieldHelperReturnType(kind)
                 << " @mj2x_llvm_field_load_" << kind << "(ptr %env, ptr " << receiver << ", ptr "
                 << strings_.intern(instruction.field.owner.to_std_string()) << ", ptr "
                 << strings_.intern(instruction.field.name.to_std_string()) << ", ptr "
                 << strings_.intern(fieldDesc) << ", i32 "
                 << (instruction.field.isStatic ? 1 : 0) << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::StoreField) {
            const std::string fieldDesc = instruction.field.desc.to_std_string();
            const char*       kind      = fieldKindFor(fieldDesc);
            if (kind == nullptr) {
                reason = "LLVM IR field access has an unsupported descriptor";
                return false;
            }
            std::string receiver;
            if (instruction.receiver.kind == RegOperandKind::Opaque &&
                instruction.receiver.type == SSAType::UNKNOWN) {
                receiver = "null";
            }
            else if (!emitOperand(instruction.receiver, receiver, reason)) {
                return false;
            }
            std::string value;
            if (!emitOperand(instruction.value, value, reason)) return false;
            out_ << "  call void @mj2x_llvm_field_store_" << kind << "(ptr %env, ptr " << receiver
                 << ", ptr " << strings_.intern(instruction.field.owner.to_std_string()) << ", ptr "
                 << strings_.intern(instruction.field.name.to_std_string()) << ", ptr "
                 << strings_.intern(fieldDesc) << ", i32 "
                 << (instruction.field.isStatic ? 1 : 0) << ", " << fieldHelperReturnType(kind)
                 << " " << value << ")\n";
            return true;
        }
        if (instruction.kind == RegInstructionKind::Allocate) {
            const std::string typeDesc = instruction.typeDesc.to_std_string();
            if (instruction.allocationKind == SSAAllocationKind::Object) {
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_new_object(ptr %env, ptr "
                     << strings_.intern(typeDesc) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (instruction.allocationKind == SSAAllocationKind::Array) {
                if (instruction.args.size() != 1) {
                    reason = "LLVM IR array allocation requires exactly one dimension";
                    return false;
                }
                std::string count;
                if (!emitOperand(instruction.args[0], count, reason)) return false;
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_new_array(ptr %env, ptr "
                     << strings_.intern(typeDesc) << ", i32 " << count << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            // MULTIANEWARRAY：调用点构建维度数组（alloca [N x i32]）。
            const std::string dims = nextTemporary();
            out_ << "  " << dims << " = alloca [" << instruction.args.size() << " x i32]\n";
            for (size_t index = 0; index < instruction.args.size(); ++index) {
                std::string dim;
                if (!emitOperand(instruction.args[index], dim, reason)) return false;
                const std::string slot = nextTemporary();
                out_ << "  " << slot << " = getelementptr ["
                     << instruction.args.size() << " x i32], ptr " << dims << ", i64 0, i64 "
                     << index << "\n";
                out_ << "  store i32 " << dim << ", ptr " << slot << "\n";
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call ptr @mj2x_llvm_new_multi_array(ptr %env, ptr "
                 << strings_.intern(typeDesc) << ", ptr " << dims << ", i32 "
                 << instruction.args.size() << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::ArrayLoad) {
            const char* kind = arrayKindFor(instruction.elementType);
            if (kind == nullptr) {
                reason = "LLVM IR array load has an unsupported element type";
                return false;
            }
            std::string array;
            std::string index;
            if (!emitOperand(instruction.receiver, array, reason) ||
                !emitOperand(instruction.index, index, reason)) {
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call " << fieldHelperReturnType(kind)
                 << " @mj2x_llvm_array_load_" << kind << "(ptr %env, ptr " << array << ", i32 "
                 << index << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::ArrayStore) {
            const char* kind = arrayKindFor(instruction.elementType);
            if (kind == nullptr) {
                reason = "LLVM IR array store has an unsupported element type";
                return false;
            }
            std::string array;
            std::string index;
            std::string value;
            if (!emitOperand(instruction.receiver, array, reason) ||
                !emitOperand(instruction.index, index, reason) ||
                !emitOperand(instruction.value, value, reason)) {
                return false;
            }
            out_ << "  call void @mj2x_llvm_array_store_" << kind << "(ptr %env, ptr " << array
                 << ", i32 " << index << ", " << fieldHelperReturnType(kind) << " " << value
                 << ")\n";
            return true;
        }
        if (instruction.kind == RegInstructionKind::Intrinsic) {
            const std::string& name = instruction.intrinsicName;
            if (name == "array.length") {
                std::string array;
                if (instruction.args.empty() || !emitOperand(instruction.args[0], array, reason)) {
                    reason = "LLVM IR array.length requires an array operand";
                    return false;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call i32 @mj2x_llvm_array_length(ptr %env, ptr "
                     << array << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (name.rfind("instanceof:", 0) == 0) {
                std::string value;
                if (instruction.args.empty() ||
                    !emitOperand(instruction.args[0], value, reason)) {
                    reason = "LLVM IR instanceof requires an object operand";
                    return false;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call i32 @mj2x_llvm_instance_of(ptr %env, " << value
                     << ", ptr " << strings_.intern(name.substr(11)) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (name == "monitor.enter" || name == "monitor.exit") {
                std::string value;
                if (instruction.args.empty() || !emitOperand(instruction.args[0], value, reason)) {
                    reason = "LLVM IR monitor requires an object operand";
                    return false;
                }
                out_ << "  call void @mj2x_llvm_monitor_"
                     << (name == "monitor.enter" ? "enter" : "exit") << "(ptr %env, ptr " << value
                     << ")\n";
                return true;
            }
            reason = "LLVM IR first slice rejects this intrinsic";
            return false;
        }
        reason = "LLVM IR encountered an unsupported instruction after validation";
        return false;
    }

    /// @brief block 的活跃异常区域（异常表顺序；无 protected 匹配则空）。
    std::vector<const RegExceptionRegion*> activeRegionsFor(const RegBlock* block) const
    {
        std::vector<const RegExceptionRegion*> result;
        for (const auto& region : function_.exceptionRegions) {
            for (const auto* protectedBlock : region.protectedBlocks) {
                if (protectedBlock == block) {
                    result.push_back(&region);
                    break;
                }
            }
        }
        return result;
    }

    /// @brief 默认返回（异常泄漏/无 handler 时保持 JVM 传播语义的兜底路径）。
    void emitDefaultReturn()
    {
        if (returnType_ == SSAType::VOID) {
            out_ << "  ret void\n";
            return;
        }
        const char* type = llvmType(returnType_);
        const char* zero = (type == nullptr) ? "0"
                           : (std::string(type) == "float" || std::string(type) == "double")
                               ? "0.0"
                               : (std::string(type) == "ptr" ? "null" : "0");
        out_ << "  ret " << (type == nullptr ? "i32" : type) << " " << zero << "\n";
    }

    /// @brief 异常分发：ExceptionCheck 后按异常表匹配 handler；无 handler/无匹配
    /// 重抛并默认返回。匹配成功时执行异常边拷贝 + exceptionValue 赋值后跳 handler。
    /// @param continueAfter 为 true 时 false 分支落到 cont_N 标签（调用方继续
    /// 发射指令/终止符）；为 false（athrow 终止符）时 false 分支走默认返回。
    bool emitExceptionDispatch(const RegBlock* block,
                               const std::vector<const RegExceptionRegion*>& regions,
                               uint32_t& dispatchIndex, bool continueAfter, std::string& reason)
    {
        const uint32_t   index = dispatchIndex++;
        const std::string chk  = nextTemporary();
        out_ << "  " << chk << " = call i1 @mj2x_llvm_exception_check(ptr %env)\n";
        const std::string exLabel   = "ex_" + std::to_string(index);
        const std::string contLabel = "cont_" + std::to_string(index);
        const std::string noExLabel = "nex_" + std::to_string(index);
        out_ << "  br i1 " << chk << ", label %" << exLabel << ", label %"
             << (continueAfter ? contLabel : noExLabel) << "\n";
        out_ << exLabel << ":\n";
        const std::string ex = nextTemporary();
        out_ << "  " << ex << " = call ptr @mj2x_llvm_exception_occurred(ptr %env)\n";
        out_ << "  call void @mj2x_llvm_exception_clear(ptr %env)\n";
        if (regions.empty()) {
            out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << ex << ")\n";
            emitDefaultReturn();
            if (continueAfter) {
                out_ << contLabel << ":\n";
            }
            else {
                out_ << noExLabel << ":\n";
                emitDefaultReturn();
            }
            return true;
        }
        for (size_t r = 0; r < regions.size(); ++r) {
            const auto& region     = *regions[r];
            const std::string match = nextTemporary();
            const bool   last       = r + 1 == regions.size();
            out_ << "  " << match << " = call i1 @mj2x_llvm_exception_matches(ptr %env, ptr "
                 << ex << ", ptr " << strings_.intern(region.caughtType) << ")\n";
            out_ << "  br i1 " << match << ", label %hx_" << index << "_" << r << ", label %"
                 << (last ? ("rx_" + std::to_string(index)) : ("exn_" + std::to_string(index) + "_" + std::to_string(r)))
                 << "\n";
            out_ << "hx_" << index << "_" << r << ":\n";
            const RegExceptionRegion::Edge* edge = nullptr;
            for (const auto& e : region.edges) {
                if (e.protectedBlock == block) {
                    edge = &e;
                    break;
                }
            }
            if (edge != nullptr) {
                for (const auto& copy : edge->copies) {
                    std::string expression;
                    if (!emitOperand(copy.source, expression, reason)) return false;
                    const auto it = registerTypes_.find(copy.dest.id.value);
                    const char* type = it == registerTypes_.end() ? nullptr : llvmType(it->second);
                    if (type == nullptr) {
                        reason = "LLVM IR exception edge copy has an unsupported type";
                        return false;
                    }
                    out_ << "  store " << type << " " << expression << ", ptr %slot_r"
                         << copy.dest.id.value << "\n";
                }
            }
            out_ << "  store ptr " << ex << ", ptr %slot_r" << region.exceptionValue.id.value
                 << "\n";
            out_ << "  br label %b" << region.handler->id.value << "\n";
            if (!last) {
                out_ << "exn_" << index << "_" << r << ":\n";
            }
        }
        out_ << "rx_" << index << ":\n";
        out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << ex << ")\n";
        emitDefaultReturn();
        if (continueAfter) {
            out_ << contLabel << ":\n";
        }
        else {
            out_ << noExLabel << ":\n";
            emitDefaultReturn();
        }
        return true;
    }

    bool emitBlock(const RegBlock& block, std::string& reason)
    {
        const auto regions = activeRegionsFor(&block);
        out_ << "b" << block.id.value << ":\n";
        for (const auto& instruction : block.instructions) {
            if (!emitInstruction(&block, instruction, regions, reason)) return false;
            // 方法含 try/catch 时，可能抛异常的指令后插入异常分发
            // （无异常区域时 pending 异常泄漏到 JNI 边界由 JVM 处理）。
            if (instruction.effect.mayThrow && !regions.empty()) {
                if (!emitExceptionDispatch(&block, regions, dispatchIndex_, true, reason)) {
                    return false;
                }
            }
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
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    // athrow：JNI Throw（null → NPE）；有异常区域时按表分发，
                    // 否则 pending 异常泄漏到 JVM 边界。
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << value << ")\n";
                    if (!regions.empty()) {
                        if (!emitExceptionDispatch(&block, regions, dispatchIndex_, false, reason)) {
                            return false;
                        }
                    }
                    else {
                        emitDefaultReturn();
                    }
                    out_ << "\n";
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
    GlobalStringTable&       strings_;
    std::ostringstream       out_;
    uint32_t                 temporaryIndex_ = 0;
    uint32_t                 edgeIndex_ = 0;
    uint32_t                 divIndex_ = 0;
    uint32_t                 dispatchIndex_ = 0;   // 函数级：异常分发 label 唯一
    std::vector<PendingEdge> pendingEdges_;
};

bool supportsMethodDescriptor(const MethodNode& method, SSAType& resultType, std::string& reason)
{
    // 实例方法自 Phase 4 起支持：this 为 SSA 参数 0（REF），wrapper 第二参为
    // jobject this；实例内调用经 JNI 分派（CallXxxMethodA）。
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
                           const std::string& helper, SSAType /*returnType*/, bool isStatic)
{
    const std::string             descriptor = method.desc.to_std_string();
    const std::vector<PrimitiveKind> arguments = argumentKinds(descriptor);
    const PrimitiveKind          retKind   = returnKind(descriptor);
    const char*                  boundaryRet = jniBoundaryType(retKind);
    const char*                  calcRet    = llvmType(primitiveToSsaType(retKind));
    std::ostringstream           out;
    // JNI native 签名：静态方法第二参为 jclass；实例方法为 jobject this。
    out << "define dllexport " << boundaryRet << " @"
        << mangleMethod(className, method.name.to_std_string(), descriptor) << "(ptr %env, ptr "
        << (isStatic ? "%clazz" : "%this");
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
    if (!isStatic) {
        // 实例方法：this 是 SSA 参数 0（REF），直接透传。
        out << ", ptr %this";
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
    GlobalStringTable  strings;
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
                             (method.methodNode->access & Opcodes::ACC_STATIC) != 0,
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
    LlvmFunctionEmitter emitter(*reg, registerTypes, helper, resultType, callResolver.get(),
                                impl_->strings);
    if (!emitter.emit(helperSource, reason)) {
        result.text = "LLVM IR lowering failed: " + reason;
        return result;
    }

    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = helperSource +
                  emitJniWrapper(className, *method.methodNode, helper, resultType,
                                 (method.methodNode->access & jbytecode::core::Opcodes::ACC_STATIC) !=
                                     0);
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

/// @brief JNIEnv 函数表槽位（本机 jni.h 精确提取 + reserved0-3 偏移）。
namespace jni_slot {
constexpr int64_t FindClass           = 6;
constexpr int64_t ThrowNew            = 14;
constexpr int64_t AllocObject         = 27;
constexpr int64_t GetObjectClass      = 31;
constexpr int64_t IsInstanceOf        = 32;
constexpr int64_t GetMethodID         = 33;
constexpr int64_t GetStaticMethodID   = 113;
constexpr int64_t GetFieldID          = 94;
constexpr int64_t GetStaticFieldID    = 144;
constexpr int64_t GetBooleanField     = 96;
constexpr int64_t GetByteField        = 97;
constexpr int64_t GetCharField        = 98;
constexpr int64_t GetShortField       = 99;
constexpr int64_t GetIntField         = 100;
constexpr int64_t GetLongField        = 101;
constexpr int64_t GetFloatField       = 102;
constexpr int64_t GetDoubleField      = 103;
constexpr int64_t GetObjectField      = 95;
constexpr int64_t SetBooleanField     = 105;
constexpr int64_t SetByteField        = 106;
constexpr int64_t SetCharField        = 107;
constexpr int64_t SetShortField       = 108;
constexpr int64_t SetIntField         = 109;
constexpr int64_t SetLongField        = 110;
constexpr int64_t SetFloatField       = 111;
constexpr int64_t SetDoubleField      = 112;
constexpr int64_t SetObjectField      = 104;
constexpr int64_t GetStaticBooleanField  = 146;
constexpr int64_t GetStaticByteField     = 147;
constexpr int64_t GetStaticCharField     = 148;
constexpr int64_t GetStaticShortField    = 149;
constexpr int64_t GetStaticIntField      = 150;
constexpr int64_t GetStaticLongField     = 151;
constexpr int64_t GetStaticFloatField    = 152;
constexpr int64_t GetStaticDoubleField   = 153;
constexpr int64_t GetStaticObjectField   = 145;
constexpr int64_t SetStaticBooleanField  = 155;
constexpr int64_t SetStaticByteField     = 156;
constexpr int64_t SetStaticCharField     = 157;
constexpr int64_t SetStaticShortField    = 158;
constexpr int64_t SetStaticIntField      = 159;
constexpr int64_t SetStaticLongField     = 160;
constexpr int64_t SetStaticFloatField    = 161;
constexpr int64_t SetStaticDoubleField   = 162;
constexpr int64_t SetStaticObjectField   = 154;
constexpr int64_t GetArrayLength         = 171;
constexpr int64_t NewObjectArray         = 172;
constexpr int64_t GetObjectArrayElement  = 173;
constexpr int64_t SetObjectArrayElement  = 174;
constexpr int64_t NewBooleanArray        = 175;
constexpr int64_t NewByteArray           = 176;
constexpr int64_t NewCharArray           = 177;
constexpr int64_t NewShortArray          = 178;
constexpr int64_t NewIntArray            = 179;
constexpr int64_t NewLongArray           = 180;
constexpr int64_t NewFloatArray          = 181;
constexpr int64_t NewDoubleArray         = 182;
constexpr int64_t GetBooleanArrayRegion  = 199;
constexpr int64_t GetByteArrayRegion     = 200;
constexpr int64_t GetCharArrayRegion     = 201;
constexpr int64_t GetShortArrayRegion    = 202;
constexpr int64_t GetIntArrayRegion      = 203;
constexpr int64_t GetLongArrayRegion     = 204;
constexpr int64_t GetFloatArrayRegion    = 205;
constexpr int64_t GetDoubleArrayRegion   = 206;
constexpr int64_t SetBooleanArrayRegion  = 207;
constexpr int64_t SetByteArrayRegion     = 208;
constexpr int64_t SetCharArrayRegion     = 209;
constexpr int64_t SetShortArrayRegion    = 210;
constexpr int64_t SetIntArrayRegion      = 211;
constexpr int64_t SetLongArrayRegion     = 212;
constexpr int64_t SetFloatArrayRegion    = 213;
constexpr int64_t SetDoubleArrayRegion   = 214;
constexpr int64_t MonitorEnter           = 217;
constexpr int64_t MonitorExit            = 218;
constexpr int64_t Throw                 = 13;
constexpr int64_t ExceptionOccurred     = 15;
constexpr int64_t ExceptionClear        = 17;
constexpr int64_t NewStringUTF          = 167;
constexpr int64_t ExceptionCheck        = 228;
}   // namespace jni_slot

/// @brief 对象/字段/类型检查共享 helper 集合（finalize 统一生成）。
///
/// @details 字段 kind 表：JNI Get/Set 函数槽、返回/参数类型、窄类型扩展方向。
/// helper 首参统一 JNIEnv*；字段访问带 null 检查（实例 receiver 为 null 抛 NPE）。
std::string emitObjectHelpers()
{
    struct FieldKindInfo
    {
        const char* kind;       // helper 后缀
        int64_t     getSlot;    // Get*Field
        int64_t     getStaticSlot;
        int64_t     setSlot;    // Set*Field
        int64_t     setStaticSlot;
        const char* jniType;    // JNI 函数声明类型
        const char* retType;    // helper 返回类型（窄提升后）
        const char* ext;        // 窄扩展（sext/zext/空）
    };
    static const FieldKindInfo kinds[] = {
        {"bool", jni_slot::GetBooleanField, jni_slot::GetStaticBooleanField,
         jni_slot::SetBooleanField, jni_slot::SetStaticBooleanField, "i8", "i32", "zext"},
        {"byte", jni_slot::GetByteField, jni_slot::GetStaticByteField, jni_slot::SetByteField,
         jni_slot::SetStaticByteField, "i8", "i32", "sext"},
        {"char", jni_slot::GetCharField, jni_slot::GetStaticCharField, jni_slot::SetCharField,
         jni_slot::SetStaticCharField, "i16", "i32", "zext"},
        {"short", jni_slot::GetShortField, jni_slot::GetStaticShortField,
         jni_slot::SetShortField, jni_slot::SetStaticShortField, "i16", "i32", "sext"},
        {"int", jni_slot::GetIntField, jni_slot::GetStaticIntField, jni_slot::SetIntField,
         jni_slot::SetStaticIntField, "i32", "i32", ""},
        {"long", jni_slot::GetLongField, jni_slot::GetStaticLongField, jni_slot::SetLongField,
         jni_slot::SetStaticLongField, "i64", "i64", ""},
        {"float", jni_slot::GetFloatField, jni_slot::GetStaticFloatField,
         jni_slot::SetFloatField, jni_slot::SetStaticFloatField, "float", "float", ""},
        {"double", jni_slot::GetDoubleField, jni_slot::GetStaticDoubleField,
         jni_slot::SetDoubleField, jni_slot::SetStaticDoubleField, "double", "double", ""},
        {"ref", jni_slot::GetObjectField, jni_slot::GetStaticObjectField,
         jni_slot::SetObjectField, jni_slot::SetStaticObjectField, "ptr", "ptr", ""},
    };

    std::ostringstream out;
    const char* npeName = "java/lang/NullPointerException";
    const char* cceName = "java/lang/ClassCastException";
    const char* nullMsg = "null";
    const auto zeroFor = [](const char* retType) -> const char* {
        const std::string t(retType);
        if (t == "float" || t == "double") return "0.0";
        if (t == "ptr") return "null";
        return "0";
    };
    // NPE / CCE 类名与消息（固定名，避开调用点字符串表）。
    out << "@_mj2x_llvm_obj_npe = private unnamed_addr constant ["
        << (std::strlen(npeName) + 1) << " x i8] c\"" << npeName << "\\00\"\n";
    out << "@_mj2x_llvm_obj_cce = private unnamed_addr constant ["
        << (std::strlen(cceName) + 1) << " x i8] c\"" << cceName << "\\00\"\n";
    out << "@_mj2x_llvm_obj_null = private unnamed_addr constant ["
        << (std::strlen(nullMsg) + 1) << " x i8] c\"" << nullMsg << "\\00\"\n\n";

    // throw_null：NPE。
    out << "define internal void @mj2x_llvm_throw_null(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";

    // new_object：FindClass + AllocObject（不调用 <init>；构造器由 invokespecial 处理）。
    out << "define internal ptr @mj2x_llvm_new_object(ptr %env, ptr %name) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %name)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ao0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::AllocObject << "\n"
        << "  %allocobject = load ptr, ptr %ao0\n"
           "  %obj = call ptr %allocobject(ptr %env, ptr %cls)\n"
           "  ret ptr %obj\n"
           "}\n\n";

    // new_array：按元素描述符首字符分发 JNI NewXxxArray / NewObjectArray。
    out << "define internal ptr @mj2x_llvm_new_array(ptr %env, ptr %desc, i32 %count) {\n"
           "entry:\n"
           "  %c1 = getelementptr i8, ptr %desc, i64 1\n"
           "  %t = load i8, ptr %c1\n"
           "  switch i8 %t, label %obj [\n"
           "    i8 90, label %bool_\n"
           "    i8 66, label %byte_\n"
           "    i8 67, label %char_\n"
           "    i8 83, label %short_\n"
           "    i8 73, label %int_\n"
           "    i8 74, label %long_\n"
           "    i8 70, label %float_\n"
           "    i8 68, label %double_\n"
           "  ]\n";
    const struct { const char* label; int64_t slot; } primArrays[] = {
        {"bool_", jni_slot::NewBooleanArray}, {"byte_", jni_slot::NewByteArray},
        {"char_", jni_slot::NewCharArray},   {"short_", jni_slot::NewShortArray},
        {"int_", jni_slot::NewIntArray},      {"long_", jni_slot::NewLongArray},
        {"float_", jni_slot::NewFloatArray},  {"double_", jni_slot::NewDoubleArray},
    };
    for (const auto& entry : primArrays) {
        out << entry.label << ":\n"
            << "  %ft" << entry.label << " = load ptr, ptr %env\n"
            << "  %fn" << entry.label << " = getelementptr ptr, ptr %ft" << entry.label
            << ", i64 " << entry.slot << "\n"
            << "  %f" << entry.label << " = load ptr, ptr %fn" << entry.label << "\n"
            << "  %a" << entry.label << " = call ptr %f" << entry.label
            << "(ptr %env, i32 %count)\n"
            << "  ret ptr %a" << entry.label << "\n";
    }
    out << "obj:\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %fc2 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc2\n"
           "  %cls = call ptr %find(ptr %env, ptr %c1)\n"
           "  %ft3 = load ptr, ptr %env\n"
           "  %na0 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::NewObjectArray << "\n"
        << "  %naf = load ptr, ptr %na0\n"
           "  %arr = call ptr %naf(ptr %env, i32 %count, ptr %cls, ptr null)\n"
           "  ret ptr %arr\n"
           "}\n\n";

    // new_multi_array：NewObjectArray 递归（JVM EagerAlloc 语义）。
    out << "define internal ptr @mj2x_llvm_new_multi_array(ptr %env, ptr %desc, ptr %dims, "
           "i32 %ndims) {\n"
           "entry:\n"
           "  %d0p = getelementptr i32, ptr %dims, i64 0\n"
           "  %d0 = load i32, ptr %d0p\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %edesc = getelementptr i8, ptr %desc, i64 1\n"
           "  %cls = call ptr %find(ptr %env, ptr %edesc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %na0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::NewObjectArray << "\n"
        << "  %naf = load ptr, ptr %na0\n"
           "  %arr = call ptr %naf(ptr %env, i32 %d0, ptr %cls, ptr null)\n"
           "  %is1 = icmp eq i32 %ndims, 1\n"
           "  br i1 %is1, label %done, label %fill\n"
           "fill:\n"
           "  %dims1 = getelementptr i32, ptr %dims, i64 1\n"
           "  %nd1 = sub i32 %ndims, 1\n"
           "  br label %loop\n"
           "loop:\n"
           "  %i = phi i32 [ 0, %fill ], [ %inext, %loopnext ]\n"
           "  %c = icmp slt i32 %i, %d0\n"
           "  br i1 %c, label %body, label %done\n"
           "body:\n"
           "  %elem = call ptr @mj2x_llvm_new_multi_array(ptr %env, ptr %edesc, ptr %dims1, "
           "i32 %nd1)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %se0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::SetObjectArrayElement << "\n"
        << "  %sef = load ptr, ptr %se0\n"
           "  call void %sef(ptr %env, ptr %arr, i32 %i, ptr %elem)\n"
           "  %inext = add i32 %i, 1\n"
           "  br label %loopnext\n"
           "loopnext:\n"
           "  br label %loop\n"
           "done:\n"
           "  ret ptr %arr\n"
           "}\n\n";

    // 字段 load/store helpers（static/instance 合一 + null 检查）。
    for (const auto& k : kinds) {
        const std::string getFn = "get" + std::string(k.kind) + "field";
        const std::string getStaticFn = "getstatic" + std::string(k.kind) + "field";
        out << "define internal " << k.retType << " @mj2x_llvm_field_load_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %findclass = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %findclass(ptr %env, ptr %owner)\n"
            << "  %isst = icmp eq i32 %is_static, 0\n"
            << "  br i1 %isst, label %inst, label %stat\n"
            << "inst:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %inst_ok\n"
            << "inst_ok:\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gf1 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetFieldID << "\n"
            << "  %getfid = load ptr, ptr %gf1\n"
            << "  %fid = call ptr %getfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %gi1 = getelementptr ptr, ptr %ft2, i64 " << k.getSlot << "\n"
            << "  %gv = load ptr, ptr %gi1\n"
            << "  %v = call " << k.jniType << " %gv(ptr %env, ptr %rcv, ptr %fid)\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w = " << k.ext << " " << k.jniType << " %v to " << k.retType << "\n"
                << "  ret " << k.retType << " %w\n";
        }
        else {
            out << "  ret " << k.retType << " %v\n";
        }
        out << "stat:\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gs1 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::GetStaticFieldID << "\n"
            << "  %getstatfid = load ptr, ptr %gs1\n"
            << "  %fid2 = call ptr %getstatfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft4 = load ptr, ptr %env\n"
            << "  %gsi1 = getelementptr ptr, ptr %ft4, i64 " << k.getStaticSlot << "\n"
            << "  %gsv = load ptr, ptr %gsi1\n"
            << "  %v2 = call " << k.jniType << " %gsv(ptr %env, ptr %cls, ptr %fid2)\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w2 = " << k.ext << " " << k.jniType << " %v2 to " << k.retType << "\n"
                << "  ret " << k.retType << " %w2\n";
        }
        else {
            out << "  ret " << k.retType << " %v2\n";
        }
        out << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret " << k.retType << " " << zeroFor(k.retType) << "\n"
            << "}\n\n";
        // store。
        out << "define internal void @mj2x_llvm_field_store_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, i32 %is_static, "
            << k.retType << " %val) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %findclass = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %findclass(ptr %env, ptr %owner)\n"
            << "  %isst = icmp eq i32 %is_static, 0\n"
            << "  br i1 %isst, label %inst, label %stat\n"
            << "inst:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %inst_ok\n"
            << "inst_ok:\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gf1 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetFieldID << "\n"
            << "  %getfid = load ptr, ptr %gf1\n"
            << "  %fid = call ptr %getfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %si1 = getelementptr ptr, ptr %ft2, i64 " << k.setSlot << "\n"
            << "  %sv = load ptr, ptr %si1\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  call void %sv(ptr %env, ptr %rcv, ptr %fid, " << k.jniType << " %tv)\n";
        }
        else {
            out << "  call void %sv(ptr %env, ptr %rcv, ptr %fid, " << k.jniType << " %val)\n";
        }
        out << "  ret void\n"
            << "stat:\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gs1 = getelementptr ptr, ptr %ft3, i64 " << jni_slot::GetStaticFieldID << "\n"
            << "  %getstatfid = load ptr, ptr %gs1\n"
            << "  %fid2 = call ptr %getstatfid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft4 = load ptr, ptr %env\n"
            << "  %ssi1 = getelementptr ptr, ptr %ft4, i64 " << k.setStaticSlot << "\n"
            << "  %ssv = load ptr, ptr %ssi1\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv2 = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  call void %ssv(ptr %env, ptr %cls, ptr %fid2, " << k.jniType << " %tv2)\n";
        }
        else {
            out << "  call void %ssv(ptr %env, ptr %cls, ptr %fid2, " << k.jniType << " %val)\n";
        }
        out << "  ret void\n"
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret void\n"
            << "}\n\n";
    }

    // 数组元素读写 helper：null → NPE；越界由 JNI Get/Set*ArrayRegion /
    // Get/SetObjectArrayElement 抛 ArrayIndexOutOfBoundsException / 存储违例抛
    // ArrayStoreException。byte 形态特判 boolean[]（JNI 要求 GetBooleanArrayRegion）。
    out << "@_mj2x_llvm_obj_zarr = private unnamed_addr constant [3 x i8] c\"[Z\\00\"\n\n";
    struct ArrayKindInfo
    {
        const char* kind;      // helper 后缀
        SSAType     ssaType;   // 元素 SSA 类型
        int64_t     getSlot;
        int64_t     setSlot;
        const char* jniType;   // region 元素类型
        const char* retType;   // helper 返回类型（窄提升后）
        const char* ext;       // 窄扩展（sext/zext/空）
    };
    static const ArrayKindInfo arrayKinds[] = {
        {"byte", SSAType::I8, jni_slot::GetByteArrayRegion, jni_slot::SetByteArrayRegion, "i8",
         "i32", "sext"},
        {"short", SSAType::I16, jni_slot::GetShortArrayRegion, jni_slot::SetShortArrayRegion,
         "i16", "i32", "sext"},
        {"char", SSAType::CHAR, jni_slot::GetCharArrayRegion, jni_slot::SetCharArrayRegion,
         "i16", "i32", "zext"},
        {"int", SSAType::I32, jni_slot::GetIntArrayRegion, jni_slot::SetIntArrayRegion, "i32",
         "i32", ""},
        {"long", SSAType::I64, jni_slot::GetLongArrayRegion, jni_slot::SetLongArrayRegion,
         "i64", "i64", ""},
        {"float", SSAType::F32, jni_slot::GetFloatArrayRegion, jni_slot::SetFloatArrayRegion,
         "float", "float", ""},
        {"double", SSAType::F64, jni_slot::GetDoubleArrayRegion, jni_slot::SetDoubleArrayRegion,
         "double", "double", ""},
        {"ref", SSAType::REF, jni_slot::GetObjectArrayElement, jni_slot::SetObjectArrayElement,
         "ptr", "ptr", ""},
    };
    for (const auto& k : arrayKinds) {
        const bool isByte = std::string(k.kind) == "byte";
        const bool isRef  = std::string(k.kind) == "ref";
        out << "define internal " << k.retType << " @mj2x_llvm_array_load_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %arr, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n";
        if (isByte) {
            // boolean[] 特判：GetBooleanArrayRegion（jboolean i8 0/1 → zext）。
            out << "  %ft0 = load ptr, ptr %env\n"
                << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
                << "  %find0 = load ptr, ptr %fc0\n"
                << "  %zcls = call ptr %find0(ptr %env, ptr @_mj2x_llvm_obj_zarr)\n"
                << "  %ft1 = load ptr, ptr %env\n"
                << "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
                << "  %isinst0 = load ptr, ptr %ii0\n"
                << "  %isbool = call i32 %isinst0(ptr %env, ptr %arr, ptr %zcls)\n"
                << "  %b = icmp ne i32 %isbool, 0\n"
                << "  br i1 %b, label %boolarr, label %bytearr\n"
                << "boolarr:\n"
                << "  %tmpb = alloca i8\n"
                << "  %ft2 = load ptr, ptr %env\n"
                << "  %gb0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::GetBooleanArrayRegion
                << "\n"
                << "  %gb = load ptr, ptr %gb0\n"
                << "  call void %gb(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmpb)\n"
                << "  %vb = load i8, ptr %tmpb\n"
                << "  %wb = zext i8 %vb to i32\n"
                << "  ret i32 %wb\n"
                << "bytearr:\n";
        }
        if (isRef) {
            // 引用数组：GetObjectArrayElement 直接返回元素。
            out << "  %ft3 = load ptr, ptr %env\n"
                << "  %gr0 = getelementptr ptr, ptr %ft3, i64 " << k.getSlot << "\n"
                << "  %gr = load ptr, ptr %gr0\n"
                << "  %v = call ptr %gr(ptr %env, ptr %arr, i32 %idx)\n"
                << "  ret ptr %v\n"
                << "npe:\n"
                << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
                << "  ret ptr null\n"
                << "}\n\n";
            // store 分支在下方循环体内（isRef 特判）。
        }
        else {
            out << "  %tmp = alloca " << k.jniType << "\n"
            << "  %ft3 = load ptr, ptr %env\n"
            << "  %gr0 = getelementptr ptr, ptr %ft3, i64 " << k.getSlot << "\n"
            << "  %gr = load ptr, ptr %gr0\n"
            << "  call void %gr(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmp)\n"
            << "  %v = load " << k.jniType << ", ptr %tmp\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %w = " << k.ext << " " << k.jniType << " %v to " << k.retType << "\n"
                << "  ret " << k.retType << " %w\n";
        }
        else {
            out << "  ret " << k.retType << " %v\n";
        }
        out << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret " << k.retType << " " << zeroFor(k.retType) << "\n"
            << "}\n\n";
        }
        // store。
        out << "define internal void @mj2x_llvm_array_store_" << k.kind
            << "(ptr %env, ptr %arr, i32 %idx, " << k.retType << " %val) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %arr, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n";
        if (isByte) {
            out << "  %ft0 = load ptr, ptr %env\n"
                << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
                << "  %find0 = load ptr, ptr %fc0\n"
                << "  %zcls = call ptr %find0(ptr %env, ptr @_mj2x_llvm_obj_zarr)\n"
                << "  %ft1 = load ptr, ptr %env\n"
                << "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
                << "  %isinst0 = load ptr, ptr %ii0\n"
                << "  %isbool = call i32 %isinst0(ptr %env, ptr %arr, ptr %zcls)\n"
                << "  %b = icmp ne i32 %isbool, 0\n"
                << "  br i1 %b, label %boolarr, label %bytearr\n"
                << "boolarr:\n"
                << "  %tvb = trunc i32 %val to i8\n"
                << "  %nzb = icmp ne i8 %tvb, 0\n"
                << "  %oneb = zext i1 %nzb to i8\n"
                << "  %tmpb = alloca i8\n"
                << "  store i8 %oneb, ptr %tmpb\n"
                << "  %ft2 = load ptr, ptr %env\n"
                << "  %sb0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::SetBooleanArrayRegion
                << "\n"
                << "  %sb = load ptr, ptr %sb0\n"
                << "  call void %sb(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmpb)\n"
                << "  ret void\n"
                << "bytearr:\n";
        }
        if (isRef) {
            // 引用数组：Get/SetObjectArrayElement 直接传元素（无 region 缓冲）。
            out << "  %ft3 = load ptr, ptr %env\n"
                << "  %sr0 = getelementptr ptr, ptr %ft3, i64 " << k.setSlot << "\n"
                << "  %sr = load ptr, ptr %sr0\n"
                << "  call void %sr(ptr %env, ptr %arr, i32 %idx, ptr %val)\n"
                << "  ret void\n"
                << "npe:\n"
                << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
                << "  ret void\n"
                << "}\n\n";
            continue;
        }
        out << "  %tmp = alloca " << k.jniType << "\n";
        if (std::strlen(k.ext) > 0) {
            out << "  %tv = trunc " << k.retType << " %val to " << k.jniType << "\n"
                << "  store " << k.jniType << " %tv, ptr %tmp\n";
        }
        else {
            out << "  store " << k.jniType << " %val, ptr %tmp\n";
        }
        out << "  %ft3 = load ptr, ptr %env\n"
            << "  %sr0 = getelementptr ptr, ptr %ft3, i64 " << k.setSlot << "\n"
            << "  %sr = load ptr, ptr %sr0\n"
            << "  call void %sr(ptr %env, ptr %arr, i32 %idx, i32 1, ptr %tmp)\n"
            << "  ret void\n"
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << "  ret void\n"
            << "}\n\n";
    }
    // array_length：null → NPE；GetArrayLength。
    out << "define internal i32 @mj2x_llvm_array_length(ptr %env, ptr %arr) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %arr, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %al0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetArrayLength << "\n"
        << "  %getlen = load ptr, ptr %al0\n"
           "  %len = call i32 %getlen(ptr %env, ptr %arr)\n"
           "  ret i32 %len\n"
           "npe:\n"
           "  call void @mj2x_llvm_throw_null(ptr %env)\n"
           "  ret i32 0\n"
           "}\n\n";

    // JNI 方法调用 helper：CallXxxMethodA（jvalue 数组）。
    // virtual/interface → Call<Type>MethodA；special → CallNonvirtual<Type>MethodA
    // （GetObjectClass + 显式类，用于 <init>/private/super）；static 外部 →
    // CallStatic<Type>MethodA。null receiver（实例）抛 NPE。
    struct CallKindInfo
    {
        const char* kind;
        int64_t     callSlot;       // Call*MethodA
        int64_t     nonvirtualSlot; // CallNonvirtual*MethodA
        int64_t     staticSlot;     // CallStatic*MethodA
        const char* boundary;       // JNI 返回类型
    };
    static const CallKindInfo callKinds[] = {
        {"object", 36, 66, 116, "ptr"},
        {"bool", 39, 69, 119, "i8"},
        {"byte", 42, 72, 122, "i8"},
        {"char", 45, 75, 125, "i16"},
        {"short", 48, 78, 128, "i16"},
        {"int", 51, 81, 131, "i32"},
        {"long", 54, 84, 134, "i64"},
        {"float", 57, 87, 137, "float"},
        {"double", 60, 90, 140, "double"},
        {"void", 63, 93, 143, "void"},
    };
    for (const auto& k : callKinds) {
        // virtual / interface。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %find = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %find(ptr %env, ptr %owner)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.callSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary << " (ptr, ptr, ptr, ptr) %call(ptr %env, ptr %rcv, "
               "ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << (std::string(k.boundary) == "void"
                    ? "  ret void\n"
                    : "  ret " + std::string(k.boundary) + " " + zeroFor(k.boundary) + "\n")
            << "}\n\n";
        // special（invokespecial：<init>/private/super）。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_nonvirtual_" << k.kind
            << "(ptr %env, ptr %rcv, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %isnull = icmp eq ptr %rcv, null\n"
            << "  br i1 %isnull, label %npe, label %ok\n"
            << "ok:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %gc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::GetObjectClass << "\n"
            << "  %getcls = load ptr, ptr %gc0\n"
            << "  %rcls = call ptr %getcls(ptr %env, ptr %rcv)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %rcls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.nonvirtualSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary
            << " (ptr, ptr, ptr, ptr, ptr) %call(ptr %env, ptr %rcv, ptr %rcls, ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "npe:\n"
            << "  call void @mj2x_llvm_throw_null(ptr %env)\n"
            << (std::string(k.boundary) == "void"
                    ? "  ret void\n"
                    : "  ret " + std::string(k.boundary) + " " + zeroFor(k.boundary) + "\n")
            << "}\n\n";
        // static（外部类）。
        out << "define internal " << k.boundary << " @mj2x_llvm_call_static_" << k.kind
            << "(ptr %env, ptr %owner, ptr %name, ptr %desc, ptr %jv) {\n"
            << "entry:\n"
            << "  %ft0 = load ptr, ptr %env\n"
            << "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
            << "  %find = load ptr, ptr %fc0\n"
            << "  %cls = call ptr %find(ptr %env, ptr %owner)\n"
            << "  %ft1 = load ptr, ptr %env\n"
            << "  %gm0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::GetStaticMethodID << "\n"
            << "  %getmid = load ptr, ptr %gm0\n"
            << "  %mid = call ptr %getmid(ptr %env, ptr %cls, ptr %name, ptr %desc)\n"
            << "  %ft2 = load ptr, ptr %env\n"
            << "  %ca0 = getelementptr ptr, ptr %ft2, i64 " << k.staticSlot << "\n"
            << "  %call = load ptr, ptr %ca0\n"
            << (std::string(k.boundary) == "void" ? "  " : "  %r = ")
            << "call " << k.boundary
            << " (ptr, ptr, ptr, ptr) %call(ptr %env, ptr %cls, ptr %mid, ptr %jv)\n"
            << (std::string(k.boundary) == "void" ? "  ret void\n"
                                                  : "  ret " + std::string(k.boundary) + " %r\n")
            << "}\n\n";
    }

    // check_cast：IsInstanceOf == 0 → 挂起 ClassCastException；返回原引用。
    out << "define internal ptr @mj2x_llvm_check_cast(ptr %env, ptr %obj, ptr %desc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %desc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinstanceof = load ptr, ptr %ii0\n"
           "  %ok = call i32 %isinstanceof(ptr %env, ptr %obj, ptr %cls)\n"
           "  %bad = icmp eq i32 %ok, 0\n"
           "  br i1 %bad, label %fail, label %done\n"
           "fail:\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc = call i32 %thrownew(ptr %env, ptr @_mj2x_llvm_obj_cce, ptr @_mj2x_llvm_obj_null)\n"
           "  br label %done\n"
           "done:\n"
           "  ret ptr %obj\n"
           "}\n\n";

    // instance_of：IsInstanceOf → i32 0/1（intrinsic "instanceof:<desc>"）。
    out << "define internal i32 @mj2x_llvm_instance_of(ptr %env, ptr %obj, ptr %desc) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %findclass = load ptr, ptr %fc0\n"
           "  %cls = call ptr %findclass(ptr %env, ptr %desc)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinstanceof = load ptr, ptr %ii0\n"
           "  %ok = call i32 %isinstanceof(ptr %env, ptr %obj, ptr %cls)\n"
           "  ret i32 %ok\n"
           "}\n\n";

    // 异常基础设施：ExceptionCheck/Occurred/Clear/Matches（catch 类型匹配，
    // 空串 = catch-all/finally）/Throw（athrow；null → NPE）。
    out << "define internal i1 @mj2x_llvm_exception_check(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %ec0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionCheck << "\n"
        << "  %check = load ptr, ptr %ec0\n"
           "  %c = call i32 %check(ptr %env)\n"
           "  %r = icmp ne i32 %c, 0\n"
           "  ret i1 %r\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_exception_occurred(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %eo0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionOccurred << "\n"
        << "  %occ = load ptr, ptr %eo0\n"
           "  %ex = call ptr %occ(ptr %env)\n"
           "  ret ptr %ex\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_exception_clear(ptr %env) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %cl0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::ExceptionClear << "\n"
        << "  %clear = load ptr, ptr %cl0\n"
           "  call void %clear(ptr %env)\n"
           "  ret void\n"
           "}\n\n";
    out << "define internal i1 @mj2x_llvm_exception_matches(ptr %env, ptr %ex, ptr %catch) {\n"
           "entry:\n"
           "  %c0 = load i8, ptr %catch\n"
           "  %isempty = icmp eq i8 %c0, 0\n"
           "  br i1 %isempty, label %yes, label %cls\n"
           "cls:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %c = call ptr %find(ptr %env, ptr %catch)\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %ii0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::IsInstanceOf << "\n"
        << "  %isinst = load ptr, ptr %ii0\n"
           "  %m = call i32 %isinst(ptr %env, ptr %ex, ptr %c)\n"
           "  %r = icmp ne i32 %m, 0\n"
           "  ret i1 %r\n"
           "yes:\n"
           "  ret i1 true\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_throw(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %th0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::Throw << "\n"
        << "  %throw = load ptr, ptr %th0\n"
           "  %rc = call i32 %throw(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";
    // String/Class 常量：NewStringUTF（modified UTF-8 字节）/ FindClass。
    out << "define internal ptr @mj2x_llvm_string_const(ptr %env, ptr %utf8) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %ns0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::NewStringUTF << "\n"
        << "  %newstr = load ptr, ptr %ns0\n"
           "  %s = call ptr %newstr(ptr %env, ptr %utf8)\n"
           "  ret ptr %s\n"
           "}\n\n";
    out << "define internal ptr @mj2x_llvm_class_const(ptr %env, ptr %name) {\n"
           "entry:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %c = call ptr %find(ptr %env, ptr %name)\n"
           "  ret ptr %c\n"
           "}\n\n";
    // MonitorEnter/Exit（null → NPE ThrowNew）。
    out << "define internal void @mj2x_llvm_monitor_enter(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %me0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::MonitorEnter << "\n"
        << "  %me = load ptr, ptr %me0\n"
           "  %rc = call i32 %me(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";
    out << "define internal void @mj2x_llvm_monitor_exit(ptr %env, ptr %obj) {\n"
           "entry:\n"
           "  %isnull = icmp eq ptr %obj, null\n"
           "  br i1 %isnull, label %npe, label %ok\n"
           "ok:\n"
           "  %ft0 = load ptr, ptr %env\n"
           "  %mx0 = getelementptr ptr, ptr %ft0, i64 " << jni_slot::MonitorExit << "\n"
        << "  %mx = load ptr, ptr %mx0\n"
           "  %rc = call i32 %mx(ptr %env, ptr %obj)\n"
           "  ret void\n"
           "npe:\n"
           "  %ft1 = load ptr, ptr %env\n"
           "  %fc0 = getelementptr ptr, ptr %ft1, i64 " << jni_slot::FindClass << "\n"
        << "  %find = load ptr, ptr %fc0\n"
           "  %cls = call ptr %find(ptr %env, ptr @_mj2x_llvm_obj_npe)\n"
           "  %ft2 = load ptr, ptr %env\n"
           "  %tn0 = getelementptr ptr, ptr %ft2, i64 " << jni_slot::ThrowNew << "\n"
        << "  %thrownew = load ptr, ptr %tn0\n"
           "  %rc2 = call i32 %thrownew(ptr %env, ptr %cls, ptr @_mj2x_llvm_obj_null)\n"
           "  ret void\n"
           "}\n\n";

    return out.str();
}

void LlvmIrBackend::finalize(const CompilerIndex&)
{
    std::ostringstream full;
    full << "; ModuleID = 'mj2x_llvm'\n";
    full << "source_filename = \"mj2x_llvm\"\n\n";
    const std::string strings = impl_->strings.emitDefinitions();
    full << strings;
    if (!strings.empty()) full << "\n";
    full << impl_->bodies.str();
    const std::string bodies = impl_->bodies.str();
    if (bodies.find("mj2x_llvm_throw_arith") != std::string::npos) {
        full << emitThrowArithmeticHelper();
    }
    if (bodies.find("mj2x_llvm_field_load_") != std::string::npos ||
        bodies.find("mj2x_llvm_field_store_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_load_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_store_") != std::string::npos ||
        bodies.find("mj2x_llvm_array_length") != std::string::npos ||
        bodies.find("mj2x_llvm_call_") != std::string::npos ||
        bodies.find("mj2x_llvm_new_object") != std::string::npos ||
        bodies.find("mj2x_llvm_new_array") != std::string::npos ||
        bodies.find("mj2x_llvm_new_multi_array") != std::string::npos ||
        bodies.find("mj2x_llvm_check_cast") != std::string::npos ||
        bodies.find("mj2x_llvm_instance_of") != std::string::npos ||
        bodies.find("mj2x_llvm_exception_") != std::string::npos ||
        bodies.find("mj2x_llvm_throw(") != std::string::npos ||
        bodies.find("mj2x_llvm_string_const") != std::string::npos ||
        bodies.find("mj2x_llvm_class_const") != std::string::npos ||
        bodies.find("mj2x_llvm_monitor_") != std::string::npos) {
        full << emitObjectHelpers();
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

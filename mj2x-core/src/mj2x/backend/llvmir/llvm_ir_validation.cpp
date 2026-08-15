/// @file llvm_ir_validation.cpp
/// @brief LLVM IR 后端验证层：RegIR → 寄存器类型表（fail-closed 检查）。
#include "llvm_ir_internal.hpp"

#include <type_traits>
#include <variant>

namespace mj2x {
namespace llvmir {

using namespace jbytecode::core;
using namespace jbytecode::tree;

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

}   // namespace llvmir
}   // namespace mj2x

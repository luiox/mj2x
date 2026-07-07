#include "reg_dump.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

namespace mj2x {

namespace {

const char* typeName(SSAType t) {
    switch (t) {
        case SSAType::VOID: return "void";
        case SSAType::BOOL: return "bool";
        case SSAType::I8: return "i8";
        case SSAType::I16: return "i16";
        case SSAType::CHAR: return "char";
        case SSAType::I32: return "i32";
        case SSAType::I64: return "i64";
        case SSAType::F32: return "f32";
        case SSAType::F64: return "f64";
        case SSAType::REF: return "ref";
        default: return "?";
    }
}

std::string regName(RegId id) {
    std::ostringstream os;
    os << "r" << id.value;
    return os.str();
}

std::string valueName(RegValue value) {
    return regName(value.id) + ":" + typeName(value.type);
}

std::string operandName(const RegOperand& operand) {
    std::ostringstream os;
    switch (operand.kind) {
        case RegOperandKind::Register:
            os << regName(operand.reg);
            break;
        case RegOperandKind::IntLiteral:
            os << operand.intValue;
            break;
        case RegOperandKind::FloatLiteral:
            os << operand.floatValue;
            break;
        case RegOperandKind::NullLiteral:
            os << "null";
            break;
        case RegOperandKind::Opaque:
            os << "<opaque:" << operand.tag << ">";
            break;
    }
    return os.str();
}

void dumpCopy(std::ostringstream& os, const RegCopy& copy) {
    os << valueName(copy.dest) << " <- " << operandName(copy.source);
}

void dumpCopies(std::ostringstream& os, const std::vector<RegCopy>& copies) {
    if (copies.empty()) return;
    os << " { ";
    for (size_t i = 0; i < copies.size(); ++i) {
        if (i) os << "; ";
        dumpCopy(os, copies[i]);
    }
    os << " }";
}

void dumpSuccessor(std::ostringstream& os, const RegSuccessor& successor, const char* prefix) {
    os << "    " << prefix << " block b" << (successor.block ? successor.block->id.value : 0);
    dumpCopies(os, successor.copies);
    os << "\n";
}

void dumpOperandList(std::ostringstream& os, const std::vector<RegOperand>& operands) {
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i) os << ", ";
        os << operandName(operands[i]);
    }
}

const char* binaryOpName(SSABinaryOp op) {
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
        case SSABinaryOp::UShr: return ">>>";
    }
    return "?";
}

const char* compareName(SSAComparePredicate predicate) {
    switch (predicate) {
        case SSAComparePredicate::Eq: return "==";
        case SSAComparePredicate::Ne: return "!=";
        case SSAComparePredicate::Lt: return "<";
        case SSAComparePredicate::Le: return "<=";
        case SSAComparePredicate::Gt: return ">";
        case SSAComparePredicate::Ge: return ">=";
    }
    return "?";
}

void dumpInstruction(std::ostringstream& os, const RegInstruction& ins) {
    os << "  ";
    if (ins.hasDest) os << valueName(ins.dest) << " = ";
    switch (ins.kind) {
        case RegInstructionKind::Copy:
            os << "copy ";
            dumpCopy(os, ins.copy);
            break;
        case RegInstructionKind::Unary:
            os << "neg " << operandName(ins.value);
            break;
        case RegInstructionKind::Binary:
            os << operandName(ins.lhs) << " " << binaryOpName(ins.binaryOp)
               << " " << operandName(ins.rhs);
            break;
        case RegInstructionKind::Compare:
            os << "cmp " << operandName(ins.lhs) << " "
               << compareName(ins.comparePredicate) << " " << operandName(ins.rhs);
            break;
        case RegInstructionKind::Convert:
            os << "convert " << operandName(ins.value) << " -> " << typeName(ins.targetType);
            if (ins.convertKind == SSAConvertKind::ReferenceCast && !ins.referenceTypeDesc.empty()) {
                os << " checkcast " << ins.referenceTypeDesc;
            }
            break;
        case RegInstructionKind::LoadField:
            os << (ins.field.isStatic ? "getstatic " : "getfield ")
               << ins.field.owner << "." << ins.field.name << ":" << ins.field.desc;
            if (!ins.field.isStatic) os << " from " << operandName(ins.receiver);
            break;
        case RegInstructionKind::StoreField:
            os << (ins.field.isStatic ? "putstatic " : "putfield ")
               << ins.field.owner << "." << ins.field.name << ":" << ins.field.desc;
            if (!ins.field.isStatic) os << " on " << operandName(ins.receiver);
            os << " = " << operandName(ins.value);
            break;
        case RegInstructionKind::ArrayLoad:
            os << "array.load " << typeName(ins.elementType)
               << " " << operandName(ins.receiver) << "[" << operandName(ins.index) << "]";
            break;
        case RegInstructionKind::ArrayStore:
            os << "array.store " << typeName(ins.elementType)
               << " " << operandName(ins.receiver) << "[" << operandName(ins.index)
               << "] = " << operandName(ins.value);
            break;
        case RegInstructionKind::Call:
            os << "call " << ins.method.owner << "." << ins.method.name << ins.method.desc << "(";
            dumpOperandList(os, ins.args);
            os << ")";
            break;
        case RegInstructionKind::Allocate: {
            const char* kind = "object";
            if (ins.allocationKind == SSAAllocationKind::Array) kind = "array";
            if (ins.allocationKind == SSAAllocationKind::MultiArray) kind = "multiarray";
            os << "alloc " << kind << " " << ins.typeDesc << "(";
            dumpOperandList(os, ins.args);
            os << ")";
            break;
        }
        case RegInstructionKind::Intrinsic:
            os << "intrinsic " << ins.intrinsicName << "(";
            dumpOperandList(os, ins.args);
            os << ")";
            break;
        case RegInstructionKind::Barrier:
            os << "barrier " << ins.barrierReason;
            break;
    }
    os << "\n";
}

} // namespace

std::string dumpRegIR(const RegFunction& fn) {
    std::ostringstream os;
    os << "function " << fn.symbol.owner << "." << fn.symbol.name << fn.symbol.desc << "\n";
    os << "regs:";
    for (const auto& param : fn.parameters) os << " " << valueName(param);
    os << "\n\n";

    for (const auto& block : fn.blocks) {
        os << "block b" << block->id.value;
        if (block->l1LabelId >= 0) os << " (l1=" << block->l1LabelId << ")";
        if (block.get() == fn.entry) os << " ; ENTRY";
        os << ":\n";

        for (const auto& ins : block->instructions) dumpInstruction(os, ins);

        std::visit([&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                dumpSuccessor(os, t.target, "goto");
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                os << "    br " << operandName(t.condition) << "\n";
                dumpSuccessor(os, t.trueTarget, "  T:");
                dumpSuccessor(os, t.falseTarget, "  F:");
            } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                os << "    switch " << operandName(t.value) << "\n";
                for (const auto& c : t.cases) {
                    std::ostringstream prefix;
                    prefix << "  case " << c.key << ":";
                    dumpSuccessor(os, c.target, prefix.str().c_str());
                }
                dumpSuccessor(os, t.defaultTarget, "  default:");
            } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                os << "    ret";
                if (t.hasValue) os << " " << operandName(t.value);
                os << "\n";
            } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                os << "    throw " << operandName(t.value) << "\n";
            } else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                os << "    unreachable\n";
            }
        }, block->terminator);
        os << "\n";
    }

    return os.str();
}

} // namespace mj2x

#include "reg_dump.hpp"

#include "mj2x/codegen/c_helpers.hpp"

#include <charconv>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

namespace mj2x {

namespace {

/// @brief Appendix to a UTF-8 builder without dynamic string temporaries.
void appendStr(ca::str::Utf8StringBuilder& b, std::string_view text)
{
    b.append(text.data(), text.size());
}

void appendInt(ca::str::Utf8StringBuilder& b, int64_t value)
{
    const std::string digits = std::to_string(value);
    appendStr(b, digits);
}

void appendDouble(ca::str::Utf8StringBuilder& b, double value)
{
    char       buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc())
        b.append(reinterpret_cast<const char*>(buffer), static_cast<ca::usize>(result.ptr - buffer));
    else
        b.append("?");
}

/// @brief JVM 符号字节的注释安全转义（共享实现，见 mj2x/codegen/c_helpers.hpp）。
using codegen::escapeCommentIdent;

const char* typeName(SSAType t)
{
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

void appendRegName(ca::str::Utf8StringBuilder& b, RegId id)
{
    b.append("r");
    appendInt(b, id.value);
}

void appendValueName(ca::str::Utf8StringBuilder& b, RegValue value)
{
    appendRegName(b, value.id);
    b.append(":");
    b.append(typeName(value.type));
}

void appendOperandName(ca::str::Utf8StringBuilder& b, const RegOperand& operand)
{
    switch (operand.kind) {
    case RegOperandKind::Register: appendRegName(b, operand.reg); break;
    case RegOperandKind::IntLiteral: appendInt(b, operand.intValue); break;
    case RegOperandKind::FloatLiteral: appendDouble(b, operand.floatValue); break;
    case RegOperandKind::NullLiteral: b.append("null"); break;
    case RegOperandKind::Opaque:
        b.append("<opaque:");
        appendStr(b, operand.tag);
        b.append(">");
        break;
    }
}

void dumpCopy(ca::str::Utf8StringBuilder& b, const RegCopy& copy)
{
    appendValueName(b, copy.dest);
    b.append(" <- ");
    appendOperandName(b, copy.source);
}

void dumpCopies(ca::str::Utf8StringBuilder& b, const std::vector<RegCopy>& copies)
{
    if (copies.empty()) return;
    b.append(" { ");
    for (size_t i = 0; i < copies.size(); ++i) {
        if (i) b.append("; ");
        dumpCopy(b, copies[i]);
    }
    b.append(" }");
}

void dumpSuccessor(ca::str::Utf8StringBuilder& b, const RegSuccessor& successor, const char* prefix)
{
    b.append("    ");
    b.append(prefix);
    b.append(" block b");
    appendInt(b, successor.block ? successor.block->id.value : 0);
    dumpCopies(b, successor.copies);
    b.append("\n");
}

void dumpOperandList(ca::str::Utf8StringBuilder& b, const std::vector<RegOperand>& operands)
{
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i) b.append(", ");
        appendOperandName(b, operands[i]);
    }
}

const char* binaryOpName(SSABinaryOp op)
{
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

const char* compareName(SSAComparePredicate predicate)
{
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

/// @brief JVM symbol text (owner/name/desc) is arbitrary bytes; escape it so the
///        dump/comment stream stays valid ASCII while remaining traceable.
void appendSymbol(ca::str::Utf8StringBuilder& b, const ca::str::Utf8StringRef& symbol)
{
    const auto escaped = escapeCommentIdent(symbol);
    appendStr(b, std::string_view(escaped));
}
/// @brief std::string 重载：SSAFunctionSymbol 等交付边界仍持有 std::string，
///        经 string_view 视图统一进入转义路径。
void appendSymbol(ca::str::Utf8StringBuilder& b, std::string_view symbol)
{
    appendSymbol(b, ca::str::Utf8StringRef::from_string_view(symbol));
}

void dumpInstructionLine(ca::str::Utf8StringBuilder& b, const RegInstruction& ins)
{
    if (ins.hasDest) {
        appendValueName(b, ins.dest);
        b.append(" = ");
    }
    switch (ins.kind) {
    case RegInstructionKind::Copy:
        b.append("copy ");
        dumpCopy(b, ins.copy);
        break;
    case RegInstructionKind::Unary:
        b.append("neg ");
        appendOperandName(b, ins.value);
        break;
    case RegInstructionKind::Binary:
        appendOperandName(b, ins.lhs);
        b.append(" ");
        b.append(binaryOpName(ins.binaryOp));
        b.append(" ");
        appendOperandName(b, ins.rhs);
        break;
    case RegInstructionKind::Compare:
        b.append("cmp ");
        appendOperandName(b, ins.lhs);
        b.append(" ");
        b.append(compareName(ins.comparePredicate));
        b.append(" ");
        appendOperandName(b, ins.rhs);
        break;
    case RegInstructionKind::Convert:
        b.append("convert ");
        appendOperandName(b, ins.value);
        b.append(" -> ");
        b.append(typeName(ins.targetType));
        if (ins.convertKind == SSAConvertKind::ReferenceCast && !ins.referenceTypeDesc.is_empty()) {
            b.append(" checkcast ");
            appendSymbol(b, ins.referenceTypeDesc);
        }
        break;
    case RegInstructionKind::LoadField:
        b.append(ins.field.isStatic ? "getstatic " : "getfield ");
        appendSymbol(b, ins.field.owner);
        b.append(".");
        appendSymbol(b, ins.field.name);
        b.append(":");
        appendSymbol(b, ins.field.desc);
        if (!ins.field.isStatic) {
            b.append(" from ");
            appendOperandName(b, ins.receiver);
        }
        break;
    case RegInstructionKind::StoreField:
        b.append(ins.field.isStatic ? "putstatic " : "putfield ");
        appendSymbol(b, ins.field.owner);
        b.append(".");
        appendSymbol(b, ins.field.name);
        b.append(":");
        appendSymbol(b, ins.field.desc);
        if (!ins.field.isStatic) {
            b.append(" on ");
            appendOperandName(b, ins.receiver);
        }
        b.append(" = ");
        appendOperandName(b, ins.value);
        break;
    case RegInstructionKind::ArrayLoad:
        b.append("array.load ");
        b.append(typeName(ins.elementType));
        b.append(" ");
        appendOperandName(b, ins.receiver);
        b.append("[");
        appendOperandName(b, ins.index);
        b.append("]");
        break;
    case RegInstructionKind::ArrayStore:
        b.append("array.store ");
        b.append(typeName(ins.elementType));
        b.append(" ");
        appendOperandName(b, ins.receiver);
        b.append("[");
        appendOperandName(b, ins.index);
        b.append("] = ");
        appendOperandName(b, ins.value);
        break;
    case RegInstructionKind::Call:
        b.append("call ");
        appendSymbol(b, ins.method.owner);
        b.append(".");
        appendSymbol(b, ins.method.name);
        appendSymbol(b, ins.method.desc);
        b.append("(");
        dumpOperandList(b, ins.args);
        b.append(")");
        break;
    case RegInstructionKind::Allocate:
    {
        const char* kind = "object";
        if (ins.allocationKind == SSAAllocationKind::Array) kind = "array";
        if (ins.allocationKind == SSAAllocationKind::MultiArray) kind = "multiarray";
        b.append("alloc ");
        b.append(kind);
        b.append(" ");
        appendSymbol(b, ins.typeDesc);
        b.append("(");
        dumpOperandList(b, ins.args);
        b.append(")");
        break;
    }
    case RegInstructionKind::Intrinsic:
        b.append("intrinsic ");
        appendStr(b, ins.intrinsicName);
        b.append("(");
        dumpOperandList(b, ins.args);
        b.append(")");
        break;
    case RegInstructionKind::Barrier:
        b.append("barrier ");
        appendStr(b, ins.barrierReason);
        break;
    }
}

void dumpTerminatorLines(ca::str::Utf8StringBuilder& b, const RegBlock& block)
{
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                dumpSuccessor(b, t.target, "goto");
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                b.append("    br ");
                appendOperandName(b, t.condition);
                b.append("\n");
                dumpSuccessor(b, t.trueTarget, "  T:");
                dumpSuccessor(b, t.falseTarget, "  F:");
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                b.append("    switch ");
                appendOperandName(b, t.value);
                b.append("\n");
                for (const auto& c : t.cases) {
                    ca::str::Utf8StringBuilder prefix;
                    prefix.append("  case ");
                    appendInt(prefix, c.key);
                    prefix.append(":");
                    dumpSuccessor(b, c.target, prefix.build().c_str());
                }
                dumpSuccessor(b, t.defaultTarget, "  default:");
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                b.append("    ret");
                if (t.hasValue) {
                    b.append(" ");
                    appendOperandName(b, t.value);
                }
                b.append("\n");
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                b.append("    throw ");
                appendOperandName(b, t.value);
                b.append("\n");
            }
            else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                b.append("    unreachable\n");
            }
        },
        block.terminator);
}

}   // namespace

ca::str::Utf8String regInstructionLine(const RegInstruction& ins)
{
    ca::str::Utf8StringBuilder b;
    dumpInstructionLine(b, ins);
    return b.build();
}

ca::str::Utf8String regTerminatorLines(const RegBlock& block)
{
    ca::str::Utf8StringBuilder b;
    dumpTerminatorLines(b, block);
    return b.build();
}

std::string dumpRegIR(const RegFunction& fn)
{
    ca::str::Utf8StringBuilder b;
    b.append("function ");
    appendSymbol(b, fn.symbol.owner);
    b.append(".");
    appendSymbol(b, fn.symbol.name);
    appendSymbol(b, fn.symbol.desc);
    b.append("\n");
    b.append("regs:");
    for (const auto& param : fn.parameters) {
        b.append(" ");
        appendValueName(b, param);
    }
    b.append("\n\n");

    for (const auto& block : fn.blocks) {
        b.append("block b");
        appendInt(b, block->id.value);
        if (block->l1LabelId >= 0) {
            b.append(" (l1=");
            appendInt(b, block->l1LabelId);
            b.append(")");
        }
        if (block.get() == fn.entry) b.append(" ; ENTRY");
        b.append(":\n");

        for (const auto& ins : block->instructions) {
            b.append("  ");
            dumpInstructionLine(b, ins);
            b.append("\n");
        }

        dumpTerminatorLines(b, *block);
        b.append("\n");
    }

    return b.build().to_std_string();
}

}   // namespace mj2x
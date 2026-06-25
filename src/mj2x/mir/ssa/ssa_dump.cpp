#include "ssa_dump.hpp"

#include <sstream>
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

// value 引用名（注意：刻意不用 cstack/v<slot> 等 c2j 锚点命名）
// RTTI 禁用，用 valueKind 标签判类型，不用 dynamic_cast。
std::string valueName(const SSAValue* v) {
    if (!v) return "<null>";
    switch (v->valueKind) {
        case SSAValueKind::Parameter: {
            auto* p = static_cast<const SSAParameter*>(v);
            std::ostringstream os; os << "param" << p->index; return os.str();
        }
        case SSAValueKind::BlockArg: {
            auto* a = static_cast<const SSABlockArg*>(v);
            std::ostringstream os; os << "b" << a->id.value; return os.str();
        }
        case SSAValueKind::InstructionResult: {
            auto* r = static_cast<const SSAInstructionResult*>(v);
            std::ostringstream os; os << "v" << r->id.value; return os.str();
        }
        case SSAValueKind::IntLiteral: {
            auto* lit = static_cast<const SSAIntLiteral*>(v);
            std::ostringstream os; os << lit->value; return os.str();
        }
        case SSAValueKind::FloatLiteral: {
            auto* lit = static_cast<const SSAFloatLiteral*>(v);
            std::ostringstream os; os << lit->value; return os.str();
        }
        case SSAValueKind::NullLiteral: return "null";
        case SSAValueKind::Opaque: {
            auto* o = static_cast<const SSAOpaqueValue*>(v);
            return "<opaque:" + o->tag + ">";
        }
    }
    return "<?>";
}

void dumpSuccessor(std::ostringstream& os, const SSASuccessor& s, const char* prefix) {
    os << "    " << prefix << " block b" << (s.block ? s.block->id.value : -1);
    if (!s.args.empty()) {
        os << "(";
        for (size_t i = 0; i < s.args.size(); ++i) {
            if (i) os << ", ";
            os << valueName(s.args[i]);
        }
        os << ")";
    }
    os << "\n";
}

} // namespace

std::string dumpSSA(const SSAFunction& fn) {
    std::ostringstream os;
    os << "function " << fn.symbol.owner << "." << fn.symbol.name << fn.symbol.desc << "\n";
    os << "params: ";
    for (auto& p : fn.parameters) os << valueName(p.get()) << ":" << typeName(p->type) << " ";
    os << "\n\n";

    for (auto& b : fn.blocks) {
        os << "block b" << b->id.value;
        if (b->l1LabelId >= 0) os << " (l1=" << b->l1LabelId << ")";
        if (!b->args.empty()) {
            os << "(";
            for (size_t i = 0; i < b->args.size(); ++i) {
                if (i) os << ", ";
                os << "b" << b->args[i].id.value << ":" << typeName(b->args[i].type)
                   << "@slot" << b->args[i].slot;
            }
            os << ")";
        }
        if (b.get() == fn.entry) os << "  ; ENTRY";
        os << ":\n";

        for (auto& ins : b->instructions) {
            os << "  ";
            if (ins->hasResult) os << valueName(&ins->result) << " = ";
            // RTTI 禁用，按 kind 标签判类型（不用 dynamic_cast）。
            switch (ins->kind) {
                case SSAInstructionKind::Unary: {
                    auto* u = static_cast<SSAUnaryInstruction*>(ins.get());
                    os << "neg " << valueName(u->value);
                    break;
                }
                case SSAInstructionKind::Binary: {
                    auto* bi = static_cast<SSABinaryInstruction*>(ins.get());
                    const char* opn = "+";
                    switch (bi->op) {
                        case SSABinaryOp::Add: opn = "+"; break;
                        case SSABinaryOp::Sub: opn = "-"; break;
                        case SSABinaryOp::Mul: opn = "*"; break;
                        case SSABinaryOp::Div: opn = "/"; break;
                        case SSABinaryOp::Rem: opn = "%"; break;
                        case SSABinaryOp::And: opn = "&"; break;
                        case SSABinaryOp::Or: opn = "|"; break;
                        case SSABinaryOp::Xor: opn = "^"; break;
                        case SSABinaryOp::Shl: opn = "<<"; break;
                        case SSABinaryOp::Shr: opn = ">>"; break;
                        case SSABinaryOp::UShr: opn = ">>>"; break;
                    }
                    os << valueName(bi->lhs) << " " << opn << " " << valueName(bi->rhs);
                    break;
                }
                case SSAInstructionKind::Compare: {
                    auto* c = static_cast<SSACompareInstruction*>(ins.get());
                    const char* opn = "==";
                    switch (c->predicate) {
                        case SSAComparePredicate::Eq: opn = "=="; break;
                        case SSAComparePredicate::Ne: opn = "!="; break;
                        case SSAComparePredicate::Lt: opn = "<"; break;
                        case SSAComparePredicate::Le: opn = "<="; break;
                        case SSAComparePredicate::Gt: opn = ">"; break;
                        case SSAComparePredicate::Ge: opn = ">="; break;
                    }
                    os << "cmp " << valueName(c->lhs) << " " << opn << " " << valueName(c->rhs);
                    break;
                }
                default:
                    os << "<instr kind=" << static_cast<int>(ins->kind) << ">";
                    break;
            }
            os << "\n";
        }

        // terminator
        std::visit([&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, SSAJumpTerminator>) {
                dumpSuccessor(os, t.target, "goto");
            } else if constexpr (std::is_same_v<T, SSABranchTerminator>) {
                os << "    br " << valueName(t.condition) << "\n";
                dumpSuccessor(os, t.trueTarget, "  T:");
                dumpSuccessor(os, t.falseTarget, "  F:");
            } else if constexpr (std::is_same_v<T, SSAReturnTerminator>) {
                os << "    ret " << (t.value ? valueName(t.value) : "") << "\n";
            } else if constexpr (std::is_same_v<T, SSAThrowTerminator>) {
                os << "    throw " << valueName(t.value) << "\n";
            } else if constexpr (std::is_same_v<T, SSAUnreachableTerminator>) {
                os << "    unreachable\n";
            } else {
                os << "    <terminator>\n";
            }
        }, b->terminator);
        os << "\n";
    }
    return os.str();
}

} // namespace mj2x

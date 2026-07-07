#include "opcode_utils.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

BinaryOp opcodeToBinaryOp(int opcode) {
    switch (opcode) {
    case Opcodes::IADD: case Opcodes::LADD:
    case Opcodes::FADD: case Opcodes::DADD: return BinaryOp::ADD;
    case Opcodes::ISUB: case Opcodes::LSUB:
    case Opcodes::FSUB: case Opcodes::DSUB: return BinaryOp::SUB;
    case Opcodes::IMUL: case Opcodes::LMUL:
    case Opcodes::FMUL: case Opcodes::DMUL: return BinaryOp::MUL;
    case Opcodes::IDIV: case Opcodes::LDIV:
    case Opcodes::FDIV: case Opcodes::DDIV: return BinaryOp::DIV;
    case Opcodes::IREM: case Opcodes::LREM:
    case Opcodes::FREM: case Opcodes::DREM: return BinaryOp::REM;
    case Opcodes::ISHL: case Opcodes::LSHL: return BinaryOp::SHL;
    case Opcodes::ISHR: case Opcodes::LSHR: return BinaryOp::SHR;
    case Opcodes::IUSHR: case Opcodes::LUSHR: return BinaryOp::USHR;
    case Opcodes::IAND: case Opcodes::LAND: return BinaryOp::AND;
    case Opcodes::IOR: case Opcodes::LOR: return BinaryOp::OR;
    case Opcodes::IXOR: case Opcodes::LXOR: return BinaryOp::XOR;
    default: return BinaryOp::ADD;
    }
}

UnaryOp opcodeToUnaryOp(int opcode) {
    (void)opcode;
    return UnaryOp::NEG;
}

PrimitiveKind opcodeToResultType(int opcode) {
    switch (opcode) {
    case Opcodes::IADD: case Opcodes::ISUB: case Opcodes::IMUL:
    case Opcodes::IDIV: case Opcodes::IREM:
    case Opcodes::ISHL: case Opcodes::ISHR: case Opcodes::IUSHR:
    case Opcodes::IAND: case Opcodes::IOR: case Opcodes::IXOR:
    case Opcodes::INEG:
        return PrimitiveKind::INT;
    case Opcodes::LADD: case Opcodes::LSUB: case Opcodes::LMUL:
    case Opcodes::LDIV: case Opcodes::LREM:
    case Opcodes::LSHL: case Opcodes::LSHR: case Opcodes::LUSHR:
    case Opcodes::LAND: case Opcodes::LOR: case Opcodes::LXOR:
    case Opcodes::LNEG:
        return PrimitiveKind::LONG;
    case Opcodes::FADD: case Opcodes::FSUB: case Opcodes::FMUL:
    case Opcodes::FDIV: case Opcodes::FREM: case Opcodes::FNEG:
        return PrimitiveKind::FLOAT;
    case Opcodes::DADD: case Opcodes::DSUB: case Opcodes::DMUL:
    case Opcodes::DDIV: case Opcodes::DREM: case Opcodes::DNEG:
        return PrimitiveKind::DOUBLE;
    case Opcodes::I2L: return PrimitiveKind::LONG;
    case Opcodes::I2F: return PrimitiveKind::FLOAT;
    case Opcodes::I2D: return PrimitiveKind::DOUBLE;
    case Opcodes::L2I: return PrimitiveKind::INT;
    case Opcodes::L2F: return PrimitiveKind::FLOAT;
    case Opcodes::L2D: return PrimitiveKind::DOUBLE;
    case Opcodes::F2I: return PrimitiveKind::INT;
    case Opcodes::F2L: return PrimitiveKind::LONG;
    case Opcodes::F2D: return PrimitiveKind::DOUBLE;
    case Opcodes::D2I: return PrimitiveKind::INT;
    case Opcodes::D2L: return PrimitiveKind::LONG;
    case Opcodes::D2F: return PrimitiveKind::FLOAT;
    default: return PrimitiveKind::INT;
    }
}

CmpOp branchOpcodeToCmpOp(int opcode) {
    switch (opcode) {
    case Opcodes::IF_ICMPEQ: case Opcodes::IF_ACMPEQ: return CmpOp::EQ;
    case Opcodes::IF_ICMPNE: case Opcodes::IF_ACMPNE: return CmpOp::NE;
    case Opcodes::IF_ICMPLT: return CmpOp::LT;
    case Opcodes::IF_ICMPGE: return CmpOp::GE;
    case Opcodes::IF_ICMPGT: return CmpOp::GT;
    case Opcodes::IF_ICMPLE: return CmpOp::LE;
    default: return CmpOp::EQ;
    }
}

CmpOp branchZeroOpcodeToCmpOp(int opcode) {
    switch (opcode) {
    case Opcodes::IFEQ: case Opcodes::IFNULL: return CmpOp::EQ;
    case Opcodes::IFNE: case Opcodes::IFNONNULL: return CmpOp::NE;
    case Opcodes::IFLT: return CmpOp::LT;
    case Opcodes::IFGE: return CmpOp::GE;
    case Opcodes::IFGT: return CmpOp::GT;
    case Opcodes::IFLE: return CmpOp::LE;
    default: return CmpOp::EQ;
    }
}

} // namespace mj2x


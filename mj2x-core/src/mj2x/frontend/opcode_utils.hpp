#pragma once

#include "mir_node.hpp"


namespace mj2x {

BinaryOp      opcodeToBinaryOp(int opcode);
UnaryOp       opcodeToUnaryOp(int opcode);
PrimitiveKind opcodeToResultType(int opcode);
CmpOp         branchOpcodeToCmpOp(int opcode);
CmpOp         branchZeroOpcodeToCmpOp(int opcode);

}   // namespace mj2x

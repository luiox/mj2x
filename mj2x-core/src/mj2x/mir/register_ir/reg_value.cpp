#include "reg_value.hpp"

#include <utility>

namespace mj2x {

RegOperand RegOperand::fromRegister(RegValue value) {
    RegOperand operand;
    operand.kind = RegOperandKind::Register;
    operand.type = value.type;
    operand.reg = value.id;
    return operand;
}

RegOperand RegOperand::fromInt(int64_t value, SSAType type) {
    RegOperand operand;
    operand.kind = RegOperandKind::IntLiteral;
    operand.type = type;
    operand.intValue = value;
    return operand;
}

RegOperand RegOperand::fromFloat(double value, SSAType type) {
    RegOperand operand;
    operand.kind = RegOperandKind::FloatLiteral;
    operand.type = type;
    operand.floatValue = value;
    return operand;
}

RegOperand RegOperand::nullRef() {
    RegOperand operand;
    operand.kind = RegOperandKind::NullLiteral;
    operand.type = SSAType::REF;
    return operand;
}

RegOperand RegOperand::opaque(SSAType type, std::string tag) {
    RegOperand operand;
    operand.kind = RegOperandKind::Opaque;
    operand.type = type;
    operand.tag = std::move(tag);
    return operand;
}

} // namespace mj2x

/// @file reg_value.hpp
/// @brief Register IR value and operand model.
///
/// @details
/// RegIR is the L3 handoff between block-argument SSA and the future RegC
/// backend.  It deliberately has no JVM stack-slot operand kind: every computed
/// value is addressed by a virtual register, while literals remain immediates.
/// This keeps the structural "no cstack/sp anchor" property enforceable by the
/// type model instead of by post-hoc string filtering.
#pragma once

#include "mj2x/mir/ssa/ssa_type.hpp"

#include <cstdint>
#include <string>

namespace mj2x {

/// @brief Virtual register identifier inside one RegFunction.
struct RegId
{
    uint32_t value = 0;
    bool     operator==(const RegId& other) const { return value == other.value; }
    bool     operator!=(const RegId& other) const { return value != other.value; }
};

/// @brief A typed virtual register definition.
struct RegValue
{
    RegId   id;
    SSAType type = SSAType::UNKNOWN;
};

/// @brief Operand tag used for no-RTTI RegIR dispatch.
enum class RegOperandKind : uint8_t
{
    Register,
    IntLiteral,
    FloatLiteral,
    NullLiteral,
    Opaque,
};

/// @brief A source operand for RegIR instructions and edge transfers.
///
/// @details
/// Opaque operands represent modeled-later JVM values such as String LDCs or
/// undefined defensive fallbacks.  They are still stackless: consumers must not
/// interpret their tag as a JVM local or stack position.
struct RegOperand
{
    RegOperandKind kind = RegOperandKind::Opaque;
    SSAType        type = SSAType::UNKNOWN;
    RegId          reg{};
    int64_t        intValue   = 0;
    double         floatValue = 0.0;
    std::string    tag;

    static RegOperand fromRegister(RegValue value);
    static RegOperand fromInt(int64_t value, SSAType type);
    static RegOperand fromFloat(double value, SSAType type);
    static RegOperand nullRef();
    static RegOperand opaque(SSAType type, std::string tag);
};

}   // namespace mj2x

/// @file ssa_value.hpp
/// @brief SSA value hierarchy shared by parameters, block args, literals, and instruction results.
///
/// @details
/// SSAValue is the operand currency of mj2x's stackless IR.  A non-literal value
/// has a stable id inside its SSAFunction and can be referenced many times after
/// a single definition.  Literal values are owned by SSAFunction as interned
/// constants and intentionally do not consume normal value ids.
#pragma once

#include "ssa_ids.hpp"
#include "ssa_type.hpp"

#include <libca/str/utf8_string.hpp>

#include <cstdint>
#include <string>

namespace mj2x {

/// @brief Synthetic block-arg slot used for an exception object entering a handler.
inline constexpr int kSSAExceptionObjectSlot = -1;

/// @brief Base slot for synthetic block args that represent stack-map join values.
inline constexpr int kSSAStackValueSlotBase = -1000;

/// @brief Return the synthetic slot for an operand-stack entry at bottom-to-top index `depth`.
constexpr int ssaStackValueSlot(int depth)
{
    return kSSAStackValueSlotBase - depth;
}

/// @brief True when `slot` identifies an operand-stack join value instead of a JVM local.
constexpr bool isSSAStackValueSlot(int slot)
{
    return slot <= kSSAStackValueSlotBase;
}

/// @brief Convert a synthetic operand-stack slot back to its bottom-to-top index.
constexpr int ssaStackValueDepth(int slot)
{
    return kSSAStackValueSlotBase - slot;
}

/// @brief Value tag used for no-RTTI dispatch.
enum class SSAValueKind : uint8_t
{
    Parameter,
    BlockArg,
    InstructionResult,
    IntLiteral,
    FloatLiteral,
    NullLiteral,
    Opaque,
};

/// @brief Base class for every SSA operand.
///
/// @details
/// Parameters, block args, instruction results, and opaque values receive stable
/// ids.  Literals are still SSAValue operands but may leave `id` empty because
/// they are printed and consumed by immediate value.
struct SSAValue
{
    SSAValueKind valueKind;
    SSAValueId   id;
    SSAType      type = SSAType::UNKNOWN;

    SSAValue()
        : valueKind(SSAValueKind::Opaque)
    {}
    SSAValue(SSAValueKind vk, SSAValueId id_, SSAType t)
        : valueKind(vk)
        , id(id_)
        , type(t)
    {}
    virtual ~SSAValue() = default;
};

/// @brief Method entry parameter.
///
/// @details
/// `index` is the JVM local slot, not the ordinal parameter number.  Instance
/// methods therefore bind `this` at slot 0, and category-2 `long`/`double`
/// parameters leave the expected one-slot gap before the next parameter.
struct SSAParameter : SSAValue
{
    uint32_t index = 0;   ///< JVM local slot index.
    SSAParameter()
        : SSAValue(SSAValueKind::Parameter, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSAParameter(SSAValueId id_, SSAType t, uint32_t idx)
        : SSAValue(SSAValueKind::Parameter, id_, t)
        , index(idx)
    {}
};

/// @brief Block entry parameter representing an implicit phi.
///
/// @details
/// Every predecessor passes one value for each block arg through its successor
/// edge. Non-negative `slot` values record the JVM local slot whose reaching
/// definitions forced the argument to exist. `kSSAExceptionObjectSlot` is the
/// synthetic handler-entry exception object. `ssaStackValueSlot(depth)` records
/// a StackMapTable operand-stack value at block entry, used by javac shapes such
/// as ternary joins where branches push a value and the join block consumes it.
struct SSABlockArg : SSAValue
{
    int slot = 0;   ///< Local slot, exception slot, or synthetic stack-map slot.
    SSABlockArg()
        : SSAValue(SSAValueKind::BlockArg, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSABlockArg(SSAValueId id_, SSAType t, int s)
        : SSAValue(SSAValueKind::BlockArg, id_, t)
        , slot(s)
    {}
};

/// @brief Result value uniquely defined by one SSAInstruction.
struct SSAInstructionResult : SSAValue
{
    SSAInstructionResult()
        : SSAValue(SSAValueKind::InstructionResult, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSAInstructionResult(SSAValueId id_, SSAType t)
        : SSAValue(SSAValueKind::InstructionResult, id_, t)
    {}
};

/// @brief Integer literal from ICONST/BIPUSH/SIPUSH/LDC int/long.
struct SSAIntLiteral : SSAValue
{
    int64_t value = 0;
    SSAIntLiteral()
        : SSAValue(SSAValueKind::IntLiteral, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSAIntLiteral(int64_t v, SSAType t)
        : SSAValue(SSAValueKind::IntLiteral, SSAValueId{}, t)
        , value(v)
    {}
    // literal 不占正式 value id 槽位（去重/内联），id 留空；dump 时按 value 印。
};

/// @brief Floating literal from FCONST/DCONST/LDC float/double.
struct SSAFloatLiteral : SSAValue
{
    double value = 0.0;
    SSAFloatLiteral()
        : SSAValue(SSAValueKind::FloatLiteral, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSAFloatLiteral(double v, SSAType t)
        : SSAValue(SSAValueKind::FloatLiteral, SSAValueId{}, t)
        , value(v)
    {}
};

/// @brief The singleton null reference literal.
struct SSANullLiteral : SSAValue
{
    SSANullLiteral()
        : SSAValue(SSAValueKind::NullLiteral, SSAValueId{}, SSAType::REF)
    {}
};

/// @brief Conservative value for modeled-later JVM objects or undefined inputs.
///
/// @details
/// Opaque values keep SSA construction total for inputs such as LDC String or
/// defensive undefined-local fallbacks while still making the dump visibly
/// different from a real computed value.
struct SSAOpaqueValue : SSAValue
{
    // tag 是 SSA 把 LDC 字面量/未定义局部「夹带」通过 opaque value 的派生标记
    // （如 "ldc_string:<literal>"），非 JVM 符号节点字段，保留 std::string。
    std::string tag;   // 调试用（如 "ldc_string", "undef_local_3"）
    SSAOpaqueValue()
        : SSAValue(SSAValueKind::Opaque, SSAValueId{}, SSAType::UNKNOWN)
    {}
    SSAOpaqueValue(SSAValueId id_, SSAType t, std::string tag_)
        : SSAValue(SSAValueKind::Opaque, id_, t)
        , tag(std::move(tag_))
    {}
};

}   // namespace mj2x

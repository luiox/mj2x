/// @file reg_instruction.hpp
/// @brief Register IR instruction families.
///
/// @details
/// RegIR instructions mirror the stackless SSA operation families but replace
/// SSAValue pointers with RegOperand/RegValue records.  Block arguments are not
/// represented here; they are eliminated into RegCopy records attached to CFG
/// successor edges.
#pragma once

#include "mj2x/mir/register_ir/reg_value.hpp"
#include "mj2x/mir/ssa/ssa_effect.hpp"
#include "mj2x/mir/ssa/ssa_instruction.hpp"

#include <string>
#include <vector>

namespace mj2x {

/// @brief Instruction tag used for no-RTTI dispatch.
enum class RegInstructionKind : uint8_t
{
    Copy,
    Unary,
    Binary,
    Compare,
    Convert,
    LoadField,
    StoreField,
    ArrayLoad,
    ArrayStore,
    Call,
    Allocate,
    Intrinsic,
    Barrier,
};

/// @brief Explicit register copy used by phi/block-argument elimination.
///
/// @details
/// Copies can appear as normal instructions for local rewrites or on successor
/// edges for branch-sensitive block argument transfers.
struct RegCopy
{
    RegValue   dest;
    RegOperand source;
};

/// @brief One block-local RegIR instruction.
///
/// @details
/// The active fields are selected by `kind`.  A compact tagged struct keeps the
/// model simple and avoids RTTI.  `hasDest` is false for stores, void calls,
/// barriers, monitor intrinsics, and other side-effect-only instructions.
struct RegInstruction
{
    RegInstructionKind kind    = RegInstructionKind::Copy;
    bool               hasDest = false;
    RegValue           dest{};
    SSAEffect          effect;
    /// @brief 调用目标间接化标记（`applyCallIndirections` 写入）。
    ///
    /// @details
    /// 置位时发射端走 per-signature dispatcher（调用点只带站点索引），
    /// 替代把 owner/name/desc 身份字符串直接落在调用点上。
    /// 已解析到"已转换 native 函数"的目标保留直接 C 调用快路径
    /// （间接化它是纯开销，不产生任何混淆收益）。
    bool indirectCall = false;

    RegCopy             copy{};
    SSAUnaryOp          unaryOp          = SSAUnaryOp::Neg;
    SSABinaryOp         binaryOp         = SSABinaryOp::Add;
    SSAComparePredicate comparePredicate = SSAComparePredicate::Eq;
    SSAConvertKind      convertKind      = SSAConvertKind::Numeric;
    SSAType             targetType       = SSAType::UNKNOWN;
    ca::str::Utf8StringRef referenceTypeDesc;

    RegOperand value;
    RegOperand lhs;
    RegOperand rhs;
    RegOperand receiver;
    RegOperand index;

    SSAFieldRef             field;
    SSAMethodRef            method;
    SSAAllocationKind       allocationKind = SSAAllocationKind::Object;
    ca::str::Utf8StringRef  typeDesc;
    std::string             intrinsicName;
    std::string             barrierReason;
    SSAType                 elementType = SSAType::UNKNOWN;
    std::vector<RegOperand> args;
};

}   // namespace mj2x

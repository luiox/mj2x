/**
 * @file ssa_instruction.hpp
 * @brief Stackless SSA instruction families used inside SSABlock.
 *
 * @details
 * Instructions model non-control-transfer work only; block exits live in
 * `ssa_terminator.hpp`.  Every instruction carries a tag instead of relying on
 * RTTI, and every result is represented as an SSAInstructionResult embedded in
 * the instruction object.  Calls, stores, barriers, and void intrinsics have no
 * result but still carry SSAEffect information so RegC and future passes can
 * preserve ordering around memory, resolution, and throwing operations.
 */
#pragma once

#include "ssa_effect.hpp"
#include "ssa_ids.hpp"
#include "ssa_type.hpp"
#include "ssa_value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mj2x {

/// @brief Instruction tag used for no-RTTI dispatch.
enum class SSAInstructionKind : uint8_t {
    Unary, Binary, Compare, Convert,
    LoadField, StoreField,
    ArrayLoad, ArrayStore,
    Call, Allocate, Intrinsic, Barrier,
};

/// @brief Unary arithmetic operations.
enum class SSAUnaryOp : uint8_t { Neg, BitNot };

/// @brief Binary arithmetic and bitwise operations.
enum class SSABinaryOp : uint8_t { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr, UShr };

/// @brief Integer/reference comparison predicates used by branch conditions.
enum class SSAComparePredicate : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

/// @brief Conversion family. Numeric changes primitive width; ReferenceCast keeps runtime type info.
enum class SSAConvertKind : uint8_t { Numeric, ReferenceCast };

/**
 * @brief Base class for all block-local SSA instructions.
 *
 * @details
 * Operands are stored in concrete subclasses.  `result` is valid only when
 * `hasResult` is true; otherwise its id remains empty.  The `effect` field is
 * intentionally conservative for JVM operations that may resolve classes,
 * touch heap memory, call out to JNI/runtime helpers, or throw.
 */
struct SSAInstruction {
    SSAInstructionKind kind;
    SSAInstructionResult result;   // result.id.value == 0 表示无 result（void call/store）
    bool hasResult = false;
    SSAEffect effect;

    explicit SSAInstruction(SSAInstructionKind k) : kind(k) {}
    virtual ~SSAInstruction() = default;
};

/// @brief Unary arithmetic such as INEG/LNEG/FNEG/DNEG.
struct SSAUnaryInstruction : SSAInstruction {
    SSAUnaryOp op;
    SSAValue* value = nullptr;
    SSAUnaryInstruction() : SSAInstruction(SSAInstructionKind::Unary), op(SSAUnaryOp::Neg) {}
};

/// @brief Binary arithmetic and bitwise operations such as IADD/LMUL/ISHL/LAND.
struct SSABinaryInstruction : SSAInstruction {
    SSABinaryOp op;
    SSAValue* lhs = nullptr;
    SSAValue* rhs = nullptr;
    SSABinaryInstruction() : SSAInstruction(SSAInstructionKind::Binary), op(SSABinaryOp::Add) {}
};

/// @brief Compare two values and produce an SSA BOOL for branch terminators.
struct SSACompareInstruction : SSAInstruction {
    SSAComparePredicate predicate;
    SSAValue* lhs = nullptr;
    SSAValue* rhs = nullptr;
    SSACompareInstruction() : SSAInstruction(SSAInstructionKind::Compare), predicate(SSAComparePredicate::Eq) {}
};

/**
 * @brief Numeric conversion or runtime reference cast.
 *
 * @details
 * Numeric conversions lower JVM I2L/I2B/D2I-style opcodes.  ReferenceCast lowers
 * CHECKCAST and keeps `referenceTypeDesc` so RegC can emit the required runtime
 * class check instead of seeing only a generic REF result.
 */
struct SSAConvertInstruction : SSAInstruction {
    SSAConvertKind convertKind;
    SSAValue* value = nullptr;
    SSAType targetType = SSAType::UNKNOWN;
    std::string referenceTypeDesc;  // ReferenceCast 目标类/数组描述符，Numeric 留空。
    SSAConvertInstruction() : SSAInstruction(SSAInstructionKind::Convert), convertKind(SSAConvertKind::Numeric) {}
};

/// @brief JVM field reference in owner/name/descriptor form.
struct SSAFieldRef {
    std::string owner;
    std::string name;
    std::string desc;
    bool isStatic = false;
};

/// @brief Read an instance or static field; instance reads carry a receiver value.
struct SSALoadFieldInstruction : SSAInstruction {
    SSAFieldRef field;
    SSAValue* receiver = nullptr;   // null = static
    SSALoadFieldInstruction() : SSAInstruction(SSAInstructionKind::LoadField) {}
};

/// @brief Write an instance or static field; instance writes carry receiver and value.
struct SSAStoreFieldInstruction : SSAInstruction {
    SSAFieldRef field;
    SSAValue* receiver = nullptr;
    SSAValue* value = nullptr;
    SSAStoreFieldInstruction() : SSAInstruction(SSAInstructionKind::StoreField) {}
};

/// @brief Read one array element with bounds/null checks represented by effect.
struct SSAArrayLoadInstruction : SSAInstruction {
    SSAValue* array = nullptr;
    SSAValue* index = nullptr;
    SSAType elementType = SSAType::UNKNOWN;
    SSAArrayLoadInstruction() : SSAInstruction(SSAInstructionKind::ArrayLoad) {}
};

/// @brief Write one array element with bounds/null/store checks represented by effect.
struct SSAArrayStoreInstruction : SSAInstruction {
    SSAValue* array = nullptr;
    SSAValue* index = nullptr;
    SSAValue* value = nullptr;
    SSAType elementType = SSAType::UNKNOWN;
    SSAArrayStoreInstruction() : SSAInstruction(SSAInstructionKind::ArrayStore) {}
};

/// @brief JVM method reference and original invoke opcode.
struct SSAMethodRef {
    std::string owner;
    std::string name;
    std::string desc;
    int invokeOpcode = 0;   // INVOKEVIRTUAL/SPECIAL/STATIC/INTERFACE
};

/**
 * @brief JVM method invocation.
 *
 * @details
 * `args` are stored in source order.  For instance calls, args[0] is the
 * receiver followed by descriptor arguments; for static calls, args contains
 * only descriptor arguments.  This order is deliberately independent of JVM
 * stack pop order to make RegC call emission straightforward.
 */
struct SSACallInstruction : SSAInstruction {
    SSAMethodRef method;
    std::vector<SSAValue*> args;
    SSACallInstruction() : SSAInstruction(SSAInstructionKind::Call) {}
};

/// @brief Allocation form lowered from NEW/NEWARRAY/ANEWARRAY/MULTIANEWARRAY.
enum class SSAAllocationKind : uint8_t { Object, Array, MultiArray };

/**
 * @brief Object or array allocation.
 *
 * @details
 * `typeDesc` is the internal class name for objects and a JVM array descriptor
 * for arrays.  `args` holds array dimensions/counts in source order; object
 * allocation has no args.
 */
struct SSAAllocateInstruction : SSAInstruction {
    SSAAllocationKind allocationKind = SSAAllocationKind::Object;
    std::string typeDesc;            // 类/数组描述符
    std::vector<SSAValue*> args;     // 维度/计数
    SSAAllocateInstruction() : SSAInstruction(SSAInstructionKind::Allocate) {}
};

/// @brief Named JVM intrinsic used when a compact operation is clearer than a new instruction family.
struct SSAIntrinsicInstruction : SSAInstruction {
    std::string name;                // "array.length", "monitor.enter", ...
    std::vector<SSAValue*> args;
    SSAIntrinsicInstruction() : SSAInstruction(SSAInstructionKind::Intrinsic) {}
};

/// @brief Explicit side-effect barrier placeholder for future passes.
struct SSABarrierInstruction : SSAInstruction {
    std::string reason;
    SSABarrierInstruction() : SSAInstruction(SSAInstructionKind::Barrier) {}
};

} // namespace mj2x

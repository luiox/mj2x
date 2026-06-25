// mj2x SSA IR — 块内非控制转移指令（12 类）。
// 详见 doc/IR_DESIGN.md §7。每条至多一个 result（void call/store 无 result）。
#pragma once

#include "ssa_effect.hpp"
#include "ssa_ids.hpp"
#include "ssa_type.hpp"
#include "ssa_value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mj2x {

// ---- 指令类型 tag（RTTI 禁用，用 kind 标签替代 dynamic_cast） ----
enum class SSAInstructionKind : uint8_t {
    Unary, Binary, Compare, Convert,
    LoadField, StoreField,
    ArrayLoad, ArrayStore,
    Call, Allocate, Intrinsic, Barrier,
};

// ---- 算术/位运算 op ----
enum class SSAUnaryOp : uint8_t { Neg, BitNot };
enum class SSABinaryOp : uint8_t { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr, UShr };
enum class SSAComparePredicate : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
enum class SSAConvertKind : uint8_t { Numeric, ReferenceCast };

// 抽象基类。每条指令 operands 在子类的构造里填。kind 标签用于无 RTTI 的类型判定。
struct SSAInstruction {
    SSAInstructionKind kind;
    SSAInstructionResult result;   // result.id.value == 0 表示无 result（void call/store）
    bool hasResult = false;
    SSAEffect effect;

    explicit SSAInstruction(SSAInstructionKind k) : kind(k) {}
    virtual ~SSAInstruction() = default;
};

// 单目算术（INEG / LNEG / FNEG ...）。
struct SSAUnaryInstruction : SSAInstruction {
    SSAUnaryOp op;
    SSAValue* value = nullptr;
    SSAUnaryInstruction() : SSAInstruction(SSAInstructionKind::Unary), op(SSAUnaryOp::Neg) {}
};

// 双目算术（IADD / IMUL / ISHL / LAND ...）。LCMP 三元比较也走这个（R1）。
struct SSABinaryInstruction : SSAInstruction {
    SSABinaryOp op;
    SSAValue* lhs = nullptr;
    SSAValue* rhs = nullptr;
    SSABinaryInstruction() : SSAInstruction(SSAInstructionKind::Binary), op(SSABinaryOp::Add) {}
};

// 比较，产 bool。IF 指令的判定条件。
struct SSACompareInstruction : SSAInstruction {
    SSAComparePredicate predicate;
    SSAValue* lhs = nullptr;
    SSAValue* rhs = nullptr;
    SSACompareInstruction() : SSAInstruction(SSAInstructionKind::Compare), predicate(SSAComparePredicate::Eq) {}
};

// 数值/引用转换（I2L / I2B / CHECKCAST）。
struct SSAConvertInstruction : SSAInstruction {
    SSAConvertKind convertKind;
    SSAValue* value = nullptr;
    SSAType targetType = SSAType::UNKNOWN;
    SSAConvertInstruction() : SSAInstruction(SSAInstructionKind::Convert), convertKind(SSAConvertKind::Numeric) {}
};

// 字段引用描述（owner.name:desc）。
struct SSAFieldRef {
    std::string owner;
    std::string name;
    std::string desc;
    bool isStatic = false;
};

// 读字段。instance 字段带 receiver。
struct SSALoadFieldInstruction : SSAInstruction {
    SSAFieldRef field;
    SSAValue* receiver = nullptr;   // null = static
    SSALoadFieldInstruction() : SSAInstruction(SSAInstructionKind::LoadField) {}
};

// 写字段。
struct SSAStoreFieldInstruction : SSAInstruction {
    SSAFieldRef field;
    SSAValue* receiver = nullptr;
    SSAValue* value = nullptr;
    SSAStoreFieldInstruction() : SSAInstruction(SSAInstructionKind::StoreField) {}
};

// 数组读。
struct SSAArrayLoadInstruction : SSAInstruction {
    SSAValue* array = nullptr;
    SSAValue* index = nullptr;
    SSAType elementType = SSAType::UNKNOWN;
    SSAArrayLoadInstruction() : SSAInstruction(SSAInstructionKind::ArrayLoad) {}
};

// 数组写。
struct SSAArrayStoreInstruction : SSAInstruction {
    SSAValue* array = nullptr;
    SSAValue* index = nullptr;
    SSAValue* value = nullptr;
    SSAType elementType = SSAType::UNKNOWN;
    SSAArrayStoreInstruction() : SSAInstruction(SSAInstructionKind::ArrayStore) {}
};

// 方法调用描述。
struct SSAMethodRef {
    std::string owner;
    std::string name;
    std::string desc;
    int invokeOpcode = 0;   // INVOKEVIRTUAL/SPECIAL/STATIC/INTERFACE
};

// 方法调用。
struct SSACallInstruction : SSAInstruction {
    SSAMethodRef method;
    std::vector<SSAValue*> args;
    SSACallInstruction() : SSAInstruction(SSAInstructionKind::Call) {}
};

// 分配（NEW / NEWARRAY / ANEWARRAY / MULTIANEWARRAY）。
enum class SSAAllocationKind : uint8_t { Object, Array, MultiArray };
struct SSAAllocateInstruction : SSAInstruction {
    SSAAllocationKind allocationKind = SSAAllocationKind::Object;
    std::string typeDesc;            // 类/数组描述符
    std::vector<SSAValue*> args;     // 维度/计数
    SSAAllocateInstruction() : SSAInstruction(SSAInstructionKind::Allocate) {}
};

// intrinsic（ARRAYLENGTH / MONITORENTER / MONITOREXIT / INSTANCEOF）。
struct SSAIntrinsicInstruction : SSAInstruction {
    std::string name;                // "array.length", "monitor.enter", ...
    std::vector<SSAValue*> args;
    SSAIntrinsicInstruction() : SSAInstruction(SSAInstructionKind::Intrinsic) {}
};

// 副作用栅栏（占位，防 DCE 误删）。
struct SSABarrierInstruction : SSAInstruction {
    std::string reason;
    SSABarrierInstruction() : SSAInstruction(SSAInstructionKind::Barrier) {}
};

} // namespace mj2x

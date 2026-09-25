// mj2x SSA IR — type system.
// 对齐 L1 PrimitiveKind（frontend/mir_node.hpp），不重复发明。详见 mj2x/doc_ai/ir_design.md §5。
#pragma once

#include <cstdint>

namespace mj2x {

enum class SSAType : uint8_t
{
    UNKNOWN,
    VOID,
    BOOL,   // JVM boolean（计算位宽与 I32 一致，lowering 区分）
    I8,     // JVM byte
    I16,    // JVM short
    CHAR,   // JVM char
    I32,    // JVM int（含 BOOL/CHAR 的计算位宽）
    I64,    // JVM long  (category 2)
    F32,    // JVM float
    F64,    // JVM double (category 2)
    REF,    // JVM 对象引用（含数组，R1 用 REF 统一，R2 细化）
};

// JVM category-2（long/double）占 2 个栈/局部槽。reaching-def 槽位对齐用（doc_ai/ssa_algorithm.md
// §3.1）。 VOID = 0，其余 category-1 = 1。
inline int ssaStackSize(SSAType t)
{
    switch (t) {
    case SSAType::I64:
    case SSAType::F64: return 2;
    case SSAType::VOID: return 0;
    default: return 1;
    }
}

inline bool ssaIsIntLike(SSAType t)
{
    return t == SSAType::BOOL || t == SSAType::I8 || t == SSAType::I16 || t == SSAType::CHAR ||
           t == SSAType::I32;
}

inline bool ssaIsFloat(SSAType t)
{
    return t == SSAType::F32 || t == SSAType::F64;
}

inline bool ssaIsPrimitiveScalar(SSAType t)
{
    return ssaIsIntLike(t) || t == SSAType::I64 || ssaIsFloat(t);
}

}   // namespace mj2x

/// @file jvm_semantics.hpp
/// @brief 跨后端共享的 JVM 语义决策常量/判定（RegIR 层之下、发射器之上的
///        单一事实来源）。
///
/// @details
/// 背景：同一 JVM 语义曾在 4 个后端各自维护（X2 移位掩码四处状态不一的
/// 结构性根因，详见 doc_ai/cross_backend_semantics.md）。语义决策先以共享
/// 常量/判定的形式收敛到这里，由四后端语义回归集
///（mtest_set g13.shift_out_of_range 等）守门；后续再演进为 RegIR 层可断言的
/// 中间表示。
#pragma once

#include <cstdint>

#include "mj2x/mir/ssa/ssa_type.hpp"

namespace mj2x::codegen {

/// JVM 移位计数按被移位数位宽掩码（JLS 15.19）：int 系 &31、long &63——
/// 超界/负数计数在 JVM 是定义行为；C 裸移位（count >= 位宽）是 UB，
/// LLVM shift（shift >= 位宽）产生 poison，发射端必须显式掩码。
inline constexpr int kJvmShiftMaskI32 = 31;
inline constexpr int kJvmShiftMaskI64 = 63;

/// 移位指令（Shl/Shr/UShr）按 dest 位宽取掩码值。SSA 的移位 dest 只有
/// I32/I64 两种整型（类型系统保证），其余取值属发射端构造错误，按 I32 掩。
inline constexpr int jvmShiftMask(SSAType type)
{
    return type == SSAType::I64 ? kJvmShiftMaskI64 : kJvmShiftMaskI32;
}

}   // namespace mj2x::codegen

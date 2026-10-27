#pragma once

#include "mir_node.hpp"
#include "mj2x/mir/ssa/ssa_type.hpp"


#include <string_view>

namespace mj2x {

PrimitiveKind jvmSortToPrimitive(int sort);
/// 纯分析函数：按 JVM descriptor 首字节判 PrimitiveKind，不持有字符串，
/// 收 string_view 让 Utf8StringRef/std::string 调用点都能零适配传入。
PrimitiveKind jvmDescToPrimitive(std::string_view desc);
int           primitiveStackSize(PrimitiveKind kind);

/// @brief PrimitiveKind → SSAType（SSA 参数/返回类型的唯一映射）。
///
/// @details
/// 原先是 `ssa_builder.cpp` 的文件内静态函数，但 RegC 后端也需要它——方法间
/// 直调的前向声明必须按**描述符**算出被调方签名（不能用调用点操作数类型推，
/// 窄类型提升会让同一函数产出两份不同声明）。两端必须同一张表，故提到这里。
inline SSAType mirKindToSSA(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return SSAType::BOOL;
    case PrimitiveKind::BYTE: return SSAType::I8;
    case PrimitiveKind::CHAR: return SSAType::CHAR;
    case PrimitiveKind::SHORT: return SSAType::I16;
    case PrimitiveKind::INT: return SSAType::I32;
    case PrimitiveKind::LONG: return SSAType::I64;
    case PrimitiveKind::FLOAT: return SSAType::F32;
    case PrimitiveKind::DOUBLE: return SSAType::F64;
    case PrimitiveKind::VOID: return SSAType::VOID;
    case PrimitiveKind::OBJECT: return SSAType::REF;
    default: return SSAType::UNKNOWN;
    }
}

/// @brief 描述符单槽 → SSAType（'Z'..'D' / 'L' / '['，取首字符起的完整槽）。
inline SSAType descriptorSlotToSSA(std::string_view slot)
{
    return mirKindToSSA(jvmDescToPrimitive(slot));
}

}   // namespace mj2x

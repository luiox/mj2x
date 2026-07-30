#pragma once

#include "mir_node.hpp"

#include <jbytecode/jbytecode.hpp>

#include <string_view>

namespace mj2x {

PrimitiveKind jvmSortToPrimitive(int sort);
/// 纯分析函数：按 JVM descriptor 首字节判 PrimitiveKind，不持有字符串，
/// 收 string_view 让 Utf8StringRef/std::string 调用点都能零适配传入。
PrimitiveKind jvmDescToPrimitive(std::string_view desc);
int           primitiveStackSize(PrimitiveKind kind);

}   // namespace mj2x

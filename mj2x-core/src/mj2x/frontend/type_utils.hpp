#pragma once

#include "mir_node.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

PrimitiveKind jvmSortToPrimitive(int sort);
PrimitiveKind jvmDescToPrimitive(const std::string& desc);
int primitiveStackSize(PrimitiveKind kind);

} // namespace mj2x


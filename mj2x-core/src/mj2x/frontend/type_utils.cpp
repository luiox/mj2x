#include "type_utils.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

PrimitiveKind jvmSortToPrimitive(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return PrimitiveKind::BOOLEAN;
    case Type::BYTE: return PrimitiveKind::BYTE;
    case Type::CHAR: return PrimitiveKind::CHAR;
    case Type::SHORT: return PrimitiveKind::SHORT;
    case Type::INT: return PrimitiveKind::INT;
    case Type::FLOAT: return PrimitiveKind::FLOAT;
    case Type::LONG: return PrimitiveKind::LONG;
    case Type::DOUBLE: return PrimitiveKind::DOUBLE;
    case Type::VOID: return PrimitiveKind::VOID;
    default: return PrimitiveKind::OBJECT;
    }
}

PrimitiveKind jvmDescToPrimitive(std::string_view desc)
{
    // JVM descriptor 首字节是 ASCII 类型字符，按字节读安全且 O(1)。
    if (desc.empty()) return PrimitiveKind::VOID;
    switch (desc[0]) {
    case 'Z': return PrimitiveKind::BOOLEAN;
    case 'B': return PrimitiveKind::BYTE;
    case 'C': return PrimitiveKind::CHAR;
    case 'S': return PrimitiveKind::SHORT;
    case 'I': return PrimitiveKind::INT;
    case 'J': return PrimitiveKind::LONG;
    case 'F': return PrimitiveKind::FLOAT;
    case 'D': return PrimitiveKind::DOUBLE;
    case 'V': return PrimitiveKind::VOID;
    default: return PrimitiveKind::OBJECT;
    }
}

int primitiveStackSize(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::LONG:
    case PrimitiveKind::DOUBLE: return 2;
    default: return 1;
    }
}

}   // namespace mj2x

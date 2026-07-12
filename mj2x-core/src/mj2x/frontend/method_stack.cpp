#include "method_stack.hpp"
#include "type_utils.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

MethodStack::MethodStack(MethodNode& mn)
    : maxStack_(mn.maxStack)
    , maxLocals_(mn.maxLocals)
{
    auto desc     = Type::getMethodType(mn.desc);
    auto argTypes = desc.getArgumentTypes();
    int  slot     = 0;

    if (!(mn.access & Opcodes::ACC_STATIC)) localTypes_[slot++] = PrimitiveKind::OBJECT;   // this

    for (const auto& argType : argTypes) {
        auto kind = PrimitiveKind::INT;
        switch (argType.getSort()) {
        case Type::BOOLEAN: kind = PrimitiveKind::BOOLEAN; break;
        case Type::BYTE: kind = PrimitiveKind::BYTE; break;
        case Type::CHAR: kind = PrimitiveKind::CHAR; break;
        case Type::SHORT: kind = PrimitiveKind::SHORT; break;
        case Type::INT: kind = PrimitiveKind::INT; break;
        case Type::LONG: kind = PrimitiveKind::LONG; break;
        case Type::FLOAT: kind = PrimitiveKind::FLOAT; break;
        case Type::DOUBLE: kind = PrimitiveKind::DOUBLE; break;
        default: kind = PrimitiveKind::OBJECT; break;
        }
        localTypes_[slot] = kind;
        slot += primitiveStackSize(kind);
    }
}

PrimitiveKind MethodStack::getLocalType(int index) const
{
    auto it = localTypes_.find(index);
    return it != localTypes_.end() ? it->second : PrimitiveKind::INT;
}

}   // namespace mj2x

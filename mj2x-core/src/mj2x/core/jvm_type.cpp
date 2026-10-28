#include "jvm_type.hpp"

#include <jbc/type/descriptor.hpp>

#include <stdexcept>

namespace mj2x {

namespace {

/// @brief jbc::type 结构校验入口：Err 即抛（J-6 根治点——空/截断描述符
/// 确定性 invalid_argument，不再依赖越界读的漂移行为）。
void validateMethodDescriptor(const ca::str::Utf8StringRef& desc)
{
    std::string_view view{reinterpret_cast<const char*>(desc.data()), desc.byte_length()};
    auto parsed = jbc::type::parseMethodDescriptor(view);
    if (parsed.is_err()) {
        std::string msg = "Invalid method descriptor: ";
        msg.append(view);
        msg.append(" (");
        msg.append(jbc::type::to_string(std::move(parsed).unwrap_err()));
        msg.append(")");
        throw std::invalid_argument(msg);
    }
}

/// @brief 单类型描述符结构校验（V 为返回位专用，不在此面）。
void validateFieldType(const ca::str::Utf8StringRef& desc)
{
    std::string_view view{reinterpret_cast<const char*>(desc.data()), desc.byte_length()};
    auto parsed = jbc::type::parseFieldType(view);
    if (parsed.is_err()) {
        std::string msg = "Invalid descriptor: ";
        msg.append(view);
        msg.append(" (");
        msg.append(jbc::type::to_string(std::move(parsed).unwrap_err()));
        msg.append(")");
        throw std::invalid_argument(msg);
    }
}

}   // namespace

Type Type::getType(const ca::str::Utf8StringRef& typeDescriptor)
{
    if (typeDescriptor.is_empty()) {
        throw std::invalid_argument("Empty descriptor");
    }
    ca::u8 c = typeDescriptor.byte_at(0);
    if (c == '[' || c == 'L') {
        validateFieldType(typeDescriptor);
    }
    else if (c == '(') {
        validateMethodDescriptor(typeDescriptor);
    }
    return slice(typeDescriptor, 0, typeDescriptor.byte_length());
}

Type Type::getObjectType(const ca::str::Utf8StringRef& internalName)
{
    if (internalName.is_empty()) {
        throw std::invalid_argument("empty internal name");
    }
    if (internalName.byte_at(0) == '[') {
        return Type(ARRAY, internalName);
    }
    return Type(INTERNAL, internalName);
}

Type Type::getMethodType(const ca::str::Utf8StringRef& methodDescriptor)
{
    return Type(METHOD, methodDescriptor);
}

std::vector<Type> Type::getArgumentTypes(const ca::str::Utf8StringRef& methodDescriptor)
{
    validateMethodDescriptor(methodDescriptor);
    std::vector<Type> args;
    size_t            cur = 1;
    while (methodDescriptor.byte_at(cur) != ')') {
        size_t start = cur;
        while (methodDescriptor.byte_at(cur) == '[') ++cur;
        if (methodDescriptor.byte_at(cur) == 'L') {
            size_t end = cur;
            while (end < methodDescriptor.byte_length() && methodDescriptor.byte_at(end) != ';')
                ++end;
            cur = end + 1;
        }
        else {
            ++cur;
        }
        args.push_back(slice(methodDescriptor, start, cur));
    }
    return args;
}

Type Type::getReturnType(const ca::str::Utf8StringRef& methodDescriptor)
{
    validateMethodDescriptor(methodDescriptor);
    size_t returnOffset = returnTypeOffset(methodDescriptor);
    return slice(methodDescriptor, returnOffset, methodDescriptor.byte_length());
}

int Type::getSort() const
{
    return (sort_ == INTERNAL) ? OBJECT : sort_;
}

ca::str::Utf8StringRef Type::getDescriptor(ca::str::Utf8StringArena& arena) const
{
    if (sort_ == OBJECT || sort_ == INTERNAL) {
        ca::str::Utf8StringBuilder desc;
        desc.append("L").append(value_).append(";");
        return arena.intern(desc.build().ref());
    }
    return value_;
}

ca::str::Utf8StringRef Type::getInternalName(ca::str::Utf8StringArena& arena) const
{
    if (sort_ == OBJECT || sort_ == INTERNAL) {
        return value_;
    }
    if (sort_ == ARRAY) {
        return arena.intern(value_);
    }
    throw std::logic_error("getInternalName() called on non-reference type");
}

std::vector<Type> Type::getArgumentTypes() const
{
    if (sort_ != METHOD) {
        throw std::logic_error("getArgumentTypes() called on non-method type");
    }
    return getArgumentTypes(value_);
}

Type Type::getReturnType() const
{
    if (sort_ != METHOD) {
        throw std::logic_error("getReturnType() called on non-method type");
    }
    return getReturnType(value_);
}

std::string Type::toString() const
{
    ca::str::Utf8StringBuilder buf;
    if (sort_ == OBJECT || sort_ == INTERNAL) {
        buf.append("L").append(value_).append(";");
    }
    else {
        buf.append(value_);
    }
    auto d = buf.build();
    return std::string(reinterpret_cast<const char*>(d.data()), d.byte_length());
}

bool Type::operator==(const Type& other) const
{
    if (this == &other) return true;
    int s1 = (sort_ == INTERNAL) ? OBJECT : sort_;
    int s2 = (other.sort_ == INTERNAL) ? OBJECT : other.sort_;
    if (s1 != s2) return false;
    return value_.equals(other.value_);
}

bool Type::operator!=(const Type& other) const
{
    return !(*this == other);
}

Type Type::slice(const ca::str::Utf8StringRef& desc, size_t begin, size_t end)
{
    if (begin >= end) throw std::invalid_argument("Empty descriptor");
    ca::u8 c = desc.byte_at(begin);
    switch (c) {
    case 'V': return Type(VOID, desc.slice(begin, begin + 1));
    case 'Z': return Type(BOOLEAN, desc.slice(begin, begin + 1));
    case 'C': return Type(CHAR, desc.slice(begin, begin + 1));
    case 'B': return Type(BYTE, desc.slice(begin, begin + 1));
    case 'S': return Type(SHORT, desc.slice(begin, begin + 1));
    case 'I': return Type(INT, desc.slice(begin, begin + 1));
    case 'F': return Type(FLOAT, desc.slice(begin, begin + 1));
    case 'J': return Type(LONG, desc.slice(begin, begin + 1));
    case 'D': return Type(DOUBLE, desc.slice(begin, begin + 1));
    case '[': return Type(ARRAY, desc.slice(begin, end));
    case 'L': return Type(OBJECT, desc.slice(begin + 1, end - 1));
    case '(': return Type(METHOD, desc.slice(begin, end));
    default:
    {
        std::string msg = "Invalid descriptor: ";
        msg.append(reinterpret_cast<const char*>(desc.data()), desc.byte_length());
        throw std::invalid_argument(msg);
    }
    }
}

size_t Type::returnTypeOffset(const ca::str::Utf8StringRef& methodDescriptor)
{
    size_t cur = 1;
    while (methodDescriptor.byte_at(cur) != ')') {
        while (methodDescriptor.byte_at(cur) == '[') ++cur;
        if (methodDescriptor.byte_at(cur) == 'L') {
            size_t e = cur;
            while (e < methodDescriptor.byte_length() && methodDescriptor.byte_at(e) != ';')
                ++e;
            if (e >= methodDescriptor.byte_length()) break;
            cur = e + 1;
        }
        else {
            ++cur;
        }
    }
    return cur + 1;
}

}   // namespace mj2x

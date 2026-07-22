#include "ssa_function.hpp"

#include <cstdint>
#include <cstring>

namespace mj2x {

namespace {

bool sameFloatingBits(double lhs, double rhs, SSAType type)
{
    if (type == SSAType::F32) {
        const float lhsFloat = static_cast<float>(lhs);
        const float rhsFloat = static_cast<float>(rhs);
        uint32_t    lhsBits   = 0;
        uint32_t    rhsBits   = 0;
        std::memcpy(&lhsBits, &lhsFloat, sizeof(lhsBits));
        std::memcpy(&rhsBits, &rhsFloat, sizeof(rhsBits));
        return lhsBits == rhsBits;
    }

    uint64_t lhsBits = 0;
    uint64_t rhsBits = 0;
    std::memcpy(&lhsBits, &lhs, sizeof(lhsBits));
    std::memcpy(&rhsBits, &rhs, sizeof(rhsBits));
    return lhsBits == rhsBits;
}

}   // namespace

// 整型 literal 去重：相同 value+type 复用同一对象。R1 简单线性查找（literal 数量小）。
SSAIntLiteral* SSAFunction::internInt(int64_t v, SSAType t)
{
    for (auto& lit : intLiterals) {
        if (lit->value == v && lit->type == t) return lit.get();
    }
    auto  lit = std::make_unique<SSAIntLiteral>(v, t);
    auto* ptr = lit.get();
    intLiterals.push_back(std::move(lit));
    return ptr;
}

SSAFloatLiteral* SSAFunction::internFloat(double v, SSAType t)
{
    for (auto& lit : floatLiterals) {
        if (lit->type == t && sameFloatingBits(lit->value, v, t)) return lit.get();
    }
    auto  lit = std::make_unique<SSAFloatLiteral>(v, t);
    auto* ptr = lit.get();
    floatLiterals.push_back(std::move(lit));
    return ptr;
}

SSANullLiteral* SSAFunction::internNull()
{
    if (!nullLiteral) nullLiteral = std::make_unique<SSANullLiteral>();
    return nullLiteral.get();
}

SSAOpaqueValue* SSAFunction::newOpaque(SSAType t, std::string tag)
{
    // opaque 不去重（每个都是独立 value，占正式 id）。
    auto  v   = std::make_unique<SSAOpaqueValue>(ids.newValueId(), t, std::move(tag));
    auto* ptr = v.get();
    opaqueValues.push_back(std::move(v));
    return ptr;
}

}   // namespace mj2x

#include "ssa_function.hpp"

namespace mj2x {

// 整型 literal 去重：相同 value+type 复用同一对象。R1 简单线性查找（literal 数量小）。
SSAIntLiteral* SSAFunction::internInt(int64_t v, SSAType t) {
    for (auto& lit : intLiterals) {
        if (lit->value == v && lit->type == t) return lit.get();
    }
    auto lit = std::make_unique<SSAIntLiteral>(v, t);
    auto* ptr = lit.get();
    intLiterals.push_back(std::move(lit));
    return ptr;
}

SSAFloatLiteral* SSAFunction::internFloat(double v, SSAType t) {
    for (auto& lit : floatLiterals) {
        if (lit->value == v && lit->type == t) return lit.get();
    }
    auto lit = std::make_unique<SSAFloatLiteral>(v, t);
    auto* ptr = lit.get();
    floatLiterals.push_back(std::move(lit));
    return ptr;
}

SSANullLiteral* SSAFunction::internNull() {
    if (!nullLiteral) nullLiteral = std::make_unique<SSANullLiteral>();
    return nullLiteral.get();
}

SSAOpaqueValue* SSAFunction::newOpaque(SSAType t, std::string tag) {
    // opaque 不去重（每个都是独立 value，占正式 id）。
    auto v = std::make_unique<SSAOpaqueValue>(ids.newValueId(), t, std::move(tag));
    auto* ptr = v.get();
    opaqueValues.push_back(std::move(v));
    return ptr;
}

} // namespace mj2x

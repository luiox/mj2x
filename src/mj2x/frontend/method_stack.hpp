#pragma once

#include "mir_node.hpp"

#include <jbytecode/jbytecode.hpp>

#include <unordered_map>

namespace mj2x {

class MethodStack {
public:
    explicit MethodStack(jbytecode::MethodNode& mn);

    int maxStack() const { return maxStack_; }
    int maxLocals() const { return maxLocals_; }
    PrimitiveKind getLocalType(int index) const;

private:
    int maxStack_ = 0;
    int maxLocals_ = 0;
    std::unordered_map<int, PrimitiveKind> localTypes_;
};

} // namespace mj2x

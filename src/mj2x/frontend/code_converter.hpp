#pragma once

#include "block.hpp"
#include "mir_node.hpp"
#include "method_stack.hpp"
#include "mj2x/core/compiler_index.hpp"

#include <jbytecode/jbytecode.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

class CodeConverter {
public:
    CodeConverter();
    ~CodeConverter();

    std::vector<Block*> convert(jbytecode::MethodNode& mn);

    static std::string dumpMIR(const std::vector<Block*>& blocks);

private:
    void analyzeControlFlow(jbytecode::MethodNode& mn);
    void buildBlocks();
    void translateInstructions(jbytecode::MethodNode& mn);
    void processInsn(jbytecode::AbstractInsnNode* insn);
    Block* getOrCreateBlock(int labelId);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mj2x

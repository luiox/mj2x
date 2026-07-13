/**
 * @file code_converter.hpp
 * @brief 前端：JVM 字节码 -> L1 MIR（栈式中间表示）。
 *
 * @details
 * CodeConverter 是前端入口，把 MethodNode 的 tree 指令序列切成基本块并提升为
 * L1 MIR（见 mir_node.hpp 的 MIRKind）。L1 MIR 保留 JVM 操作族和控制流，栈操作
 * （DUP/SWAP/POP）也保留为节点——SSA builder 需要 category-1/2 规则来消除栈。
 *
 * 输出的 Block 拥有 MIRNode 所有权，由调用方（Pipeline/Backend）消费后释放。
 */
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

/// 把 MethodNode 字节码转换为 L1 MIR 基本块序列（前端）。
class CodeConverter
{
public:
    CodeConverter();
    ~CodeConverter();

    /**
     * @brief 转换一个方法的字节码为 L1 MIR blocks。
     * @return Block* 指针由 converter 内部拥有，调用方只读消费。
     */
    std::vector<Block*> convert(jbytecode::tree::MethodNode& mn);

    /// dump L1 MIR 为可读文本（调试/dumpMIR 用）。
    static std::string dumpMIR(const std::vector<Block*>& blocks);

private:
    void   analyzeControlFlow(jbytecode::tree::MethodNode& mn);
    void   buildBlocks();
    void   translateInstructions(jbytecode::tree::MethodNode& mn);
    void   processInsn(jbytecode::tree::AbstractInsnNode* insn);
    Block* getOrCreateBlock(int labelId);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace mj2x

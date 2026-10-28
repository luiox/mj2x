/// @file code_converter.hpp
/// @brief 前端：JVM 字节码 -> L1 MIR（栈式中间表示）。
///
/// @details
/// CodeConverter 是前端入口，把 MethodNode 的 tree 指令序列切成基本块并提升为
/// L1 MIR（见 mir_node.hpp 的 MIRKind）。L1 MIR 保留 JVM 操作族和控制流，栈操作
/// （DUP/SWAP/POP）也保留为节点——SSA builder 需要 category-1/2 规则来消除栈。
///
/// 输出的 Block 拥有 MIRNode 所有权，由调用方（Pipeline/Backend）消费后释放。
#pragma once

#include "block.hpp"
#include "mir_node.hpp"
#include "mj2x/core/compiler_index.hpp"

#include <jbc/core/attribute.hpp>
#include <jbc/core/class_file.hpp>
#include <jbc/core/insn.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

/// 把方法字节码转换为 L1 MIR 基本块序列（前端）。
///
/// 批 3b 起主入口走 libjbc DOM（批 2 的 tree→DOM 阶段边界同步产物）：
/// decodeInsns 逐指令投影 + CP 解析 + 按 pc 切块。MethodNode 重载是测试垫片
/// （树经 plain 序列化 + readClass 字节往返到 DOM 后走同一读路径），批 6 摘树
/// 时随树一并退役。
class CodeConverter
{
public:
    /// @param arena  MIR 节点字符串字段（owner/name/desc/typeDesc 等）入池用，
    /// 调用方注入寿命（str-ownership-spec §6.3）。必须存活到 convert() 产出的
    /// Block 消费完毕（CompilerIndex 的 job 级 arena 满足此要求）。
    explicit CodeConverter(ca::str::Utf8StringArena& arena);
    ~CodeConverter();

    /// @brief 转换 DOM 中第 methodIndex 个方法的字节码为 L1 MIR blocks。
    /// @param cf  批 2 同步产出的类 DOM；decodeCode 的 lazy 缓存落在其上。
    /// @return Block* 指针由 converter 内部拥有，调用方只读消费。
    std::vector<Block*> convert(jbc::core::ClassFile& cf, ca::usize methodIndex);

    /// @brief 测试垫片：MethodNode → plain 字节 → DOM → 同一读路径。

    /// dump L1 MIR 为可读文本（调试/dumpMIR 用）。
    static std::string dumpMIR(const std::vector<Block*>& blocks);

private:
    void   analyzeControlFlow(const std::vector<jbc::core::RawInsn>& insns,
                              const jbc::core::CodeAttr&             code);
    void   buildBlocks();
    void   translateInstructions(const std::vector<jbc::core::RawInsn>& insns);
    void   processInsn(const jbc::core::RawInsn& insn);
    Block* getOrCreateBlock(int labelId);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace mj2x

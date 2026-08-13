#pragma once

#include "mir_node.hpp"

#include <memory>
#include <vector>

namespace mj2x {

/// @brief Method exception-table entry normalized to L1 label ids.
///
/// @details
/// The protected range is `[startLabel, endLabel)`, matching JVM exception
/// table semantics.  Labels are CodeConverter's pointer-derived label keys, not
/// bytecode offsets, because libjbytecode tree reconstruction may reset offsets.
/// An empty `catchType` represents a catch-all/finally handler.
///
/// `startInsnIdx`/`endInsnIdx` are the instruction-list indices of the
/// start/end labels (filled by CodeConverter::translateInstructions).  They are
/// the authoritative protected-range representation for block-granularity
/// decisions (SSABuilder::blockInProtectedRange): a block is protected iff its
/// instruction range intersects `[startInsnIdx, endInsnIdx)`.  This decouples
/// protection from label-to-block splitting, so the range stays exact even if
/// the L1 splitter later merges or reorders blocks.  -1 means not yet resolved
/// (defensive fallback keeps the legacy label-scan semantics).
struct ExceptionHandler
{
    int                    startLabel   = 0;   ///< Inclusive protected-range start label key.
    int                    endLabel     = 0;   ///< Exclusive protected-range end label key.
    int                    handlerLabel = 0;   ///< Handler entry label key.
    int                    startInsnIdx = -1;  ///< Instruction-list index of start label.
    int                    endInsnIdx   = -1;  ///< Instruction-list index of end label.
    ca::str::Utf8StringRef catchType;          ///< JVM internal class name; empty means catch-all.
};

/// @brief L1 basic block produced by CodeConverter.
struct Block
{
    int                                   id = 0;
    std::vector<std::unique_ptr<MIRNode>> nodes;
    std::vector<Block*>                   successors;
    std::vector<Block*>                   predecessors;
    ExceptionHandler*                     exceptionHandler = nullptr;
    /// @brief Method-level exception table copied from MethodNode.
    ///
    /// @details
    /// CodeConverter returns only L1 blocks to downstream SSA/RegC layers.  The
    /// exception table is attached by value to the first block so later layers
    /// can model handler entry and protected ranges without re-reading bytecode
    /// tree nodes.
    std::vector<ExceptionHandler> exceptionTable;

    /// @brief 块内指令的 instruction-list 序号范围（CodeConverter 填充）。
    ///
    /// @details
    /// firstInsnIdx 是块首指令（含 LABEL 等伪指令）的序号，lastInsnIdx 是块尾
    /// 指令序号。与 ExceptionHandler::startInsnIdx/endInsnIdx 同尺（同一指令
    /// 列表的下标），SSABuilder 用「块范围 ∩ 保护区」判定块是否受保护，取代
    /// 依赖块序的 label 区间扫描。空块（无节点）保持 -1。
    int firstInsnIdx = -1;
    int lastInsnIdx  = -1;

    explicit Block(int id_)
        : id(id_)
    {}
    Block(const Block&)            = delete;
    Block& operator=(const Block&) = delete;
};

}   // namespace mj2x

#pragma once

#include "mir_node.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mj2x {

/// @brief Method exception-table entry normalized to L1 label ids.
///
/// @details
/// The protected range is `[startLabel, endLabel)`, matching JVM exception
/// table semantics.  Labels are CodeConverter's pointer-derived label keys, not
/// bytecode offsets, because libjbytecode tree reconstruction may reset offsets.
/// An empty `catchType` represents a catch-all/finally handler.
struct ExceptionHandler
{
    int         startLabel   = 0;   ///< Inclusive protected-range start label key.
    int         endLabel     = 0;   ///< Exclusive protected-range end label key.
    int         handlerLabel = 0;   ///< Handler entry label key.
    std::string catchType;          ///< JVM internal class name; empty means catch-all.
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

    explicit Block(int id_)
        : id(id_)
    {}
    Block(const Block&)            = delete;
    Block& operator=(const Block&) = delete;
};

}   // namespace mj2x

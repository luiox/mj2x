// mj2x SSA IR — function 容器。
// 详见 doc/IR_DESIGN.md §2。owns 所有块、参数、value（块内指令 owns 其 result 的生命周期，
// 但 Parameter/BlockArg/Literal 由 function 持有）。
#pragma once

#include "ssa_block.hpp"
#include "ssa_ids.hpp"
#include "ssa_value.hpp"

#include "jbytecode/core/primitives.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace mj2x {

/// @brief JVM method identity and signature carried by SSA and later RegC.
struct SSAFunctionSymbol
{
    std::string          owner;   ///< JVM internal owner name.
    std::string          name;
    std::string          desc;
    std::vector<SSAType> parameterTypes;
    SSAType              returnType = SSAType::UNKNOWN;
};

/// @brief One exceptional CFG edge carrying handler local values.
///
/// @details
/// The JVM exception table enters a handler without an ordinary terminator
/// edge. The handler still needs the local-slot values that were live at the
/// protected block, so the transfer is represented explicitly for lowering.
struct SSAExceptionEdge
{
    SSABlock*             protectedBlock = nullptr;
    std::vector<SSAValue*> handlerArgs;
};

/// @brief One SSA-level JVM exception-table region.
///
/// @details
/// `protectedBlocks` preserves the L1 block range covered by the original
/// exception table entry.  `handler` is the block entered when RegC catches a
/// matching pending exception.  `exceptionValue` is a synthetic REF block
/// argument that represents the JVM handler-entry operand stack item; it uses
/// `kSSAExceptionObjectSlot` rather than a real local slot.  `caughtType` is the
/// JVM internal class name from the table, or empty for catch-all/finally.
struct SSAExceptionRegion
{
    std::vector<SSABlock*> protectedBlocks;            ///< Blocks covered by this region.
    std::vector<SSAExceptionEdge> edges;               ///< Local-value transfers per block.
    SSABlock*              handler        = nullptr;   ///< Handler entry block.
    SSABlockArg*           exceptionValue = nullptr;   ///< Synthetic caught-exception arg.
    std::string            caughtType;                 ///< Empty means catch-all/finally.
};

/// @brief Full block-argument SSA function owned by one method conversion.
struct SSAFunction
{
    SSAFunctionSymbol                          symbol;
    std::vector<std::unique_ptr<SSAParameter>> parameters;        // owns
    std::vector<std::unique_ptr<SSABlock>>     blocks;            // owns blocks
    SSABlock*                                  entry = nullptr;   // 合成入口块
    std::vector<SSAExceptionRegion>            exceptionRegions;

    /// @brief invokedynamic 站点元数据（自定义 bootstrap，阶段3）。
    ///
    /// @details
    /// SSA builder 对无法静态展开的 invokedynamic（非 LambdaMetafactory /
    /// StringConcatFactory，见 InvokeDynamicPreprocessor）生成
    /// Intrinsic("indy:<index>") 指令并把本站点压入此表；index 为表内下标。
    /// 值类型拷贝（std::string / Any 值语义），不依赖 SSA arena 生命周期。
    struct InvokeDynamicSite
    {
        std::string                          name, desc;
        std::optional<jbytecode::core::Handle> bsm;
        std::vector<ca::core::Any>           bsmArgs;
    };
    std::vector<InvokeDynamicSite> indySites;

    SSAIds ids;   // id 发放器（function 内全局）

    // owns 额外的 value（literal/opaque）。指令 result 由指令自己 owns（通过
    // unique_ptr<SSAInstruction>）， 但 result 作为 SSAValue 被引用时，其地址稳定（指令存在
    // blocks[*].instructions 里，不移动）。 literal 不占正式
    // id（去重/内联），用裸指针持有，function 析构时释放。
    std::vector<std::unique_ptr<SSAIntLiteral>>   intLiterals;
    std::vector<std::unique_ptr<SSAFloatLiteral>> floatLiterals;
    std::unique_ptr<SSANullLiteral>               nullLiteral;
    std::vector<std::unique_ptr<SSAOpaqueValue>>  opaqueValues;

    // literal 工厂（简单去重：相同 value+type 复用）。
    SSAIntLiteral*   internInt(int64_t v, SSAType t);
    SSAFloatLiteral* internFloat(double v, SSAType t);
    SSANullLiteral*  internNull();
    SSAOpaqueValue*  newOpaque(SSAType t, std::string tag);
};

}   // namespace mj2x

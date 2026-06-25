// mj2x SSA IR — function 容器。
// 详见 doc/IR_DESIGN.md §2。owns 所有块、参数、value（块内指令 owns 其 result 的生命周期，
// 但 Parameter/BlockArg/Literal 由 function 持有）。
#pragma once

#include "ssa_block.hpp"
#include "ssa_ids.hpp"
#include "ssa_value.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace mj2x {

struct SSAFunctionSymbol {
    std::string owner;       // 内部类名
    std::string name;
    std::string desc;
    std::vector<SSAType> parameterTypes;
    SSAType returnType = SSAType::UNKNOWN;
};

// 异常保护区。R1 留空（带 try-catch 的方法 fail-closed）。R2 填充。
struct SSAExceptionRegion {
    std::vector<SSABlock*> protectedBlocks;
    SSABlock* handler = nullptr;
    std::string caughtType;   // 空 = catch all / finally
};

struct SSAFunction {
    SSAFunctionSymbol symbol;
    std::vector<std::unique_ptr<SSAParameter>> parameters;   // owns
    std::vector<std::unique_ptr<SSABlock>> blocks;           // owns blocks
    SSABlock* entry = nullptr;                               // 合成入口块
    std::vector<SSAExceptionRegion> exceptionRegions;

    SSAIds ids;   // id 发放器（function 内全局）

    // owns 额外的 value（literal/opaque）。指令 result 由指令自己 owns（通过 unique_ptr<SSAInstruction>），
    // 但 result 作为 SSAValue 被引用时，其地址稳定（指令存在 blocks[*].instructions 里，不移动）。
    // literal 不占正式 id（去重/内联），用裸指针持有，function 析构时释放。
    std::vector<std::unique_ptr<SSAIntLiteral>> intLiterals;
    std::vector<std::unique_ptr<SSAFloatLiteral>> floatLiterals;
    std::unique_ptr<SSANullLiteral> nullLiteral;
    std::vector<std::unique_ptr<SSAOpaqueValue>> opaqueValues;

    // literal 工厂（简单去重：相同 value+type 复用）。
    SSAIntLiteral* internInt(int64_t v, SSAType t);
    SSAFloatLiteral* internFloat(double v, SSAType t);
    SSANullLiteral* internNull();
    SSAOpaqueValue* newOpaque(SSAType t, std::string tag);
};

} // namespace mj2x

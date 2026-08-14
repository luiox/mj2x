/// @file vm_encoder.hpp
/// @brief VmEncoder：把 RegFunction 编码成 VmProgram（opcode 字节流 + 常量池）。
///
/// @details
/// VmEncoder 是 VMC4 的核心编码器，位于 RegIR 之后的分叉点：
///   RegFunction → VmEncoder::encode() → VmProgram → 解释器执行
///
/// ### 寄存器映射策略
///
/// RegIR 的虚拟寄存器（RegId）统一映射到 VM 寄存器索引（u16）。映射是**恒等映射**：
///   - RegId.value 直接作为 VM 寄存器索引（MVP 不做 per-instance 重映射，Phase 2 起
///     VmInstance.opcodeTable 会引入寄存器编号随机化）。
///   - 字面量（RegOperandKind::IntLiteral/FloatLiteral）收集到 constantPool，编码前
///     预扫描全部计算寄存器得到高水位，常量槽分配在其之后（与计算寄存器空间隔离，
///     避免 SSA value id 密集递增导致的重叠），prologue 预加载。
///
/// 寄存器文件布局（见 vm_opcode.hpp）：
///   [0 .. numInputs)         : 方法参数
///   [numInputs .. maxCompute): 计算临时寄存器（= RegId.value 范围）
///   [maxCompute .. numRegs)  : 常量槽（prologue 从 constantPool 加载）
///
/// ### 两遍编码
///
/// 因为 Jump/Branch/Return 的目标 block 索引需要解析成 bytecode offset，而 offset
/// 只有在编码完前驱 block 后才知道，所以采用两遍：
///   - 第一遍：计算每个 block 的起始 offset，记录到 blockTargets
///   - 第二遍：编码每个 block 的指令 + terminator
///
/// ### fail-closed
///
/// 遇到 MVP 不支持的 family（LoadField/Call/Allocate 等）时返回 nullptr 并填 reason，
/// Vmc4Backend 据此保留字节码不转 VM。这符合 mj2x 的 fail-closed 哲学
/// （见 doc_ai/vmc4_multi_vm_design.md §7）。
#pragma once

#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/core/user_intrinsics.hpp"

#include <memory>
#include <string>

namespace mj2x {

struct RegFunction;
class VmInstance;

/// @brief RegIR → VM bytecode 编码器。
class VmEncoder
{
public:
    /// @brief 编码一个 RegFunction 为 VmProgram。
    ///
    /// @param fn 已经过 DCE + transform 的 RegIR 函数。
    /// @param instance VM 实例（MVP 只影响命名，Phase 2 影响寄存器/opcode 映射）。
    /// @param reason 失败时填充原因（fail-closed 日志）。
    /// @param userIntrinsics 用户自定义 intrinsic（issue #818）；nullptr 时不匹配。
    /// @return 成功返回 VmProgram；失败返回 nullptr（含 MVP 不支持的 family）。
    static std::unique_ptr<VmProgram> encode(const RegFunction& fn, const VmInstance& instance,
                                             std::string& reason,
                                             const std::vector<UserIntrinsic>* userIntrinsics =
                                                 nullptr);
};

}   // namespace mj2x

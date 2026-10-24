/// @file vm_encoder.hpp
/// @brief VmEncoder：把 RegFunction 编码成 VmProgram（opcode 字节流 + 常量池）。
///
/// @details
/// VmEncoder 是 VMC4 的核心编码器，位于 RegIR 之后的分叉点：
///   RegFunction → VmEncoder::encode() → VmProgram → 解释器执行
///
/// ### 寄存器映射策略
///
/// RegIR 的虚拟寄存器（RegId）统一映射到 VM 寄存器索引（u16）。默认**恒等映射**：
///   - RegId.value 直接作为 VM 寄存器索引（MVP 不做 per-instance 重映射，Phase 2 起
///     VmInstance.opcodeTable 会引入寄存器编号随机化）。
///   - 字面量（RegOperandKind::IntLiteral/FloatLiteral）收集到 constantPool，编码前
///     预扫描全部计算寄存器得到高水位，常量槽分配在其之后（与计算寄存器空间隔离，
///     避免 SSA value id 密集递增导致的重叠），prologue 预加载。
///
/// regfile 开启（issue #10）时改为**线性扫描分配映射**：计算寄存器压入定长池
/// [numInputs, poolTop)，死值槽位复用，寄存器号不再镜像 SSA value id；池装不下
/// 的活值分到溢出槽（统一 slots 数组高位索引，操作数直接寻址），常量槽在溢出槽
/// 区之后分配。映射计划见 vm_regfile.hpp。
///
/// 寄存器文件布局（见 vm_opcode.hpp；regfile 开启时计算段换为分配池 + 溢出槽区）：
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

/// @brief 编码期选项（issue #9：super-ops / 指令碎片化；issue #10：寄存器文件定长化）。
///
/// @details
/// 三项都是默认关、不被其他开关联动（与 --vmc4-* 现有开关同一约定）。
/// - superOps：相邻指令折叠成复合指令（±1 → SuperInc/SuperDec）
/// - fragment：一条指令拆成多条等价碎片，单独看每条是无意义中间态
/// - regfile：寄存器文件定长化池顶（0 = 关）。开启后计算寄存器经线性扫描
///   分配压入 [numInputs, poolTop)，装不下的活值分到溢出槽（直接寻址，
///   见 vm_regfile.hpp）；fragment 开启时其临时槽改占池顶 3 槽。
struct VmEncodeOptions
{
    bool     superOps = false;   ///< super-operator 折叠（默认关）
    bool     fragment = false;   ///< 指令碎片化（默认关）
    uint32_t regfile  = 0;       ///< 寄存器文件池顶上限（0 = 关，恒等映射；issue #10）
};

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
                                             std::string&                   reason,
                                             const std::vector<UserIntrinsic>* userIntrinsics =
                                                 nullptr,
                                             const VmEncodeOptions& options = {});
};

/// @brief 块 id 容量校验：总块数（真实 + 合成）必须落在 u16 表示域内（#1133）。
///
/// @details
/// VMC4 块 id 是 u16 操作数（Jump/Branch/Switch 的目标、blockTargets /
/// blockExcRegions 的下标），表示域上限 0xFFFF。总块数 = 真实块 + 合成边块
/// （每条带 RegCopy 的后继边各物质化一个合成块，排在真实块之后、同一 id 空间）。
/// 历史校验只卡 realCount ≤ 0xFFFE，合成边块不计入——真实块少但边拷贝多的方法
/// 总块数仍可越过 0xFFFF，id 回绕后跳转目标静默指向错误块（fail-silent 腐蚀）。
/// 编码入口（VmEncoder::encode）在 planSyntheticEdges 之后、任何 id 消费之前
/// 一次性判定：超限 fail-closed 报错退出，不产出错误产物；诊断含总块数、
/// 真实块数、合成块数、上限四个数字，可定位。
///
/// @param realCount 真实块数（RegFunction::blocks.size()）。
/// @param syntheticCount 合成边块数（EncoderState::syntheticEdges.size()）。
/// @param reason 失败时填充诊断（含四个数字）；成功时不修改。
/// @return 总块数 ≤ 0xFFFF 返回 true；超限返回 false。
bool checkBlockIdCapacity(size_t realCount, size_t syntheticCount, std::string& reason);

}   // namespace mj2x

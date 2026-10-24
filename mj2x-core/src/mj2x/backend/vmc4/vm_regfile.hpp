/// @file vm_regfile.hpp
/// @brief VMC4 寄存器文件定长化（issue #10，--vmc4-regfile）：线性扫描分配 + 溢出槽。
///
/// @details
/// RegIR 的虚拟寄存器（RegId.value == SSA value id）默认恒等映射进 VM 寄存器索引
/// ——寄存器号直接暴露 SSA 值编号，是红队的结构性锚点。定长化开启后：
///   - 计算寄存器经**线性扫描分配**（Poletto）压入固定池 `[numInputs, poolTop)`，
///     死值槽位被复用，寄存器号不再镜像 SSA id；
///   - 池装不下的活值分到**溢出槽** `[spillBase, spillBase+spillCount)`。
///
/// ### 溢出槽直接寻址（实现定界：不引入 Spill/Reload opcode）
///
/// 解释器的寄存器文件是统一 slots 数组，溢出槽就是数组高位索引——指令操作数
/// （u16）直接引用溢出槽即可读写，无需 reg↔slot 搬运指令。相比 Spill/Reload
/// 双 opcode 方案：零运行时开销、零 opcode/解释器/实例面改动、编码器只需在
/// 操作数解析单点（resolveOperand）做重映射。若未来运行时把寄存器文件拆成
/// 定长栈上数组 + 独立溢出区（handler 只摸定长数组），再补 Spill/Reload。
///
/// ### 与 super-ops / fragment 的组合
///
/// - 融合对（planSuperOps 产物）在分配前视为单条复合指令：被吸收指令的 dest
///   不再存在（handler 局部变量承载），不参与分配；融合产物的源 = 两条指令
///   全部源操作数，dest = 承载指令的 dest。
/// - fragment 临时槽改占**池顶 3 槽** `[poolTop, poolTop+3)`（fragment 关闭时
///   不预留），保持全部计算面落在寄存器文件上限之内；溢出槽区从 limit 起。
/// - 常量槽 / String/.class 字面量槽仍在溢出槽区之后分配（与历史形态同构）。
///
/// ### 确定性
///
/// 同一 RegFunction + 同一 limit → 同一分配（可复现构建不变量）。区间排序、
/// 空闲池、活跃表全部用有序容器 + 确定性 tie-break，无未播种随机源。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mj2x {

struct RegBlock;
struct RegFunction;

/// @brief 一个 RegFunction 的定长化分配计划（VmEncoder 消费）。
struct VmRegfilePlan
{
    /// RegId.value → VM 槽位索引（池内寄存器或溢出槽）。参数区 [0, numInputs)
    /// 恒等映射不入表（参数是入口现场，不可移动）。融合吸收的 dest 不入表
    /// （融合后不再存在，编码器不会发射它）。
    std::map<uint32_t, uint32_t> vmRegOf;

    uint16_t numInputs = 0;   ///< 参数区大小（恒等映射段）
    uint16_t poolTop   = 0;   ///< 池顶：分配池 = [numInputs, poolTop)
    uint16_t spillBase = 0;   ///< 溢出槽区起点 = regfileLimit（fragment 预留在其前）
    uint32_t spillCount = 0;  ///< 溢出槽数
    uint32_t highWater  = 0;  ///< 溢出槽区终点；常量槽从该高水位向上分配
};

/// @brief 对一个 RegFunction 做线性扫描寄存器分配（issue #10）。
///
/// @details
/// 活性分析（块级 backward 不动点）→ 活跃区间（定义点/使用点 + liveIn/liveOut
/// 块跨度包络，包络是安全过近似：非重叠包络必不干扰）→ Poletto 线性扫描
/// （池耗尽时溢出「结束点最远」的区间，被溢出者得稳定溢出槽）。
///
/// 边拷贝语义：后继边拷贝的 source 在**源块 terminator 位点**被读取、dest 在
/// **目标块入口**诞生（块参数）；异常区域拷贝的 source 在**保护块末尾**被读取
/// （保护块内任何可抛位点都可能分派）、dest 在 **handler 块入口**诞生；
/// exceptionValue 同为 handler 入口定义。
///
/// @param fn 已过 DCE 的 RegIR 函数。
/// @param regfileLimit 寄存器文件上限（池顶，含参数段）；1..0xFFFF。
/// @param fragmentReserve fragment 开启：池顶预留 3 个临时槽（要求
///        regfileLimit ≥ numInputs + 3，否则 fail-closed）。
/// @param superOpPlan VmEncoder::planSuperOps 的融合计划（融合对按单条复合
///        指令参与分配）；superOps 关闭时传空表。
/// @param out 成功时填充分配计划。
/// @param reason 失败时填充诊断；成功时不修改。
/// @return 成功 true；参数越界 / fragment 容量不足返回 false。
bool planVmRegfile(const RegFunction& fn, uint32_t regfileLimit, bool fragmentReserve,
                   const std::map<const RegBlock*, std::vector<int>>& superOpPlan,
                   VmRegfilePlan& out, std::string& reason);

}   // namespace mj2x

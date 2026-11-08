/// @file vmx64_encoder.hpp
/// @brief Vmx64Encoder：把 RegFunction 编码成最小 opcode 指令流（Vmx64Program）。
///
/// @details
/// vmx64 编码器位于 RegIR 之后的分叉点（与 VMC4 的 VmEncoder 平行）：
///   RegFunction → Vmx64Encoder::encode() → Vmx64Program → Vmx64AsmEmitter
///
/// ### 与 VMC4 编码策略的差异
///
/// - **最小 opcode 面**（Phase 1）：只支持 i32/i64 常量与拷贝、add/sub/mul、
///   if_icmp 分支族（Compare+Branch 融合）、goto、返回族；其余 family 一律
///   fail-closed（返回 nullptr + reason），backend 据此保留字节码不转 native。
/// - **无常量池预装载**：字面量在消费点前用 IConst/LConst 物化进 2 个共享
///   暂存 slot（物化点与唯一消费者相邻，复用安全），全部数据内联指令流。
/// - **流中无明文 opcode**：每个指令站点以 [tEnc:u16] 开头（站点表索引），
///   加密由发射器施加（编码器落明文 t，发射器按 key schedule 改写密文）。
/// - **Compare+Branch 融合**：RegIR 的 Branch 消费 Compare 结果；当 Compare
///   结果寄存器仅被该分支消费且双方操作数都是 i32（寄存器或字面量）时，
///   融合为一条 IfIcmp 站点（JVM if_icmp<cc> 语义），Compare 本体不再出现。
///
/// ### 两遍编码
///
/// 分支/跳转目标是 u32 流偏移，只有所有块布局确定后才能解析，因此：
///   - 第一遍（plan）：按块规划站点序列与大小，记录每块起始偏移；
///   - 第二遍（materialize）：写流（t 明文 + 操作数 + 解析后的目标偏移）。
///
/// ### fail-closed
///
/// 任何不支持的形态（字段/调用/数组/分配/intrinsic/monitor/异常/switch/
/// 浮点/对象引用/单独 Compare 等）整体失败并填 reason——不生成半成品。
#pragma once

#include "mj2x/backend/vmx64/vmx64_opcode.hpp"

#include <memory>
#include <string>

namespace mj2x {

struct RegFunction;

/// @brief RegIR → vmx64 指令流编码器。
class Vmx64Encoder
{
public:
    /// @brief 编码一个 RegFunction 为 Vmx64Program。
    ///
    /// @param fn 已经过 DCE 的 RegIR 函数（与 VMC4 同前端，不做 RegC 变换）。
    /// @param reason 失败时填充原因（fail-closed 日志）。
    /// @return 成功返回 Vmx64Program；失败返回 nullptr（流中 t 为明文站点号，
    ///         加密由 Vmx64AsmEmitter 施加）。
    static std::unique_ptr<Vmx64Program> encode(const RegFunction& fn, std::string& reason);
};

}   // namespace mj2x

/// @file vmx64_asm_emit.hpp
/// @brief Vmx64AsmEmitter：把 Vmx64Program 发射成 MASM x64（ml64）汇编文本。
///
/// @details
/// vmx64 的核心交付：自包含的 .asm 模块——虚拟寄存器文件（栈上 slot 池）、
/// 加密的 handler 地址表、共享 handler 代码、数据驱动间接跳转 dispatcher。
/// 每条 handler 末尾用 `vmx64_NEXT` 宏从流中取下一 handler 编号（站点号），
/// 解密、查表、解密地址、`jmp rdx`——VMP 风格的间接跳转链，流中不含明文
/// opcode，静态分析无 C 骨架可锚定。
///
/// ### VM 运行时寄存器约定（发射文本与 handler 共同遵守）
///
///   rbx = pc（当前站点 tEnc 字的流偏移，相对流起点，非绝对地址——ASLR 无关）
///   rsi = 虚拟寄存器 slot 池基址（[rbp-frame] 栈上）
///   rdi = 加密站点表基址（table[t] = handler 地址 - ks64(t)）
///   r13 = 本方法 done 标签（返回出口）
///   r14 = 指令流基址
///   r15 = key schedule 常量 K1（seed 派生）
///   r12 = 链 rolling key（每 dispatch 由 pc 重算，再与站点密钥异或滚动；
///         功能性密钥全部由 pc/站点号确定性派生——路径无关，正确性不依赖
///         执行历史；r12 的静息值随路径滚动，纯分析噪声）
///   rax/rcx/rdx/r8-r11 = handler 内 scratch（跨 handler 不保序）
///
/// ### Key schedule（Phase 1 最简版）
///
///   ks64(x) = (rotl64(x * P1, 13) ^ K1) * P2
///   P1 = 0x61C88647, P2 = 0x045D9F3B（固定公开常数，均 < 2^31，可作
///   imul r64, r64, imm32 立即数发射）；K1 从 instanceSeed 派生。
///
/// 两处消费（发射侧 C++ 与 .asm handler 侧逐指令镜像，改公式必须双侧同步）：
///   - 链加密：流中 tEnc = t XOR (u16)ks64(流偏移)——handler 由 rbx 重算；
///   - 表加密：table[t] = handler 地址 - ks64(t)——handler 解密用 add。
///     （XOR 对重定位符号不可表达——`sym XOR imm` 不是合法 MASM 数据表达式，
///     源级可表达的最近形态是 `sym - imm`（重定位 + addend）；故编号链用
///     XOR、地址表用 SUB/ADD。二进制层若需纯 XOR，走 post-link 加密器，
///     见下方「post-link 纯 XOR 加密器」设计注。）
///
/// ### post-link 纯 XOR 加密器（设计注，未实现——接口意向定档）
///
/// 源级 `sym - imm` 形态把 addend 明文留在重定位里（.obj/.dll 的
/// RELOC{IE}NT 携带明文 handler 偏移差，逆向者重放 `+addend` 即还原明文表）。
/// 消除该静态痕迹需在**链接后**对产物字节做二次加密，把表项改写成
/// `handler - ks64(t) XOR mask` 纯异或形态——但那要求 handler 侧解密序列
/// 同步改为 `xor`，且解密用的 mask 域与重定位解析互斥（重定位写入的地址
/// 会覆盖密文）。定档设计约束（实现时在此接口下单点收口，不改散装公式）：
///
///   - **单源锚点**：keystream 只经 vmx64Ks64（本文件唯一公开派生函数），
///     加密器 = 读产物 → 定位 .data 表/流（按发射器落盘符号名
///     `mj2x_vmx64_sites_<tag>`/`mj2x_vmx64_code_<tag>` 的 COFF/PE 段偏移）→
///     重算 keystream → 异或改写 → 原地落盘。发射侧不新增平行公式。
///   - **时序**：CLI 侧在 buildXmakeProject 产出 DLL 之后、嵌入/交付之前
///     插桩（单消费点，与 VMC4 的 JNI_OnLoad at-rest 解密互斥——纯 XOR 改写
///     后运行期无需解密原语，明文只在 dispatch 瞬时存在于 rdx）。
///   - **确定性**：同 seed + 同产物 → 逐字节相同（keystream 全部由
///     instanceSeed/站点号/流偏移确定性派生，无随机量）。
///   - **前置条件**：加密器要求产物含 vmx64 符号名（发射器符号命名是契约，
///     不得在加密器侧重新推导）；不满足即整体拒绝（fail-closed，不产半加密
///     产物）。
///
/// ### 确定性
///
/// 同 instanceSeed + 同 Vmx64Program → 逐字节相同的 .asm 文本（无时间戳、
/// 无绝对路径、无宿主环境差异）。
///
/// ### JNI 边界（混合链接边界）
///
/// 每方法入口 `mj2x_vmx64_exec_<tag> PROC FRAME` 按 JNIEnv ABI 收参
/// （env=rcx、thisOrClass=rdx、p0=r8、p1=r9、p2+ 走栈），env/this 落帧槽
/// [rbp-8]/[rbp-16] 供后续的 extern C helper（mj2x-rt 字段访问/分配/
/// 异常等 env 重活）经既定边界取用；最小 opcode 集不在 VM 循环内
/// 外呼 C。RegisterNatives 绑定 C TU 由 Vmx64Backend::finalize 发射
///（registrationSource()，见 backend 注释）。
#pragma once

#include "mj2x/backend/vmx64/vmx64_opcode.hpp"

#include <cstdint>
#include <string>

namespace mj2x {

/// @brief 主 seed 与身份哈希混合派生实例 seed（确定性，Phase 1 最简版）。
///
/// @details
/// 同主 seed + 同身份 → 同实例 seed；不同身份 → 不同实例 seed。接口对齐
/// VMC4 的 vmc4DeriveInstanceSeed（后续可换 VMC4 式 seed_derivation 家族，
/// 消费点只有 ks64 的 K1 输入一处）。
uint64_t vmx64DeriveInstanceSeed(uint64_t mainSeed, uint64_t identityHash);

/// @brief key schedule 常数单源（发射侧 C++ 与 .asm 文本同源取值）。
///
/// @details
/// 均为正 32 位奇数：可作 `imul r64, r64, imm32` 立即数直接发射。
inline constexpr uint64_t kVmx64P1 = 0x61C88647ULL;
inline constexpr uint64_t kVmx64P2 = 0x045D9F3BULL;

/// @brief ks64(x)：发射侧 C++ 镜像（与 .asm 内 imul/rol/xor/imul 序列逐位一致）。
///
/// @details
/// x 为流偏移（u32 域）或站点号（u16 域）；返回值低 16 位用于链字加密，
/// 全 64 位用于地址表 addend。
uint64_t vmx64Ks64(uint64_t x, uint64_t k1);

/// @brief MASM x64 发射器。
class Vmx64AsmEmitter
{
public:
    /// @brief 发射共享 handler 集与 vmx64_NEXT 宏（每个 .asm 一份）。
    ///
    /// @details
    /// handler 跨方法共享（语义只依赖站点操作数布局），在 finalize 组装时
    /// 置于全部方法段之前。
    static std::string emitSharedHandlers();

    /// @brief 发射单个方法段：加密指令流（.data）+ 加密站点表（.data）+
    ///        JNI 入口 PROC（.code）。
    ///
    /// @param program 编码产物（stream 中 t 为明文站点号，本函数在副本上加密）。
    /// @param instanceSeed 方法实例 seed（派生 K1）。
    /// @param methodTag 方法级唯一标识（fnv1a64 hex，符号命名去重）。
    /// @return 方法段 .asm 文本。
    static std::string emitMethod(const Vmx64Program& program, uint64_t instanceSeed,
                                  const std::string& methodTag);
};

}   // namespace mj2x

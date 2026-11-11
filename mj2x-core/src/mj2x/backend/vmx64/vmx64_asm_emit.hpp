/// @file vmx64_asm_emit.hpp
/// @brief Vmx64AsmEmitter：把 Vmx64Program 发射成 MASM x64（ml64）汇编文本。
///
/// @details
/// vmx64 的核心交付：自包含的 .asm 模块——虚拟寄存器文件（栈上 slot 池）、
/// handler 地址表（下称「站点表」）、共享 handler 代码、数据驱动的间接跳转
/// dispatcher。每条 handler 末尾用 `vmx64_NEXT` 宏从指令流取下一 handler
/// 编号（站点号），解密、查站点表、解回 handler 真实地址、`jmp rdx`——
/// VMP 风格的间接跳转链：指令流里不含明文 opcode，静态分析没有 C 骨架可
/// 锚定。站点表的内存布局、post-link 加密算法与运行时双形态 dispatch 的
/// 完整设计（含数字例子）见 doc:vm-site-table-encryption；本头文件是发射
/// 侧（C++）、汇编侧（handler/NEXT 宏）与加密器（vmx64_postlink）三方的
/// 契约单源。
///
/// ### VM 运行时寄存器约定（发射文本与 handler 共同遵守）
///
///   rbx = pc（当前站点 tEnc 字的流偏移，相对流起点，非绝对地址——ASLR 无关）
///   rsi = 虚拟寄存器 slot 池基址（[rbp-frame] 栈上）
///   rdi = 站点表基址（表项内容分两种落盘形态，见「post-link 加密」节）
///   r13 = 本方法 done 标签（返回出口）
///   r14 = 指令流基址
///   r15 = key schedule 常量 K1（seed 派生）
///   r12 = 形态标记 + 镜像基差，双重角色，入口 prologue 一次算定：
///         r12 = base qword 落盘值 - 站点表运行时地址。
///         未加密产物：base qword 带 DIR64 重定位，装载器把它改写成表的
///         运行时地址 → r12 = 0，NEXT 的 test/jz 走 add-only 路径。
///         post-link 加密产物：base qword 的重定位被熄灭（type 翻 0，
///         装载器跳过，见下），保持链接期首选 VA → r12 = 首选 VA - 运行时
///         地址 = -delta（delta = 实际装载基址 - 首选基址），NEXT 先 xor
///         解表项、再 `sub rdx, r12` 把基差补回去。r12 只承载这两件事，
///         不含任何噪声位。
///   rax/rcx/rdx/r8-r11 = handler 内 scratch（跨 handler 不保序；NEXT 的
///         链密钥计算用 rcx——rdx 在表项解密期承载中间值）
///
/// ### Key schedule
///
///   ks64(x) = (rotl64(x * P1, 13) ^ K1) * P2
///   P1 = 0x61C88647, P2 = 0x045D9F3B（固定公开常数，均 < 2^31，可作
///   imul r64, r64, imm32 立即数发射）；K1 从 instanceSeed 派生。
///
/// 注意区分另一种密钥流：mj2x/core/native_lib_crypto.hpp 的 xorshift32
/// 字节流（native 库加密存储，镜像边界是 NativeLoader 字节码）。两者只是
/// 「XOR 密钥流」同概念，机制与镜像边界不同——ks64 位置驱动、镜像 .asm
/// dispatch，勿混淆、勿合并。
///
/// 两个消费点（发射侧 C++ 与 .asm handler 侧逐指令镜像，改公式必须双侧
/// 同步——vmx64_stream_interp_test 的 Ks64AsmMacroReplaysCppSide 逐行锁死）：
///   - 指令流加密：流中每条指令开头的 tEnc 字 = 站点号 t XOR
///     (u16)ks64(流偏移)——handler 由 rbx（当前流偏移）重算同值异或解回；
///   - 站点表加密：源级 `sym - imm`（重定位 + addend）链接后落盘
///     `handler 首选 VA - ks64(t)`；post-link 加密器再改写为纯异或形态
///     （见下节）。
///
/// ### post-link 站点表加密（vmx64_postlink.cpp）
///
/// 未加密的链接产物在 .data 里留下两类可被静态分析直接读走的明文：
/// 表项字节本身（handler 首选 VA - ks64(t) 的线性差，配合 .reloc 里逐条
/// 指向表区的 DIR64 项，就是一张「函数指针表」级路标）与表尾的 k1 密钥
/// 材料。加密器对链接产物做三步原地改写（算法推导与数字例子见
/// doc:vm-site-table-encryption）：
///
///   1. **表项纯异或**：entry[t] = (handler 首选 VA - ks64(t))
///      XOR ks64(t, k1)——密钥材料来自表尾 k1 qword（与 prologue 的
///      `mov r15, imm` 同一 C++ 局部变量，无第二表达）。
///   2. **重定位熄灭**：表区全部 DIR64 项 + base qword 的 DIR64 项翻成
///      ABSOLUTE（type 0，装载器跳过）——.reloc 不再指认表区，装载器
///      也不会把基差加进密文。
///   3. **定位痕迹清零**：magic / 站点数 / k1 三个 qword 归零（表项与
///      base 保留）。痕迹即定位符：重复加密时 magic 扫描零命中 = fail-closed
///      拒绝，天然挡住二次加密（幂等护栏）。
///
/// 改写后的运行时 dispatch（NEXT 的 r12 两条路径，逐项算术与数字例子见
/// doc:vm-site-table-encryption）：
///   - 加密形态：rdx = 表项密文；`xor rdx, r11`（r11 = ks64(t)）→ 链接期
///     落盘值 handler 首选 VA - ks64(t)；`add rdx, r11` → handler 首选 VA；
///     `sub rdx, r12`（r12 = -delta）→ handler 运行时 VA。
///   - 未加密形态：装载器已把表项加过基差 → rdx = handler 首选 VA -
///     ks64(t) + delta；r12 = 0 → 跳过 xor；`add rdx, r11` → handler 运行
///     时 VA；`sub rdx, 0` 无操作。
///   两态汇合同一条 `add/sub/jmp rdx` 收尾。分支只压在表项 xor 这一条指令
///   上——静态发射的序列无法同时服务两种落盘形态（明文形态的表项经重定位
///   已含基差，再 xor 即破坏）。加密是纯可选加固：未加密产物本身可运行。
///   已知简并：加密产物恰按首选基址装载（delta = 0，ASLR 关闭/固定基址
///   部署）时加密态 r12 亦为 0——分支误走 add-only 跳进密文（静默错，非
///   fail-closed）。ASLR 常开使其实际不可达；固定基址场景须禁用加密或使
///   镜像避开首选基址。
///
/// 配套发射面（本文件 emitMethod 与 NEXT 宏）：
///   - 表头两 qword：magic（kVmx64TableMagic，加密器的字节级定位符）+ 站点数；
///   - 表尾两 qword：k1（加密器密钥材料）+ base（自引用重定位指针）。
///     base 的落盘值 = 站点表首选 VA：**未加密产物装载器照常改写它**
///     （运行时 = 表地址，r12 = 0，--generate-xmake 工作流不破坏）；
///     加密后重定位熄灭，静态首选 VA 常驻，r12 = -delta 补基差。
///   - NEXT 的形态分支：`test r12, r12 / jz @F`——r12 = 0（明文加数形态，
///     表项带活重定位）跳过表项 xor，`add` 直解；r12 ≠ 0（加密形态）先
///     xor 解密文。@@ 为 MASM 匿名标签（宏内多次展开不重名）。
///
/// 契约约束：
///   - **keystream 单源**：一切密钥流只经 vmx64Ks64 一个公式——发射侧
///     C++、.asm 宏文本、post-link 加密器三方共享；加密器不新增平行公式。
///   - **时序**：CLI 侧 --compile 在 buildXmakeProject 产出 DLL 之后、
///     嵌入/交付之前插桩（单消费点）。
///   - **确定性**：同产物 → 逐字节相同的加密结果（无随机量）。
///   - **fail-closed**：定位符缺失/计数越界/k1 为 0/期望的重定位项缺失/
///     非 AMD64 PE——任何一步不满足即整体拒绝，不写半加密产物。
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
/// [rbp-40h]/[rbp-48h]（被保存寄存器区 0x38 之下）供后续的 extern C
/// helper（mj2x-rt 字段访问/分配/异常等 env 重活）经既定边界取用；最小
/// opcode 集不在 VM 循环内外呼 C。RegisterNatives 绑定 C TU 由
/// Vmx64Backend::finalize 发射（registrationSource()，见 backend 注释）。
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
/// 全 64 位用于地址表 addend 与 post-link 表项异或掩码。
uint64_t vmx64Ks64(uint64_t x, uint64_t k1);

/// @brief 站点表定位 magic（post-link 加密器的字节级定位符，加密后清零）。
///
/// @details
/// ASCII "VMX64TBL" 小端序；发射为 `dq <masmHex>`。定位采用 magic 扫描而非
/// 符号名——release DLL 无 COFF 符号表（无 /DEBUG），符号定位不可依赖。
inline constexpr uint64_t kVmx64TableMagic = 0x4C42543634584D56ULL;

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

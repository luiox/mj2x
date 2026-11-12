/// @file vmx64_opcode.hpp
/// @brief vmx64 最小虚拟机指令集定义：opcode 枚举、流布局、程序容器。
///
/// @details
/// vmx64 是 VMC4 虚拟化体系的 x64 汇编发射形态（Phase 1 骨架）：复用 VMC4 的
/// 前端（L1 → SSA → RegIR），但在 RegIR 之后分叉，RegIR 被编码为自定义最小
/// opcode 字节流，由 MASM x64 汇编（ml64）发射的解释器/调度器直接执行，产生
/// VMP 风格的 `jmp rdx` 数据驱动间接跳转链，消除"经 C 编译器生成的规范代码
/// 骨架"这个已知反编译突破口。
///
/// ### 指令格式（统一变长）
///
/// 每个指令站点（site）在流中的布局：
///   [tEnc:u16][operands...]
///
/// - tEnc 是下一 handler 编号（站点在站点表中的索引 t）经 XOR rolling key
///   加密后的密文（见 vmx64_asm_emit.hpp 的 key schedule）。流中不出现明文
///   opcode，指令语义只存在于加密站点表里，纯数据驱动。
/// - operands 按 opcode 固定布局（见 Vmx64Op 各项注释）。
/// - 分支目标（Goto/IfIcmp 族）是 u32 流偏移（相对流起点）。
///
/// ### 虚拟寄存器文件（slot 池）
///
///   [0 .. numRegs)   : 方法参数 + 计算临时（RegIR 虚拟寄存器恒等映射）
///   [numRegs .. numSlots) : 2 个字面量暂存 slot（iconst/lconst 物化用，
///                       每次物化紧跟唯一消费者，复用安全）
///
/// 槽位 8 字节宽；i32 存低 4 字节（读写均按 32 位，高位垃圾无害）。
///
/// ### Phase 1 支持面（fail-closed 哲学）
///
/// 只支持：i32/i64 常量、i32/i64 槽间拷贝、i32/i64 add/sub/mul、if_icmp 分支
/// 族（i32 有符号比较）、goto、i32/i64/void 返回。其余一切 RegIR 形态
/// （字段/调用/数组/分配/intrinsic/monitor/异常/switch/浮点/对象）一律编码
/// 失败（fail-closed），保留字节码、不转 native。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mj2x {

/// @brief vmx64 Phase 1 opcode 枚举。
///
/// @details
/// 枚举数值只用于发射端命名/诊断；流内指令的语义由加密站点表（表项指向
/// 对应 handler 标签）承载，opcode 数值不进流。
enum class Vmx64Op : uint8_t
{
    IConst = 0,   ///< [dst:u16][imm:i32]        slot[dst] = imm（字面量物化）
    LConst,       ///< [dst:u16][imm:i64]        slot[dst] = imm
    ILoad,        ///< [dst:u16][src:u16]        slot[dst](i32) = slot[src](i32)
    LLoad,        ///< [dst:u16][src:u16]        slot[dst](i64) = slot[src](i64)
    IStore,       ///< [dst:u16][src:u16]        同 ILoad（语义孪生，扩大 opcode 面）
    LStore,       ///< [dst:u16][src:u16]        同 LLoad
    IAdd,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i32) = slot[a] + slot[b]
    LAdd,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i64) = slot[a] + slot[b]
    ISub,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i32) = slot[a] - slot[b]
    LSub,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i64) = slot[a] - slot[b]
    IMul,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i32) = slot[a] * slot[b]
    LMul,         ///< [dst:u16][a:u16][b:u16]   slot[dst](i64) = slot[a] * slot[b]
    IfIcmpEq,     ///< [a:u16][b:u16][t:u32][f:u32]  if (a == b) pc = t else pc = f
    IfIcmpNe,     ///< 同上 !=
    IfIcmpLt,     ///< 同上 <（i32 有符号）
    IfIcmpLe,     ///< 同上 <=
    IfIcmpGt,     ///< 同上 >
    IfIcmpGe,     ///< 同上 >=
    Goto,         ///< [target:u32]              pc = target
    IReturn,      ///< [src:u16]                 return slot[src](i32)
    LReturn,      ///< [src:u16]                 return slot[src](i64)
    VReturn,      ///<                           return（void）
    Halt,         ///<                           防御性停机哨兵（返回 0 退出）
    Count,        ///< 哨兵：支持集大小
};

/// @brief Vmx64Op 的 MASM handler 标签短名（emit 命名与诊断共用单源）。
const char* vmx64OpHandlerName(Vmx64Op op);

/// @brief Vmx64Op 的诊断名（日志/reason 文本）。
const char* vmx64OpName(Vmx64Op op);

/// @brief 该 opcode 的 operand 字节数（不含前导 tEnc:u16）。
uint32_t vmx64OpOperandSize(Vmx64Op op);

/// @brief 一个指令站点的描述（编码侧记录，发射侧消费）。
struct Vmx64Site
{
    uint32_t streamOffset = 0;   ///< 站点 tEnc 字的流偏移（链密钥输入）
    Vmx64Op  op           = Vmx64Op::Halt;
};

/// @brief JNI 参数落槽描述（入口 prologue 消费：ABI 位置 → vreg slot）。
struct Vmx64ArgSlot
{
    uint16_t slot = 0;
    bool     is64 = false;   ///< true = jlong（64 位搬移），false = jint
};

/// @brief 一个方法的完整 vmx64 程序（编码产物，发射器输入）。
struct Vmx64Program
{
    std::vector<uint8_t> stream;          ///< 指令流：[tEnc:u16][operands] 逐站点
    std::vector<Vmx64Site> sites;         ///< 站点表：索引 t = 站点序号
    uint32_t numRegs  = 0;                ///< 参数 + 计算寄存器高水位 + 1
    uint32_t numSlots = 0;                ///< numRegs + 2（字面量暂存 slot）
    uint32_t numInputs = 0;               ///< 方法参数个数（= JNI 显式参数）
    std::vector<Vmx64ArgSlot> args;       ///< 参数落槽（slot 与 is64）
    bool     returnsValue = false;        ///< 非 void 返回
    bool     retIs64      = false;        ///< 返回 jlong
};

}   // namespace mj2x

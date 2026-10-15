/// @file reg_transform_passes.hpp
/// @brief R4 protection-oriented transforms for Register IR.
///
/// @details
/// The passes operate on stackless RegIR before RegC emission. They preserve
/// edge copies and effect ordering, and only flatten control flow when the
/// complete function has no exception or observable-effect path.
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include "libca/core/datatype.hpp"

namespace mj2x {

/// @brief Select which R4 transforms are applied to a RegIR function.
///
/// @details
/// 基础三项（MBA / opaque / CFF）默认全部关闭：RegC 定位高性能 native 生成器，
/// 默认产物零混淆（语义直译、最快路径）。需要保护时经 CLI 显式开启：
/// `--regc-obfuscate` 开启基础三项；`--regc-mba-deep` / `--regc-opaque-zero` /
/// `--regc-cff-*` 各自联动开启对应基础项并叠加增强（#829）。
struct RegTransformOptions
{
    bool enableMba                   = false;
    bool enableOpaquePredicates      = false;
    bool enableControlFlowFlattening = false;

    // ---- #829 P0：Cloakwork 借鉴项（全部默认关闭，保 RegC 高性能定位）----
    /// CFF 恒假死块密度：0 = 关闭；N > 0 = 每 N 个 dispatcher 跳转注入 1 个
    /// 恒假死块（条件依赖运行时 state，编译器无法证明恒假 → 不可 DCE）。
    ca::u32 cffDeadBlockDensity = 0;
    /// CFF state 码派生随机映射（splitmix64(seed, index)）+ dispatcher 入口
    /// 状态异或（case 常量与写入值不同）。
    bool cffStateEncryption = false;
    /// CFF state 寄存器以 `volatile` 发射（防 C 编译器把 dispatcher 优化回原始
    /// 结构化控制流）。
    ///
    /// 默认 true：不加就等于白做——state 若可跨基本块常量传播，clang -O2 下
    /// dispatcher switch 会被 jump-threading 完全消解（实测见
    /// `work/cff_probe/st_cff.ll`）。仅 CFF 生效时才有影响，不改变默认产物形态。
    /// 关闭的唯一理由是需要省下 volatile 读写开销（此时平坦化只剩源码可读性收益）。
    bool cffStateVolatile = true;
    /// 深 MBA：Add 展开为 De Morgan/进位链（per-site 变体），链中夹
    /// `mj2x_regc_barrier` 防编译器折叠。
    bool mbaDeep = false;
    /// opaque-零恒等变体扩展：demorgan / shift-pair / or-dec（恒真、零 UB
    /// 位运算恒等），按 (seed, site) 选变体。
    bool opaqueZero = false;
    /// 立即数隐藏（对标 jvmtp `obfuscation.constants`）：整型立即数改写为
    /// `E ^ const_key(site)`（E = C ^ K 编译期物化，K 运行时 splitmix64
    /// 派生，RT 双端同源）。默认关闭；**不**被 `--regc-obfuscate` 联动
    /// （每立即数多一次运行时调用，是家族里最重的一项，仅显式开启）。
    /// 仅 RegC/LLVM 两后端发射面支持（vmc4 显式不走 transform passes）。
    /// 排 CFF 之后零交互。
    bool hideImmediateConstants = false;
    /// 变换种子：派生 state 码 / 变体选择均从此确定性派生（同 seed 同产物）。
    uint64_t transformSeed = 0;
};

/// @brief Per-function transform counters used by tests and diagnostics.
struct RegTransformStats
{
    ca::usize mbaRewrites          = 0;
    ca::usize opaquePredicates     = 0;
    ca::usize cffEdgeSetters       = 0;
    ca::usize cffDeadBlocks        = 0;
    ca::usize mbaDeepRewrites      = 0;
    ca::usize opaqueZeroPredicates = 0;
    ca::usize constantsHidden      = 0;
    bool      controlFlowFlattened = false;
};

/// @brief Apply the enabled R4 transforms to one RegIR function.
///
/// MBA rewrites pure i32/i64 addition with an equivalent bitwise identity.
/// Opaque predicates are merged into integer/boolean branches. CFF replaces
/// eligible pure CFG edges with state-dispatcher transitions. Functions with
/// exceptions, memory access, calls, barriers, throw terminators, or malformed
/// CFG targets are deliberately excluded from CFF.
RegTransformStats applyRegTransformPasses(RegFunction&               function,
                                          const RegTransformOptions& options = {});

}   // namespace mj2x

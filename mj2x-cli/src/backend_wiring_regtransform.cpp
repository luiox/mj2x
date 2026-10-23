// backend_wiring_regtransform.cpp
//
// R4 保护变换选项构造（RegIR 级，RegC/LLVM 接线共用）。双后端都被裁的构建
// （stackc-only/vmc4-only）无消费方，本 TU 整体置空。

#include "backend_wiring.hpp"

#if !defined(MJ2X_DISABLE_REGC_BACKEND) || !defined(MJ2X_DISABLE_LLVM_BACKEND)

#include "mj2x/core/seed_derivation.hpp"

namespace mj2x::cli {

std::optional<mj2x::RegTransformOptions> buildRegTransformOptions(const CliOptions& options)
{
    // 混淆作用域（issue #8）：显式给了 include 即隐含开启混淆——否则用户要开
    // 两个开关才能"只混淆这几个类"。exclude 单独给不隐含开启（它只做收窄）。
    // 本单元是 regcObfuscate 的唯一消费方，局部等价改写不落回 options。
    const bool obfuscateBase = options.regcObfuscate || !options.obfScopeIncludes.empty();
    if (!obfuscateBase && options.regcCffDead == 0 && !options.regcCffStateEncrypt &&
        !options.regcMbaDeep && !options.regcOpaqueZero && !options.regcHideConstants &&
        !options.regcIndirectCalls) {
        return std::nullopt;
    }
    mj2x::RegTransformOptions transform;
    transform.enableMba                   = obfuscateBase || options.regcMbaDeep;
    transform.enableOpaquePredicates      = obfuscateBase || options.regcOpaqueZero;
    transform.enableControlFlowFlattening = obfuscateBase ||
                                            options.regcCffDead != 0 ||
                                            options.regcCffStateEncrypt;
    transform.cffDeadBlockDensity = static_cast<ca::u32>(options.regcCffDead);
    transform.cffStateEncryption  = options.regcCffStateEncrypt;
    transform.mbaDeep             = options.regcMbaDeep;
    transform.opaqueZero          = options.regcOpaqueZero;
    // 立即数隐藏不被 --regc-obfuscate 联动（每立即数多一次运行时调用，
    // 家族里最重的一项，仅显式开启）；排 CFF 之后由 pass 内部保证。
    transform.hideImmediateConstants = options.regcHideConstants;
    // 调用目标间接化同样不被 --regc-obfuscate 联动（每次调用多一层
    // 分派，热路径开销可观）；排 CFF 之后由 pass 内部保证。
    transform.indirectCallTargets = options.regcIndirectCalls;
    transform.transformSeed =
        mj2x::deriveClientSeed(options.regcTransformSeed, options.clientId);
    return transform;
}

}   // namespace mj2x::cli

#endif   // RegC/LLVM 裁剪面

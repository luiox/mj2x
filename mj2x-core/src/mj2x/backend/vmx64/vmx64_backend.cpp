#include "mj2x/backend/vmx64/vmx64_backend.hpp"

#include "mj2x/core/jvm_opcodes.hpp"

#include "mj2x/backend/dom_inject.hpp"
#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/backend/vmx64/vmx64_encoder.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "libca/log/log_macros.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {

namespace {

using codegen::fnv1a64;
using codegen::hex64;

}   // namespace

// ===========================================================================
// Vmx64Backend::Impl：方法段累积（Phase 1 无 RegisterNatives C TU——Phase 2 补）。
// ===========================================================================
struct Vmx64Backend::Impl
{
    ca::str::Utf8StringBuilder source;

    /// 已虚拟化方法计数（日志/diagnostics 用；Phase 1 仅计数不落注册表）。
    size_t virtualizedMethods = 0;
};

Vmx64Backend::Vmx64Backend()
    : impl_(std::make_unique<Impl>())
{}

Vmx64Backend::~Vmx64Backend() = default;

// ===========================================================================
// compileMethod：与 VMC4 同前端（L1 → SSA → RegIR → DCE），分叉点换
// Vmx64Encoder + Vmx64AsmEmitter。fail-closed：任一阶段失败保留字节码。
// ===========================================================================
BackendResult Vmx64Backend::compileMethod(const std::string& className, CompilerMethod& method,
                                          const std::vector<Block*>& blocks, CompilerIndex* index)
{
    (void)index;
    BackendResult     backendResult;
    const std::string methodName = method.name;
    const std::string methodDesc = method.desc;

    // 构造器/类初始化器不直接标 ACC_NATIVE（JVM 部分实现拒收），Phase 1 不转。
    if (methodName == "<init>" || methodName == "<clinit>") {
        CA_LOGT_WARN("vmx64", "Skipping {}.{}{}: constructor/<clinit> not virtualized in Phase 1\n",
                     className.c_str(), methodName.c_str(), methodDesc.c_str());
        return backendResult;
    }

    // --- SSA 构建（与 RegC/VMC4 共享前端）---
    SSABuilder              ssaBuilder;
    ca::str::Utf8StringArena ssaArena;
    std::string             reason;
    const bool              isStatic = (method.access & Opcodes::ACC_STATIC) != 0;
    auto ssa = ssaBuilder.build(blocks, className, methodName, methodDesc, isStatic, method.maxLocals,
                                ssaArena, reason);
    if (!ssa) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: SSA build failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // --- RegIR lowering + DCE ---
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: lowering failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    // 不调用 applyRegTransformPasses：与 VMC4 同口径（MBA/CFF 是 RegC 的发射期
    // 混淆；vmx64 的混淆本体是 opcode 编码 + 加密跳转链，不需要在 IR 内再套）。

    // --- vmx64 分叉点：RegIR → 最小 opcode 流 ---
    auto program = Vmx64Encoder::encode(*reg, reason);
    if (!program) {
        CA_LOGT_ERROR("vmx64", "Skipping {}.{}{}: vmx64 encode failed: {}\n", className.c_str(),
                      methodName.c_str(), methodDesc.c_str(), reason.c_str());
        return backendResult;
    }

    // 方法级 tag（fnv1a64 hex，保证多方法符号命名唯一）与实例 seed 派生。
    const std::string methodId     = className + "." + methodName + methodDesc;
    const std::string methodTag    = hex64(fnv1a64(methodId));
    const uint64_t    instanceSeed = vmx64DeriveInstanceSeed(options_.seed, fnv1a64(methodId));

    // --- 发射 .asm 方法段（加密流 + 加密站点表 + JNI 入口 PROC）---
    backendResult.kind = BackendResult::Kind::ASM_SOURCE;
    backendResult.text = Vmx64AsmEmitter::emitMethod(*program, instanceSeed, methodTag);

    impl_->virtualizedMethods++;
    impl_->source.append(backendResult.text.data(), backendResult.text.size());
    return backendResult;
}

// ===========================================================================
// injectIntoClass：成功产物形态 = ASM_SOURCE 时标 ACC_NATIVE（镜像 VMC4 的
// dom_inject 路径；Phase 1 无 RegisterNatives clinit 注入——注册 C TU 缺口）。
// ===========================================================================
void Vmx64Backend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                   const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::ASM_SOURCE) return;

    if (clazz.dom == nullptr) {
        throw std::runtime_error("vmx64_backend: DOM not synced for native injection");
    }
    const std::string className(clazz.dom->thisName());
    dom_inject::markMethodNative(clazz, className, method.name, method.desc);
}

// ===========================================================================
// finalize：组装 .asm TU（头注释 + 共享 handler 集 + 全部方法段）。
// ===========================================================================
void Vmx64Backend::finalize(const CompilerIndex& index)
{
    (void)index;
    resetDiagnostics();

    std::string full;
    full += "; ----------------------------------------------------------------------------\n";
    full += "; Generated by mj2x Vmx64Backend (MASM x64, ml64)\n";
    full += ";\n";
    full += "; Deterministic build: same seed + same input -> byte-identical .asm.\n";
    full += "; Windows x64 only. Assemble with: ml64 /c vmx64.asm\n";
    full += ";\n";
    full += "; Structure: vmx64_NEXT dispatch macro + shared handlers (jmp rdx chain),\n";
    full += "; then per method: encrypted opcode stream + encrypted handler site table\n";
    full += "; (.data) and a JNI native entry PROC (vreg slots on the stack frame).\n";
    full += ";\n";
    full += "; Phase 1 JNI mixed-linking boundary: entries receive (env, thisOrClass,\n";
    full += "; args) per JNI ABI and home env/this into frame slots; the minimal opcode\n";
    full += "; subset never calls out to C. Phase 2 adds extern C helpers (mj2x-rt) for\n";
    full += "; field access / allocation / exceptions plus the RegisterNatives C TU.\n";
    full += "; ----------------------------------------------------------------------------\n";
    full += "\n";
    full += Vmx64AsmEmitter::emitSharedHandlers();

    const std::string methods = impl_->source.build().to_std_string();
    full += methods;

    if (impl_->virtualizedMethods == 0u) {
        CA_LOGT_WARN("vmx64", "finalize: no method virtualized (all inputs fail-closed or empty)\n");
    }

    sourceCode_ = std::move(full);
    if (!asmOutputPath_.empty()) {
        std::ofstream ofs(asmOutputPath_, std::ios::binary);
        if (ofs.is_open()) {
            ofs << sourceCode_;
            ofs.flush();
            if (!ofs.good())
                CA_LOGT_ERROR("vmx64", "finalize: short write to {}\n", asmOutputPath_);
        }
    }
}

}   // namespace mj2x

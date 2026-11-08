/// @file vmx64_backend.hpp
/// @brief vmx64 后端：VMC4 虚拟化体系的 x64 汇编发射形态（Phase 1 骨架）。
///
/// @details
/// Vmx64Backend 实现 mj2x::Backend 契约，前端链路与 VMC4 完全共享
/// （L1 → SSA → RegIR → DCE，不做 RegC 变换），在 RegIR 之后分叉：
///   - VMC4：RegIR → VmEncoder → VmInterpreterEmitter → C 解释器
///   - vmx64：RegIR → Vmx64Encoder → Vmx64AsmEmitter → MASM x64 .asm
///
/// 产物是自包含的 .asm 模块：栈上 vreg slot 池、加密 handler 地址表、共享
/// handler、`jmp rdx` 数据驱动间接跳转 dispatcher（见 vmx64_asm_emit.hpp），
/// 消除"经 C 编译器生成的规范代码骨架"这一已知反编译突破口。
///
/// ## 失败策略
///
/// fail-closed：SSA/RegIR/编码任一阶段失败（含 Phase 1 不支持的 family——
/// 仅 i32/i64 常量/拷贝/add/sub/mul/if_icmp/goto/返回族），保留字节码、不标
/// native、留可观测日志（继承 VMC4 哲学）。
///
/// ## Phase 1 缺口（有意控面，非静默降级）
///
/// - 无 RegisterNatives 绑定 C TU（.asm 入口已按 JNI ABI 收参，Phase 2 补
///   注册侧）；CLI 侧 vmx64 只开放 --emit-c（编译/工程生成 fail-closed）。
/// - Windows x64 only（MASM/ml64）。
#pragma once

#include "mj2x/backend/backend.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief vmx64 配置选项（Phase 1：seed 单项）。
///
/// @details
/// seed 驱动 key schedule 常数派生（K1 → 流/表加密）。同 seed + 同输入 →
/// 逐字节相同产物；不同 seed → 不同密文流/表。后续扩 dispatcher 形态/实例
/// 粒度时在此追加（对齐 Vmc4Options 的演进路径）。
struct Vmx64Options
{
    uint64_t seed = 0;
};

/// @brief vmx64 虚拟机后端。
class Vmx64Backend : public Backend
{
public:
    Vmx64Backend();
    ~Vmx64Backend() override;

    std::string_view name() const override { return "vmx64"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks,
                                CompilerIndex* index = nullptr) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::MasmX64, sourceCode_, "vmx64.asm"};
    }

    /// @brief 设置 VM 配置（seed）。必须在 compileMethod 之前调用。
    void setOptions(const Vmx64Options& options) { options_ = options; }

    /// @brief Set a `.asm` output path written during finalize.
    void setOutputPath(const std::string& asmOutputPath) { asmOutputPath_ = asmOutputPath; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Vmx64Options options_;
    std::string  asmOutputPath_;
    std::string  sourceCode_;
};

}   // namespace mj2x

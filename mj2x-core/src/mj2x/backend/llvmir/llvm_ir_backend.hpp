/// @file llvm_ir_backend.hpp
/// @brief Textual LLVM IR backend for RegIR methods.
///
/// @details
/// This opt-in backend lowers the existing SSA -> RegIR pipeline to textual
/// LLVM IR without linking against LLVM.  Supported surface (PR #864+): scalar
/// arithmetic/control flow, objects/fields, arrays, instance/external calls,
/// exceptions, monitors, ldc String/Class constants, narrow Z/B/S/C types and
/// float special literals; invokedynamic sites use JNI runtime bootstrap
/// resolution (`indy:<index>`, round 2); `--runtime-protection` checkpoints are
/// injected at method entries (compiled against the embedded mj2x-rt
/// rt_protect source).  Unsupported shapes fail closed so the original Java
/// bytecode remains intact.
#pragma once

#include "mj2x/backend/backend.hpp"
#include "mj2x/codegen/c_options.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief Emits `.ll` source for the supported RegIR subset.
class LlvmIrBackend : public Backend
{
public:
    LlvmIrBackend();
    ~LlvmIrBackend() override;

    std::string_view name() const override { return "llvm_ir"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks,
                                CompilerIndex* index = nullptr) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    /// @brief Set the textual `.ll` output path written during finalize.
    void setOutputPath(const std::string& llvmOutputPath) { llvmOutputPath_ = llvmOutputPath; }

    /// @brief 反侦测检查点（--runtime-protection）：方法入口注入
    ///        mj2x_rt_checkpoint_run 调用（编译侧附加 mj2x-rt rt_protect 源）。
    void setRuntimeProtection(codegen::RuntimeProtectionAction action, uint32_t step,
                              uint32_t delayMs)
    {
        runtimeProtection_.action  = action;
        runtimeProtection_.step    = step;
        runtimeProtection_.delayMs = delayMs;
    }

    /// @brief R4 保护变换（--regc-obfuscate 等，RegIR 级与 RegC 同源）；变换后
    ///        产物若超出 LLVM 子集则回退未变换形态（fail-closed 保方法）。
    void setTransformOptions(const RegTransformOptions& options) { transformOptions_ = options; }

    /// @brief native→native 直调档位（--regc-direct-calls；默认 Off）。
    /// @details 与 RegC 共用 codegen::allowsDirectCall 判定。LLVM 侧原先无条件
    ///          直调同模块 statics，既与 RegC 默认关的口径不一致，又一直在承担
    ///          #814 的栈观察语义代价——纳入同一门控后两端口径统一。
    void setDirectCallTier(codegen::DirectCallTier tier) { directCallTier_ = tier; }

    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::LlvmIr, sourceCode_, "native.ll"};
    }

private:
    /// 当前类是否在混淆作用域内（--obf-scope-include/exclude）。
    bool currentClassInObfuscationScope(const std::string& className, CompilerIndex* index);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string llvmOutputPath_;
    std::string sourceCode_;
    codegen::RuntimeProtectionOptions runtimeProtection_;
    RegTransformOptions               transformOptions_;
    codegen::DirectCallTier           directCallTier_ = codegen::DirectCallTier::Off;
};

}   // namespace mj2x

/// @file llvm_ir_backend.hpp
/// @brief Restricted textual LLVM IR backend for scalar RegIR methods.
///
/// @details
/// This opt-in backend lowers the existing SSA -> RegIR pipeline to textual
/// LLVM IR without linking against LLVM.  Its first slice intentionally accepts
/// only static primitive methods whose RegIR contains pure scalar arithmetic
/// and control flow.  Object/JNI operations, calls, exceptions, monitors and
/// memory access fail closed so the original Java bytecode remains intact.
#pragma once

#include "mj2x/backend/backend.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief Emits `.ll` source for the pure scalar subset of RegIR.
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

    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::LlvmIr, sourceCode_, "native.ll"};
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string llvmOutputPath_;
    std::string sourceCode_;
};

}   // namespace mj2x

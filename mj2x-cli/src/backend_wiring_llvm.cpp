// backend_wiring_llvm.cpp
//
// LLVM IR 后端接线单元：构造 + CLI 选项应用 + pipeline 登记，自注册进
// backend_registry。#886：本 TU 被 MJ2X_DISABLE_LLVM_BACKEND 整体置空
// （with_mj2x_llvm=n 或单后端选择器排除 llvm 时）。

#include "backend_wiring.hpp"

#if !defined(MJ2X_DISABLE_LLVM_BACKEND)

#include "mj2x/backend/llvmir/llvm_ir_backend.hpp"

#include <memory>

namespace mj2x::cli {
namespace {

bool wireLlvmIr(BackendWiringContext& context)
{
    const CliOptions& options = context.options;
    auto              backend = std::make_unique<mj2x::LlvmIrBackend>();
    if (!options.llvmOutputPath.empty()) backend->setOutputPath(options.llvmOutputPath);
    if (auto transform = buildRegTransformOptions(options)) {
        backend->setTransformOptions(*transform);
    }
    if (options.runtimeProtection != "none") {
        backend->setRuntimeProtection(parseRuntimeProtectionAction(options.runtimeProtection),
                                      static_cast<uint32_t>(options.runtimeProtectionStep),
                                      static_cast<uint32_t>(options.runtimeProtectionDelayMs));
    }
    context.nativeSourceBackend = backend.get();
    context.pipeline.registerBackend(std::move(backend));
    return true;
}

/// displayOrder 2 = 历史 CLI 清单第三位。
const BackendModule kLlvmIrModule = {
    CEmitterBackend::LlvmIr,
    "llvm",
    nullptr,
    0,
    2,
    &wireLlvmIr,
    /*usesCloaderBridge*/ false,
    /*injectsNativeLoaderIntoClinit*/ true,
    /*afterPipelineRun*/ nullptr,
    /*params*/ nullptr,
    /*paramCount*/ 0,
};

const BackendModuleRegistration kLlvmIrRegistration{kLlvmIrModule};

}   // namespace
}   // namespace mj2x::cli

#endif   // !MJ2X_DISABLE_LLVM_BACKEND

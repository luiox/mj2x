// backend_wiring_stackc.cpp
//
// StackC 后端接线单元：构造 + CLI 选项应用 + pipeline 登记 + 参数表段，
// 自注册进 backend_registry。#886 单后端构建：本 TU 被
// MJ2X_DISABLE_STACKC_BACKEND 整体置空，模块（含参数段）不进注册表。

#include "backend_wiring.hpp"
#include "param_registry_internal.hpp"

#if !defined(MJ2X_DISABLE_STACKC_BACKEND)

#include "mj2x/backend/stackc/stackc_backend.hpp"

#include <memory>

namespace mj2x::cli {
namespace {

bool wireStackC(BackendWiringContext& context)
{
    auto backend = std::make_unique<mj2x::StackCBackend>();
    if (context.hasUserIntrinsics()) backend->setUserIntrinsics(*context.userIntrinsics);
    if (!context.options.cOutputPath.empty()) backend->setOutputPath(context.options.cOutputPath);
    if (context.options.stackcExceptionCheck == "lite") backend->setExceptionCheckLite(true);
    // --strings-encrypt：字符串池 at-rest 加密（stackc/regc 专属开关）。
    if (context.options.stringsEncrypt) backend->setStringsEncrypt(true);
    context.nativeSourceBackend = backend.get();
    context.pipeline.registerBackend(std::move(backend));
    return true;
}

// ---------- 参数段（拼装于基表 compile 段与 strings 段之间，行序=历史表） ----------

DEF_ENUM_STR(stackcExceptionCheck, "--stackc-exception-check", "stackc_exception_check", "full,lite", "full")

const ParamDef kStackCParams[] = {
    PARAM("stackc.exception_check", "--stackc-exception-check", "stackc_exception_check", ParamType::Enum, "full,lite", 0, 0, "full", "stackc", "stackc", cli_stackcExceptionCheck, json_stackcExceptionCheck),
};

/// displayOrder 0 = 历史 CLI 清单首位；cbackend/c 为兼容别名（#886 保留）。
const char* const kStackCCliAliases[] = {"cbackend", "c"};

const BackendModule kStackCModule = {
    CEmitterBackend::StackC,
    "stackc",
    kStackCCliAliases,
    2,
    0,
    &wireStackC,
    /*usesCloaderBridge*/ true,
    /*injectsNativeLoaderIntoClinit*/ false,
    /*afterPipelineRun*/ nullptr,
    kStackCParams,
    sizeof(kStackCParams) / sizeof(kStackCParams[0]),
};

const BackendModuleRegistration kStackCRegistration{kStackCModule};

}   // namespace
}   // namespace mj2x::cli

#endif   // !MJ2X_DISABLE_STACKC_BACKEND

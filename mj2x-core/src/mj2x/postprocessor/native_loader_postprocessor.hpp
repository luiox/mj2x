#pragma once

#include "mj2x/postprocessor/postprocessor.hpp"

#include <string>

namespace mj2x {

/// @brief 注入 NativeLoader 引导类资源，并可选地为受保护类前插 load 调用。
///
/// @details
/// 两个职责：
///  - 始终把 `mj2x/runtime/NativeLoader.class` 作为 JAR 资源写入 index；
///  - 当 `injectClinitCalls=true` 时，对每个 modified 且非 library 的类，在其
///    `<clinit>` 头部前插 `NativeLoader.load(resourcePath, tempSuffix)` 调用。
///
/// clinit 注入原先在 RegCBackend 内部完成，现已统一收口到此 postprocessor。
/// RegC 和 StackC 均用 RegisterNatives 绑定：
///  - RegC：backend 在 clinit 前插 `registerNativesForClass`，本 postprocessor 再前插
///    `NativeLoader.load`，最终顺序 [load, register, ...]。故传 `injectClinitCalls=true`。
///  - StackC：自己重写整个 clinit（含 load + registerNativesForClass 和隐藏 clinit 编译），
///    故传 `injectClinitCalls=false`，仅注入 NativeLoader 资源，避免重复注入。
class NativeLoaderPostprocessor : public Postprocessor
{
public:
    NativeLoaderPostprocessor(std::string resourcePath, std::string tempSuffix,
                              bool injectClinitCalls = true);
    void process(CompilerIndex& index) override;

private:
    std::string resourcePath_;
    std::string tempSuffix_;
    bool        injectClinitCalls_;
};

}   // namespace mj2x

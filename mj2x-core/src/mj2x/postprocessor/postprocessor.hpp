/// @file postprocessor.hpp
/// @brief 后处理接口：所有方法编译完后、输出前对 CompilerIndex 的统一改写。
///
/// @details
/// Postprocessor 在 Pipeline::runPostprocessors 阶段执行（compileMethods 之后），
/// 用于 class/资源层面的统一收口，避免各 backend 重复实现。典型职责：
///  - NativeLoaderPostprocessor：注入 NativeLoader.class 资源 + clinit load 调用；
///  - CLoaderPostprocessor：注入 cloader/Loader 桥接类（StackC RegisterNatives）。
///
/// 后处理器不应触碰已编译方法的字节码（已被 backend 标记 native），只做资源注入
/// 或 class 级别的 clinit/manifest 调整。
#pragma once

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

/// 抽象后处理器：在所有 backend 编译完成后、输出前运行一次。
class Postprocessor
{
public:
    virtual ~Postprocessor()                   = default;
    virtual void process(CompilerIndex& index) = 0;
};

}   // namespace mj2x

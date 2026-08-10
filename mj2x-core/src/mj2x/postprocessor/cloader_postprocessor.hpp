#pragma once

#include "mj2x/postprocessor/postprocessor.hpp"

#include <string>

namespace mj2x {

/// @brief 生成 mj2x/cloader/Loader 桥接类（registerNativesForClass 的 Java 侧载体）。
/// @details 配置了 native 资源路径时，在 Loader 的 <clinit> 注入一次
///          NativeLoader.load（而不是每个转换类的 <clinit> 重复注入）：
///          转换类 clinit 只调用 Loader.registerNativesForClass，JVM 保证
///          首次调用 static 方法前先执行 Loader.<clinit>（即先加载 native 库）。
class CLoaderPostprocessor : public Postprocessor
{
public:
    void process(CompilerIndex& index) override;

    /// 设置 JAR 内 native 库资源路径（空则不注入 <clinit> load）。
    void setNativeLoaderResourcePath(std::string resourcePath, std::string tempSuffix)
    {
        resourcePath_ = std::move(resourcePath);
        tempSuffix_   = std::move(tempSuffix);
    }

private:
    std::string resourcePath_;
    std::string tempSuffix_;
};

}   // namespace mj2x

/// @file stackc_backend.hpp
/// @brief StackC 后端：广覆盖、正确性优先的 operand-stack 式 C 源生成。
///
/// @details
/// StackC 把 L1 MIR 直译为 `union jvalue stack[]; int sp;` 风格的 C，保留 JVM
/// 栈语义作为正确性基线和回归参考。它是 mj2x 两个后端中覆盖率最广的，但不是
/// 保护主线——保护强度由 RegC（无栈锚点）承担。
///
/// StackC 用 RegisterNatives 绑定 native 方法，clinit 重写（含 registerNativesForClass
/// 和隐藏 clinit 编译）深度耦合注册流程，故 clinit 注入留在 backend 内部（不外移
/// 到 postprocessor），仅 NativeLoader.load 指令发射复用共享常量。
#pragma once

#include "mj2x/backend/backend.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief operand-stack 式 C 源后端，正确性基线。
///
/// @details 生成 `union jvalue stack[]` + sp 操作的 C 函数，覆盖 javac 常见形状、
/// 异常、monitor、invokedynamic 桥。用 RegisterNatives 绑定。
class StackCBackend : public Backend
{
public:
    StackCBackend();
    ~StackCBackend() override;

    std::string_view name() const override { return "c_source"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    /// 设置 finalize 时写入的 .c 文件路径（空则只记录到 generatedSource）。
    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }

    /// @brief 配置 JAR 场景下 clinit 里 NativeLoader.load 的资源路径和临时后缀。
    /// @details StackC 在自己的 protectClassInitializer 里重写 clinit 时使用，
    /// 不走 NativeLoaderPostprocessor（因 RegisterNatives 绑定耦合）。
    void setJarNativeLoaderResourcePath(const std::string& resourcePath,
                                        const std::string& tempSuffix);

    /// @brief 设置 pending 异常检查模式（默认 full）。
    /// @details full：每个 JNI 调用后都插 `(*env)->ExceptionCheck`（当前行为）。
    ///          lite：只保留异常 handler 边界处的检查（对标 JNIC 的稀疏检查）；
    ///          无 handler 的方法依赖 native 返回时 JVM 自动传播 pending 异常。
    ///          热循环内省掉每轮 ExceptionCheck 的 JNI 边界开销。
    void setExceptionCheckLite(bool lite);

    /// 生成代码访问器（测试用）。
    const std::string& generatedHeader() const { return headerCode_; }
    /// 生成代码访问器（测试用）。
    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::C99, sourceCode_, "native.c"};
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string cOutputPath_;
    std::string headerCode_;
    std::string sourceCode_;
};

}   // namespace mj2x

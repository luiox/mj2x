#pragma once

// backend_registry.hpp
//
// 后端模块注册表：每个后端的 CLI 接线单元（构造、CLI 选项应用、pipeline 登记、
// 后处理策略位）以静态注册方式进入本表。mj2x-cli 主流程只依赖本表与
// mj2x::Backend 抽象接口，不再直接引用具体后端头——新增后端或拆分后端到
// 独立接线单元时主流程零改动。
//
// #886 单后端编译裁剪：被裁后端的接线单元整体不参与注册（TU 内容被裁剪宏
// 置空），注册表即本构建实际可用的后端集合，与 native_compiler.hpp 的
// backendEnabled 编译期契约保持一致（backendEnabled(kind) ⇔
// findBackendModule(kind) != nullptr，同受 MJ2X_DISABLE_*_BACKEND 驱动）。

#include "param_registry.hpp"
#include "mj2x/core/user_intrinsics.hpp"

#include <string>
#include <vector>

namespace mj2x {

class Pipeline;
class Backend;

}   // namespace mj2x

namespace mj2x::cli {

/// @brief 后端接线上下文：wire / afterPipelineRun 回调的输入输出载体。
struct BackendWiringContext
{
    CliOptions&                      options;
    const mj2x::UserIntrinsicConfig* userIntrinsics      = nullptr;   ///< null = 未加载配置
    mj2x::Pipeline&                  pipeline;
    mj2x::Backend*                   nativeSourceBackend = nullptr;   ///< wire 成功后指向已登记后端

    /// 与历史 main.cpp 口径一致：intrinsics 与 sources 双空 = 无用户 intrinsic。
    bool hasUserIntrinsics() const
    {
        return userIntrinsics != nullptr &&
               (!userIntrinsics->intrinsics.empty() || !userIntrinsics->sources.empty());
    }
};

/// @brief 单个后端的 CLI 接线模块（自注册单元）。
///
/// displayOrder 固定为历史 CLI 清单顺序（stackc, regc, llvm, vmc4）——错误
/// 文案的后端列表、--backend 枚举串顺序都以它为准，新增后端在尾部追加。
struct BackendModule
{
    CEmitterBackend    kind;        ///< 后端枚举（分发/校验键）
    const char*        cliName;     ///< --backend 取值（"stackc"）
    const char* const* aliases;     ///< 额外接受的 CLI 名（stackc: cbackend/c）
    std::size_t        aliasCount;
    int                displayOrder;///< 注册表展示顺序（历史清单序）
    /// 构造后端、应用 CLI 选项并登记进 pipeline；返回 false = 接线失败。
    bool (*wire)(BackendWiringContext& context);
    /// 需要 RegisterNatives cloader 桥接后处理（StackC/RegC/VMC4；LLVM 直发 .ll 不需要）。
    bool usesCloaderBridge;
    /// jar native-loader 模式下向每个转换类 <clinit> 注入 NativeLoader.load
    /// （RegC/LLVM 经 System.load 加载；StackC 自重写 clinit，VMC4 经 Loader 桥）。
    bool injectsNativeLoaderIntoClinit;
    /// pipeline.run() 之后的产物后处理（VMC4 trace surface 等）；空 = 无。
    void (*afterPipelineRun)(BackendWiringContext& context);
    /// 本后端参数表段（拼装于基表 compile 段与 strings 段之间，段序 =
    /// displayOrder；空段 = 无专属参数）。行序决定 --help/schema/模板展示位。
    const ParamDef* params;
    std::size_t     paramCount;
};

/// @brief 后端模块注册表（进程级单例，静态初始化期填充）。
class BackendModuleRegistry
{
public:
    static BackendModuleRegistry& instance();

    /// 按 displayOrder 升序插入（与 TU 初始化顺序无关，输出稳定）。
    void add(const BackendModule& module);

    const std::vector<const BackendModule*>& modules() const { return modules_; }

    /// 按后端枚举查（被裁后端返回 nullptr）。
    const BackendModule* findByKind(CEmitterBackend kind) const;

    /// 按 CLI 名查（含别名，如 "cbackend"/"c" → stackc；未命中返回 nullptr）。
    const BackendModule* findByCliName(const std::string& name) const;

private:
    std::vector<const BackendModule*> modules_;   ///< displayOrder 升序
};

/// @brief 接线单元的静态注册入口：TU 持有静态实例即可完成注册。
struct BackendModuleRegistration
{
    explicit BackendModuleRegistration(const BackendModule& module)
    {
        BackendModuleRegistry::instance().add(module);
    }
};

/// @brief 本构建全部可用后端模块（displayOrder 升序）。
inline const std::vector<const BackendModule*>& backendModules()
{
    return BackendModuleRegistry::instance().modules();
}

/// @brief 按后端枚举查模块（被裁后端返回 nullptr）。
inline const BackendModule* findBackendModule(CEmitterBackend kind)
{
    return BackendModuleRegistry::instance().findByKind(kind);
}

/// @brief 按 CLI 名查模块（含别名；未命中返回 nullptr）。
inline const BackendModule* findBackendModuleByCliName(const std::string& name)
{
    return BackendModuleRegistry::instance().findByCliName(name);
}

}   // namespace mj2x::cli

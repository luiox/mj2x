// backend_registry.cpp
//
// 后端模块注册表实现：注册表单例 + 后端 CLI 名清单（错误文案单源）。
// 各后端接线单元（backend_wiring_*.cpp）在静态初始化期完成自注册。

#include "backend_registry.hpp"

#include <algorithm>

namespace mj2x::cli {

BackendModuleRegistry& BackendModuleRegistry::instance()
{
    static BackendModuleRegistry registry;
    return registry;
}

void BackendModuleRegistry::add(const BackendModule& module)
{
    // 稳定插入：displayOrder 升序，同序保持注册先后（当前无同序模块）。
    auto position = std::upper_bound(modules_.begin(), modules_.end(), module.displayOrder,
                                     [](int order, const BackendModule* existing) {
                                         return order < existing->displayOrder;
                                     });
    modules_.insert(position, &module);
}

const BackendModule* BackendModuleRegistry::findByKind(CEmitterBackend kind) const
{
    for (const BackendModule* module : modules_) {
        if (module->kind == kind) return module;
    }
    return nullptr;
}

const BackendModule* BackendModuleRegistry::findByCliName(const std::string& name) const
{
    for (const BackendModule* module : modules_) {
        if (name == module->cliName) return module;
        for (std::size_t i = 0; i < module->aliasCount; ++i) {
            if (name == module->aliases[i]) return module;
        }
    }
    return nullptr;
}

/// @brief 本构建可用的后端 CLI 名清单（逗号分隔，错误文案用）。
///
/// 顺序 = 注册表 displayOrder（stackc, regc, llvm, vmc4），与历史宏拼接清单
/// 逐字节一致。
std::string enabledBackendCliList()
{
    std::string list;
    for (const BackendModule* module : backendModules()) {
        if (!list.empty()) list += ", ";
        list += module->cliName;
    }
    return list;
}

}   // namespace mj2x::cli

#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "param_registry.hpp"

namespace mj2x::cli {

/// @brief 从 JSON 配置文件加载参数（注册表驱动，全量键）。
///
/// 键名 = 注册表 jsonPath（snake_case 平铺）；未知键与类型错误被拒绝，
/// 报错定位到键名。命令行显式参数在之后覆盖（main.cpp 解析顺序保证，
/// 默认值 < preset < 配置文件 < CLI）。
///
/// @param outRoot 可选出参：解析出的 JSON 对象本体。--dump-effective-config
///                需要它来判断某个键最终落在哪一层——只报 preset/default 会
///                让"生效配置"名不副实（配置文件覆盖过的键会显示成 preset 值）。
bool loadConfigFile(const std::string& path, CliOptions& options, std::string& error,
                    nlohmann::json* outRoot = nullptr);

/// @brief 把一个 JSON 对象按注册表写入 CliOptions（配置文件与 preset 共用）。
///
/// @details
/// preset 与配置文件是同一套键、同一套校验——preset 只是"内置的一组建议值"，
/// 不另起一套解析。unknownKeyPrefix 用于把报错定位到来源层（"preset balanced"
/// 或配置文件路径），键名本身由 parseJson 附带。
bool applyJsonObject(const nlohmann::json& root, CliOptions& options,
                     const std::string& unknownKeyPrefix, std::string& error);

}   // namespace mj2x::cli

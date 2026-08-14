#pragma once

#include <string>

#include "param_registry.hpp"

namespace mj2x::cli {

/// @brief 从 JSON 配置文件加载参数（注册表驱动，全量键）。
///
/// 键名 = 注册表 jsonPath（snake_case 平铺）；未知键与类型错误被拒绝，
/// 报错定位到键名。命令行显式参数在之后覆盖（main.cpp 顺序保证）。
bool loadConfigFile(const std::string& path, CliOptions& options, std::string& error);

}   // namespace mj2x::cli

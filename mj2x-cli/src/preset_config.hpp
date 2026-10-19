#pragma once

// preset_config.hpp
//
// 默认配置档（preset）：一次设一组开关，不用记十几二十个 CLI 参数。
//
// preset 不是第四套配置语法——它是一个内置的 JSON 对象，键名与配置文件完全
// 相同（注册表 jsonPath），走同一套 parseJson 校验。差别只有两点：
//   - 来源：内置而非文件
//   - 优先级：夹在默认值与配置文件之间（配置文件是用户显式写的，应当能覆盖
//     preset；CLI 又能覆盖配置文件）
//
// 四层合并顺序：默认值 < preset < 配置文件 < CLI 参数

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "param_registry.hpp"

namespace mj2x::cli {

/// @brief 内置配置档。None 是默认（与不加 --preset 完全一致）。
enum class PresetName
{
    None,
    Perf,      ///< 性能优先，零混淆
    Balanced,  ///< 常规保护，代价可控
    Max,       ///< 最强，代价不设限
};

/// @brief 解析档位名（大小写不敏感）。非法名返回 false。
bool parsePresetName(const std::string& text, PresetName& out);

/// @brief 档位名文本（帮助/报错/生效配置回显用）。
std::string presetNameString(PresetName preset);

/// @brief 全部合法档位名（报错文案列举用）。
const std::vector<std::string>& presetNames();

/// @brief 档位展开成的配置对象（键 = 注册表 jsonPath）。
///
/// @details
/// 只含"与默认值不同"的键——未列出的键保持默认，preset 是建议值不是硬编码
/// 全量。None 返回空对象。
nlohmann::json presetObject(PresetName preset);

/// @brief 生效配置回显：每个键的最终值 + 它来自哪一层。
///
/// @details
/// 排查"到底开了啥"用。59 个开关 + 四层合并 + 联动规则（增强项隐含开基础项），
/// 出错时没有可核对的输出就只能靠猜。
///
/// @param preset       生效的档位（None 表示未给 --preset）
/// @param configRoot   配置文件解析出的对象（未给 --config 时为空对象）
/// @param configPath   配置文件路径（空表示未给 --config）
/// @param options      合并完成后的选项容器（用于 CLI 层取值）
/// @param cliSourceOf  回调：CLI flag → 该键是否由命令行显式给出
/// @param cliValueOf   回调：CLI flag → 命令行给出的原始值（Bool 恒为 "true"）
nlohmann::json buildEffectiveConfigJson(
    PresetName preset, const nlohmann::json& configRoot, const std::string& configPath,
    const CliOptions& options, bool (*cliSourceOf)(const ParamDef& param, void* context),
    std::string (*cliValueOf)(const ParamDef& param, void* context), void* context);

}   // namespace mj2x::cli

// preset_config.cpp
//
// 内置配置档的展开与生效配置回显。见 preset_config.hpp 顶部说明。

#include "preset_config.hpp"

#include <algorithm>

#include "libca/i18n/i18n.hpp"
#include "param_registry.hpp"

namespace mj2x::cli {
namespace {

using json = nlohmann::json;

std::string toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// 默认值文本 → JSON 值。defaultStr 是给 help/schema 展示用的文本，回显需要
/// 按参数类型还原成有类型的值，否则用户看到 "false" 字符串会误判。
json defaultValueJson(const ParamDef& param)
{
    const std::string text = param.defaultStr != nullptr ? param.defaultStr : "";
    switch (param.type) {
    case ParamType::Bool: return text == "true";
    case ParamType::Int:
    case ParamType::UInt64: {
        try {
            return std::stoll(text);
        }
        catch (...) {
            return text;
        }
    }
    case ParamType::StringList: return json::array();
    default: return text;
    }
}

}   // namespace

bool parsePresetName(const std::string& text, PresetName& out)
{
    const std::string lowered = toLower(text);
    if (lowered == "none") {
        out = PresetName::None;
        return true;
    }
    if (lowered == "perf") {
        out = PresetName::Perf;
        return true;
    }
    if (lowered == "balanced") {
        out = PresetName::Balanced;
        return true;
    }
    if (lowered == "max") {
        out = PresetName::Max;
        return true;
    }
    return false;
}

std::string presetNameString(PresetName preset)
{
    switch (preset) {
    case PresetName::None: return "none";
    case PresetName::Perf: return "perf";
    case PresetName::Balanced: return "balanced";
    case PresetName::Max: return "max";
    }
    return "none";
}

const std::vector<std::string>& presetNames()
{
    static const std::vector<std::string> names = {"none", "perf", "balanced", "max"};
    return names;
}

json presetObject(PresetName preset)
{
    json out = json::object();
    switch (preset) {
    case PresetName::None:
        break;
    case PresetName::Perf:
        // 性能优先：把 JNI 边界压到最少，零混淆。
        //
        // direct_calls 是本档的主要收益来源——实测（jvm-obf-tester，7 次取中
        // 位数）把 Inheritance probe 从 12.858ms 降到 2.542ms（5.06x），合计
        // 追平未混淆的 JVM 基线。代价是 #814 的栈观察语义失真，故只在明确
        // 性能优先的档位里开。直调已分档（off/intra-class/all），perf 取全开。
        out["regc_direct_calls"] = "all";
        // 内联默认已开，这里显式钉住：preset 的目的是"不用记默认值"。
        out["regc_no_inline"] = false;
        // 入口做 JNI 表恢复有成本，性能档不付。
        out["regc_safe_env"] = false;
        out["native_opt_level"] = "3";
        out["regc_comment_level"] = "none";
        break;
    case PresetName::Balanced:
        // 常规保护：基础三项 + 零运行时开销的编译期随机化 + 字符串加密。
        out["regc_obfuscate"] = true;
        // state 码随机化是编译期的，运行时代价为零，性价比最高的一项增强。
        out["regc_cff_state_encrypt"] = true;
        out["strings_encrypt"] = true;
        break;
    case PresetName::Max:
        // 最强档：基础三项 + 全部增强 + 两项重型（每次调用/每常量都付代价）。
        out["regc_obfuscate"] = true;
        out["regc_mba_deep"] = true;
        out["regc_opaque_zero"] = true;
        out["regc_cff_state_encrypt"] = true;
        // 死块密度取中等：太高会让产物体积与 -O2 后的分派开销失控。
        out["regc_cff_dead"] = 3;
        // 重型两项（不被 --regc-obfuscate 联动，代价按调用/按常量计）。
        out["regc_hide_constants"] = true;
        out["regc_indirect_calls"] = true;
        out["strings_encrypt"] = true;
        // fake 静默偏离比 terminate 更难受，且不暴露检查点位置；但会真实
        // 拖慢/干扰合法调试，只在本档开。
        out["runtime_protection"] = "fake";
        break;
    }
    return out;
}

json buildEffectiveConfigJson(PresetName preset, const json& configRoot,
                              const std::string& configPath, const CliOptions& options,
                              bool (*cliSourceOf)(const ParamDef& param, void* context),
                              std::string (*cliValueOf)(const ParamDef& param, void* context),
                              void* context)
{
    const json presetJson = presetObject(preset);

    json root       = json::object();
    root["preset"]  = presetNameString(preset);
    root["config"]  = configPath;
    root["layers"]  = json::array({"default", "preset", "config", "cli"});

    json keys = json::object();
    const ParamDef* const paramTable = params();
    const std::size_t     paramTotal = paramCount();
    for (std::size_t i = 0; i < paramTotal; ++i) {
        const ParamDef& param = paramTable[i];
        const std::string key = param.jsonPath;

        // 四层逐层覆盖，最后写的一层即生效层——必须按真实合并顺序，否则
        // "生效配置"会把被覆盖掉的值报成最终结果。
        std::string source = "default";
        json        value  = defaultValueJson(param);
        if (presetJson.contains(key)) {
            source = "preset";
            value  = presetJson.at(key);
        }
        if (configRoot.is_object() && configRoot.contains(key)) {
            source = "config";
            value  = configRoot.at(key);
        }
        if (cliSourceOf != nullptr && cliSourceOf(param, context)) {
            source = "cli";
            const std::string raw = cliValueOf != nullptr ? cliValueOf(param, context) : "";
            if (param.type == ParamType::Bool) {
                value = true;
            }
            else if (param.type == ParamType::Int || param.type == ParamType::UInt64) {
                try {
                    value = std::stoll(raw);
                }
                catch (...) {
                    value = raw;
                }
            }
            else if (param.type == ParamType::StringList) {
                // 列表值无法从单个 raw 还原，保留文本。
                value = raw;
            }
            else {
                value = raw;
            }
        }

        json entry      = json::object();
        entry["value"]  = value;
        entry["source"] = source;
        keys[key]       = std::move(entry);
    }
    root["effective"] = std::move(keys);
    (void)options;
    return root;
}

}   // namespace mj2x::cli

// config_file.cpp
//
// 配置文件加载（注册表驱动）：JSON 对象 → 逐键查注册表 → parseJson 写入
// CliOptions。未知键/类型错误报错定位到键名。合并语义：
// 默认值 < 配置文件 < CLI 参数（CLI 覆盖由 main.cpp 的解析顺序保证）。

#include "config_file.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace mj2x::cli {
namespace {

using json = nlohmann::json;

bool readJsonFile(const std::string& path, json& out, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open config file: " + path;
        return false;
    }
    try {
        input >> out;
    }
    catch (const json::exception& ex) {
        error = std::string("invalid JSON config: ") + ex.what();
        return false;
    }
    if (!out.is_object()) {
        error = "config root must be a JSON object";
        return false;
    }
    return true;
}

}   // namespace

bool loadConfigFile(const std::string& path, CliOptions& options, std::string& error)
{
    json root;
    if (!readJsonFile(path, root, error)) return false;

    for (auto it = root.begin(); it != root.end(); ++it) {
        const ParamDef* param = findParamByJsonPath(it.key());
        if (!param) {
            error = "unknown config key: " + it.key() +
                    " (legal keys: see --dump-config-template)";
            return false;
        }
        if (!param->parseJson(options, it.value(), error)) {
            // parseJson 的错误已含键名；统一加配置文件路径便于定位。
            error = path + ": " + error;
            return false;
        }
    }
    return true;
}

}   // namespace mj2x::cli

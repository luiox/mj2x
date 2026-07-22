#include "config_file.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

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

bool rejectUnknownKeys(const json& root, std::string& error)
{
    const std::unordered_set<std::string> allowed = {
        "input",
        "output",
        "emit_c",
        "emit_cpp",
        "emit_llvm",
        "backend",
        "native_annotation",
        "native_method_prefixes",
        "compile",
        "compile_mode",
        "c_compiler",
        "llvm_tool",
        "generate_xmake",
        "generate_cmake",
        "dump_mir",
    };
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "unknown config key: " + it.key();
            return false;
        }
    }
    return true;
}

bool getString(const json& root, const char* key, std::optional<std::string>& out,
               std::string& error)
{
    auto it = root.find(key);
    if (it == root.end()) return true;
    if (!it->is_string()) {
        error = std::string("config key must be a string: ") + key;
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool getBool(const json& root, const char* key, std::optional<bool>& out, std::string& error)
{
    auto it = root.find(key);
    if (it == root.end()) return true;
    if (!it->is_boolean()) {
        error = std::string("config key must be a boolean: ") + key;
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool getStringArray(const json& root, const char* key, std::vector<std::string>& out, bool& present,
                    std::string& error)
{
    auto it = root.find(key);
    if (it == root.end()) return true;
    if (!it->is_array()) {
        error = std::string("config key must be a string array: ") + key;
        return false;
    }
    out.clear();
    for (const auto& item : *it) {
        if (!item.is_string()) {
            error = std::string("config array contains a non-string item: ") + key;
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    present = true;
    return true;
}

}   // namespace

bool loadConfigFile(const std::string& path, ConfigFileOptions& options, std::string& error)
{
    json root;
    if (!readJsonFile(path, root, error)) return false;
    if (!rejectUnknownKeys(root, error)) return false;

    ConfigFileOptions parsed;
    if (!getString(root, "input", parsed.input, error)) return false;
    if (!getString(root, "output", parsed.output, error)) return false;
    if (!getString(root, "emit_c", parsed.emitC, error)) return false;
    if (!getString(root, "emit_cpp", parsed.emitCpp, error)) return false;
    if (!getString(root, "emit_llvm", parsed.emitLlvm, error)) return false;
    if (!getString(root, "backend", parsed.backend, error)) return false;
    if (!getString(root, "native_annotation", parsed.nativeAnnotation, error)) return false;
    if (!getStringArray(root,
                        "native_method_prefixes",
                        parsed.nativeMethodPrefixes,
                        parsed.hasNativeMethodPrefixes,
                        error))
        return false;
    if (!getBool(root, "compile", parsed.compile, error)) return false;
    if (!getString(root, "compile_mode", parsed.compileMode, error)) return false;
    if (!getString(root, "c_compiler", parsed.cCompiler, error)) return false;
    if (!getString(root, "llvm_tool", parsed.llvmTool, error)) return false;
    if (!getString(root, "generate_xmake", parsed.generateXmake, error)) return false;
    if (!getString(root, "generate_cmake", parsed.generateCmake, error)) return false;
    if (!getBool(root, "dump_mir", parsed.dumpMir, error)) return false;

    options = std::move(parsed);
    return true;
}

}   // namespace mj2x::cli

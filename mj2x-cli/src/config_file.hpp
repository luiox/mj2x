#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mj2x::cli {

struct ConfigFileOptions
{
    std::optional<std::string> input;
    std::optional<std::string> output;
    std::optional<std::string> emitC;
    std::optional<std::string> backend;
    std::optional<std::string> nativeAnnotation;
    std::vector<std::string>   nativeMethodPrefixes;
    bool                       hasNativeMethodPrefixes = false;
    std::optional<bool>        compile;
    std::optional<std::string> compileMode;
    std::optional<std::string> cCompiler;
    std::optional<std::string> generateXmake;
    std::optional<std::string> generateCmake;
    std::optional<bool>        dumpMir;
};

bool loadConfigFile(const std::string& path, ConfigFileOptions& options, std::string& error);

}   // namespace mj2x::cli

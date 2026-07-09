#pragma once

#include <filesystem>
#include <string>

namespace mj2x::cli {

struct XmakeProjectConfig
{
    std::filesystem::path projectDir;
    std::filesystem::path generatedSourcePath;
    std::filesystem::path crtRoot;
    std::string           projectName = "mj2x_native_project";
};

std::filesystem::path findCrtRoot(const char* argv0);

int generateXmakeProject(const XmakeProjectConfig& config);

}   // namespace mj2x::cli

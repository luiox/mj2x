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

struct XmakeBuildConfig
{
    std::filesystem::path projectDir;
    std::filesystem::path outputLibraryPath;
    std::string           mode = "release";
};

std::filesystem::path findCrtRoot(const char* argv0);

int generateXmakeProject(const XmakeProjectConfig& config);
/// @brief LLVM IR 版 xmake 工程：native.ll + xmake.lua（after_build 调 clang）。
int generateLlvmXmakeProject(const std::string& projectDirValue,
                             const std::string& llvmOutputPath);
/// @brief 配置并构建已生成的 xmake 工程，将唯一 release 共享库原子复制到目标路径。
int buildXmakeProject(const XmakeBuildConfig& config);

}   // namespace mj2x::cli

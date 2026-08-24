#pragma once

#include "native_compiler.hpp"

#include <filesystem>
#include <string>

namespace mj2x::cli {

struct XmakeProjectConfig
{
    std::filesystem::path projectDir;
    std::filesystem::path generatedSourcePath;
    std::filesystem::path crtRoot;
    std::string           projectName = "mj2x_native_project";
    /// 随工程分发的 mj2x-rt 挂载子集与后端宏按此选择(embedded_rt.hpp)。
    CEmitterBackend       backend     = CEmitterBackend::StackC;
};

struct XmakeBuildConfig
{
    std::filesystem::path projectDir;
    std::filesystem::path outputLibraryPath;
    std::string           mode = "release";
};

std::filesystem::path findCrtRoot(const char* argv0);

/// @brief 目录是否为合法 mj2x-rt 源包(rt.resources + regc crt 头 + 模板)。
bool isCrtRoot(const std::filesystem::path& root);

/// @brief 把外部磁盘源包按后端分发面复制到 <to>/<挂载>/。
/// 任一所需挂载缺失或复制失败返回 false。
bool copyRtTree(const std::filesystem::path& root, const std::filesystem::path& to,
                CEmitterBackend backend);

int generateXmakeProject(const XmakeProjectConfig& config);
/// @brief LLVM IR 版 xmake 工程：native.ll + xmake.lua（after_build 调 clang）。
/// @param withRuntimeProtection 为 true 时内嵌 mj2x-rt rt_protect 源并参与编译。
int generateLlvmXmakeProject(const std::string& projectDirValue,
                             const std::string& llvmOutputPath, bool withRuntimeProtection);
/// @brief 配置并构建已生成的 xmake 工程，将唯一 release 共享库原子复制到目标路径。
int buildXmakeProject(const XmakeBuildConfig& config);

}   // namespace mj2x::cli

/// @file multi_target.hpp
/// @brief 多目标产物编排（issue #11，--targets）：一次输入 → 多 ABI 原生库 + manifest。
///
/// @details
/// --compile 保持单目标（宿主）语义不变；--targets <abi,...> 开启多目标：
/// 保护管线与 native 源**生成一次**，逐 ABI 编译（产物落
/// `<output 目录>/<abi>/<库名><扩展名>`），并写 manifest
/// `<output 目录>/<stem>.targets.json` 记录每个 ABI 的 built/skipped/failed
/// 与原因——CI 集成的挂点。编译器解析按 issue #11 方案 (a)：显式
/// `--target-cc <abi>=<path>` 优先；宿主 ABI 回落自动探测；其余 ABI 未配置
/// 即 skipped（汇总与 manifest 明确报告，不静默失败）。
///
/// 退出码两态（与 CLI 全局约定一致）：全部 built → 0；任一 skipped/failed → 1，
/// 细节看 manifest 与汇总输出。
#pragma once

#include <string>
#include <vector>

#include "native_compiler.hpp"

namespace mj2x::cli {

/// @brief 逐 ABI 结果（manifest 行）。
struct MultiTargetOutcome
{
    std::string abi;       ///< 目标标识（如 "linux-x64"）
    std::string status;    ///< "built" / "skipped" / "failed"
    std::string path;      ///< built：产物路径
    std::string compiler;  ///< built：编译器可执行名
    std::string reason;    ///< skipped/failed：原因
};

/// @brief 多目标编排输入（源已就绪：文件路径或内存内容二选一）。
struct MultiTargetRequest
{
    std::string inputPath;    ///< 原始保护输入（manifest 记录用）
    std::string outputPath;   ///< --compile 输出路径（库布局与 manifest 锚点）
    std::string backendName;  ///< 后端 CLI 名（manifest 记录用）
    CEmitterBackend   backend           = CEmitterBackend::RegC;
    CCompilerKind     requestedCompiler = CCompilerKind::Auto;
    std::vector<std::string> targets;    ///< ABI 标识列表（调用前已校验）
    std::vector<std::string> targetCc;   ///< "abi=path" 列表（调用前已校验）
    bool        pipeMode  = false;      ///< true = 源经内存管道；false = 文件
    std::string sourcePath;               ///< pipeMode=false：C 源文件路径
    std::string pipeSource;               ///< pipeMode=true：C 源内容
    std::filesystem::path crtRoot;        ///< mj2x-rt 源包根（--compile 契约）
    std::vector<std::string> extraSources;
    ProcessTimeout processTimeout;
};

/// @brief 逐 ABI 编译 + 写 manifest + 汇总打印。
///
/// @param request 编排输入。
/// @return 全部 built 返回 0；任一 skipped/failed 返回 1。
int runMultiTargetCompile(const MultiTargetRequest& request);

}   // namespace mj2x::cli

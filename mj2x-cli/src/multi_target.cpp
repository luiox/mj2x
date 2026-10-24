/// @file multi_target.cpp
/// @brief 多目标产物编排实现（issue #11）：逐 ABI 解析/编译 + manifest（见头文件）。

#include "multi_target.hpp"

#include "libca/log/log_macros.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace mj2x::cli {
namespace {

/// "abi=path" → (abi, path)。main 校验段已保证格式合法，此处重复解析为纯函数。
bool parseTargetCcEntry(const std::string& entry, TargetAbi& abi, std::string& path)
{
    const auto sep = entry.find('=');
    if (sep == std::string::npos) return false;
    if (!parseTargetAbi(entry.substr(0, sep), abi)) return false;
    path = entry.substr(sep + 1);
    return !path.empty();
}

std::filesystem::path multiTargetLibraryPath(const std::string& outputPath,
                                             const TargetAbiInfo& info)
{
    const std::filesystem::path output(outputPath);
    std::filesystem::path dir = output.parent_path();
    if (dir.empty()) dir = ".";
    return dir / info.id / (output.stem().string() + info.librarySuffix);
}

std::filesystem::path manifestPath(const std::string& outputPath)
{
    const std::filesystem::path output(outputPath);
    std::filesystem::path dir = output.parent_path();
    if (dir.empty()) dir = ".";
    return dir / (output.stem().string() + ".targets.json");
}

/// 单目标编译：解析通过后调 compileForTargetImpl（file/pipe 双形态）。
bool compileOneTarget(const MultiTargetRequest& request, TargetAbi abi,
                      const std::string& compilerPath, const std::filesystem::path& libraryPath,
                      std::string& compilerUsed)
{
    CompileTargetSpec spec;
    spec.abi          = abi;
    spec.libraryPath  = libraryPath;
    spec.compilerPath = compilerPath;

    const int rc =
        request.pipeMode
            ? compileCSourceFromPipeForTarget(request.pipeSource, spec, request.backend,
                                              request.requestedCompiler, request.crtRoot,
                                              request.extraSources, request.processTimeout)
            : compileCSourceForTarget(request.sourcePath, spec, request.backend,
                                      request.requestedCompiler, request.crtRoot,
                                      request.extraSources, request.processTimeout);
    if (rc != 0) return false;
    compilerUsed = compilerPath.empty()
                       ? std::string(compilerKindName(request.requestedCompiler))
                       : std::filesystem::path(compilerPath).filename().string();
    if (compilerUsed.empty()) compilerUsed = "auto";
    return true;
}

nlohmann::json buildManifest(const MultiTargetRequest& request,
                             const std::vector<MultiTargetOutcome>& outcomes)
{
    nlohmann::json manifest;
    manifest["schema"]       = 1;
    manifest["input"]        = request.inputPath;
    manifest["output_stem"]  = std::filesystem::path(request.outputPath).stem().string();
    manifest["backend"]      = request.backendName;
    nlohmann::json results   = nlohmann::json::array();
    unsigned       built     = 0;
    unsigned       skipped   = 0;
    unsigned       failed    = 0;
    for (const auto& outcome : outcomes) {
        nlohmann::json entry;
        entry["abi"]    = outcome.abi;
        entry["status"] = outcome.status;
        if (outcome.status == "built") {
            entry["path"]     = outcome.path;
            entry["compiler"] = outcome.compiler;
            ++built;
        }
        else {
            entry["reason"] = outcome.reason;
            if (outcome.status == "skipped") ++skipped;
            else ++failed;
        }
        results.push_back(entry);
    }
    manifest["results"]        = results;
    manifest["summary"]["built"]   = built;
    manifest["summary"]["skipped"] = skipped;
    manifest["summary"]["failed"]  = failed;
    return manifest;
}

}   // namespace

int runMultiTargetCompile(const MultiTargetRequest& request)
{
    std::vector<MultiTargetOutcome> outcomes;

    for (const auto& id : request.targets) {
        TargetAbi abi = TargetAbi::WindowsX64;
        if (!parseTargetAbi(id, abi)) {
            // main 校验段已拦截；防御性跳过而非中断其余目标。
            outcomes.push_back({id, "skipped", {}, {}, "unknown abi id"});
            continue;
        }
        const TargetAbiInfo& info = targetAbiInfo(abi);

        // 显式 --target-cc 优先（issue #11 方案 (a)）。
        std::string compilerPath;
        for (const auto& entry : request.targetCc) {
            TargetAbi entryAbi = TargetAbi::WindowsX64;
            std::string entryPath;
            if (parseTargetCcEntry(entry, entryAbi, entryPath) && entryAbi == abi) {
                compilerPath = entryPath;
                break;
            }
        }

        const std::filesystem::path libraryPath = multiTargetLibraryPath(request.outputPath, info);
        MultiTargetOutcome outcome;
        outcome.abi = info.id;

        if (!compilerPath.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(compilerPath, ec)) {
                outcome.status = "failed";
                outcome.reason = "configured compiler not found: " + compilerPath;
            }
            else {
                std::string compilerUsed;
                if (compileOneTarget(request, abi, compilerPath, libraryPath, compilerUsed)) {
                    outcome.status   = "built";
                    outcome.path     = libraryPath.string();
                    outcome.compiler = compilerUsed;
                }
                else {
                    outcome.status = "failed";
                    outcome.reason = "compilation failed (see CLI output above)";
                }
            }
        }
        else if (abi == hostTargetAbi()) {
            // 宿主 ABI 回落自动探测：探测不到记 skipped（未尝试），探测到但
            // 编译失败记 failed（已尝试）。
            if (!probeHostCompiler(request.requestedCompiler)) {
                outcome.status = "skipped";
                outcome.reason = "no host C compiler found on PATH";
            }
            else {
                std::string compilerUsed;
                if (compileOneTarget(request, abi, {}, libraryPath, compilerUsed)) {
                    outcome.status   = "built";
                    outcome.path     = libraryPath.string();
                    outcome.compiler = compilerUsed;
                }
                else {
                    outcome.status = "failed";
                    outcome.reason = "compilation failed (see CLI output above)";
                }
            }
        }
        else {
            outcome.status = "skipped";
            outcome.reason = std::string("no compiler configured for target ") + info.id
                             + " (pass --target-cc " + info.id
                             + "=<path>; zig is the in-tree cross path)";
        }
        outcomes.push_back(std::move(outcome));
    }

    // manifest 恒写（全失败也写）：CI 集成的挂点。
    const std::filesystem::path manifest = manifestPath(request.outputPath);
    {
        std::error_code ec;
        std::filesystem::create_directories(manifest.parent_path(), ec);
        ec.clear();
        std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
        if (!out) {
            CA_LOGT_ERROR("compile", "cannot write multi-target manifest: {}\n",
                          manifest.string().c_str());
            return 1;
        }
        out << buildManifest(request, outcomes).dump(2) << "\n";
        if (!out) {
            CA_LOGT_ERROR("compile", "cannot write multi-target manifest: {}\n",
                          manifest.string().c_str());
            return 1;
        }
    }

    unsigned built = 0, skipped = 0, failed = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.status == "built") {
            ++built;
            printf("  [built]   %s -> %s (%s)\n", outcome.abi.c_str(), outcome.path.c_str(),
                   outcome.compiler.c_str());
        }
        else if (outcome.status == "skipped") {
            ++skipped;
            printf("  [skipped] %s (%s)\n", outcome.abi.c_str(), outcome.reason.c_str());
        }
        else {
            ++failed;
            printf("  [failed]  %s (%s)\n", outcome.abi.c_str(), outcome.reason.c_str());
        }
    }
    printf("Multi-target summary: built=%u skipped=%u failed=%u manifest=%s\n", built, skipped,
           failed, manifest.string().c_str());

    // 退出码两态（CLI 全局约定）：全部 built → 0；否则 1，细节看 manifest。
    return (failed == 0 && skipped == 0) ? 0 : 1;
}

}   // namespace mj2x::cli

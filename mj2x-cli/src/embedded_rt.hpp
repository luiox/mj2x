#pragma once

// mj2x-rt 源包访问(#891):源包经 rule resources.embed 嵌入 mj2x-cli,
// 生成头自注册后按 bundle(common/regc/stackc/vmc4/llvmir/templates)取用。
// 磁盘源包(findCrtRoot,发布包 share/mj2x-rt)路径仍保留,两条供给
// 路径产出相同树形:<project>/src/mj2x-rt/<挂载名>/<相对路径>。
//
// 分发面单一事实源:各后端只消费自己需要的挂载子集(rtFoldersForBackend),
// 并携带唯一后端宏(rtBackendDefine,如 MJ2X_RT_BACKEND_REGC=1),运行时源
// 与用户 intrinsic 均可据此门控。

#include "resources_mj2x_cli.generated.hpp"

#include "native_compiler.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "libca/log/log_macros.hpp"

namespace mj2x::cli {

/// 取挂载包;未注册返回 nullptr(嵌入缺失时调用方 fail-closed)。
inline const resources::Bundle* rtBundle(std::string_view name)
{
    return resources::bundle(name);
}

/// 读文本资源(模板等);mount 或 path 未命中返回空串。
inline std::string readEmbeddedText(std::string_view mount, std::string_view path)
{
    const auto* bundle = resources::bundle(mount);
    if (bundle == nullptr) return {};
    const auto result = bundle->get(path);
    if (result.is_err()) return {};
    const auto bytes = result.unwrap();
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// mj2x-rt 分发面(单一事实源):后端 → 随生成工程分发/参与编译的挂载集。
/// StackC 仅 stackc 慢路径运行时;RegC 全量 C 运行时;VMC4 仅 vmc4 解释辅助 +
/// common;LLVM 为 llvmir 运行时 + regc intrinsic 实现 + common。common 对
/// RegC/VMC4/LLVM 无条件分发(--runtime-protection 检查点 mj2x_rt_protect 在
/// 此,一个文件对的成本换保护开关不参与分发决策)。
inline std::vector<std::string_view> rtFoldersForBackend(CEmitterBackend backend)
{
    switch (backend) {
    case CEmitterBackend::StackC:
        return {"stackc"};
    case CEmitterBackend::RegC:
        return {"common", "regc"};
    case CEmitterBackend::LlvmIr:
        return {"common", "regc", "llvmir"};
    case CEmitterBackend::Vmc4:
        return {"common", "vmc4"};
    }
    return {};
}

/// 编译 mj2x-rt 源时的后端宏 define 体(如 "MJ2X_RT_BACKEND_REGC=1"),
/// 生成的工程恰好定义一个;分发面与宏一一对应,新增后端两处同步改。
inline std::string_view rtBackendDefine(CEmitterBackend backend)
{
    switch (backend) {
    case CEmitterBackend::StackC: return "MJ2X_RT_BACKEND_STACKC=1";
    case CEmitterBackend::RegC: return "MJ2X_RT_BACKEND_REGC=1";
    case CEmitterBackend::LlvmIr: return "MJ2X_RT_BACKEND_LLVM=1";
    case CEmitterBackend::Vmc4: return "MJ2X_RT_BACKEND_VMC4=1";
    }
    return {};
}

/// 按后端把内嵌运行时源写入 <projectDir>/src/mj2x-rt/<挂载名>/<路径>。
/// 任一所需挂载缺失或写出失败返回 false(调用方 fail-closed)。
inline bool writeRtTree(const std::filesystem::path& projectDir, CEmitterBackend backend)
{
    namespace fs = std::filesystem;
    for (const std::string_view mount : rtFoldersForBackend(backend)) {
        const auto* bundle = resources::bundle(mount);
        if (bundle == nullptr) {
            CA_LOGT_ERROR("compile", "embedded mj2x-rt mount is unavailable: {}\n",
                          std::string{mount}.c_str());
            return false;
        }
        for (const auto& item : *bundle) {
            std::string  relative{item.path.substr(1)};  // 去掉开头 '/'
            const fs::path dest =
                projectDir / "src" / "mj2x-rt" / mount / fs::path{relative};
            std::error_code ec;
            fs::create_directories(dest.parent_path(), ec);
            if (ec) {
                CA_LOGT_ERROR("compile", "cannot create rt directory: {}\n",
                              ec.message().c_str());
                return false;
            }
            std::ofstream out(dest, std::ios::binary);
            if (!out) {
                CA_LOGT_ERROR("compile", "cannot write rt file: {}\n", dest.string().c_str());
                return false;
            }
            out.write(reinterpret_cast<const char*>(item.bytes().data()),
                      static_cast<std::streamsize>(item.bytes().size()));
        }
    }
    return true;
}

}   // namespace mj2x::cli

#pragma once

// mj2x-rt 源包访问(#891):源包经 rule resources.embed 嵌入 mj2x-cli,
// 生成头自注册后按 bundle(common/regc/templates)取用。
// 磁盘源包(findCrtRoot,发布包 share/mj2x-rt)路径仍保留,两条供给
// 路径产出相同树形:<project>/src/mj2x-rt/<挂载名>/<相对路径>。

#include "resources_mj2x_cli.generated.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "libca/log/log_macros.hpp"

namespace mj2x::cli {

/// 源包挂载名(与 mj2x-rt/rt.resources 一致);生成工程内对应
/// src/mj2x-rt/<挂载名>/。templates 仅生成期模板读取用,不随树展开。
constexpr std::string_view kRtSourceMounts[] = {"common", "regc"};

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

/// 把 common+regc 源写入 <projectDir>/src/mj2x-rt/<挂载名>/<路径>。
/// 任一挂载缺失或写出失败返回 false(调用方 fail-closed)。
inline bool writeRtTree(const std::filesystem::path& projectDir)
{
    namespace fs = std::filesystem;
    for (const std::string_view mount : kRtSourceMounts) {
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

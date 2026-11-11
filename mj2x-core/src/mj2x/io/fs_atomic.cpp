#include "mj2x/io/fs_atomic.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

namespace mj2x {

namespace {

/// 进程内单调序号：与 steady_clock 计数拼成临时名 token。clock 计数单独
/// 用在低精度时钟平台存在两次连续调用同 tick 相撞的理论窗口，序号兜底。
unsigned long long nextToken()
{
    static std::atomic<unsigned long long> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return tick ^ (static_cast<unsigned long long>(++sequence) << 32);
}

std::filesystem::path temporaryFor(const std::filesystem::path& dst)
{
    std::filesystem::path temporary = dst;
    temporary += ".mj2x-atomic-" + std::to_string(nextToken());
    return temporary;
}

/// 把临时文件 rename 替换到 dst。Windows 走 MoveFileExW
/// （REPLACE_EXISTING 覆盖既有目标 | WRITE_THROUGH 落盘收尾）；POSIX 上
/// rename 本身原子替换。任何失败先删临时文件（失败静默——此时错误文本
/// 以替换失败为准），保证不留 ".mj2x-atomic-*" 尸体。
ca::core::Result<void, std::string> renameReplaceUnique(const std::filesystem::path& temporary,
                                                        const std::filesystem::path& dst)
{
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), dst.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        return ca::core::Err(std::string("atomic rename failed: ") +
                             std::system_category().message(static_cast<int>(GetLastError())));
    }
#else
    std::error_code renameEc;
    std::filesystem::rename(temporary, dst, renameEc);
    if (renameEc)
    {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        return ca::core::Err(std::string("atomic rename failed: ") + renameEc.message());
    }
#endif
    return ca::core::Ok();
}

}   // namespace

ca::core::Result<void, std::string> writeFileAtomically(const std::filesystem::path& dst,
                                                        const ca::u8* data, ca::usize size)
{
    const std::filesystem::path temporary = temporaryFor(dst);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return ca::core::Err(std::string("cannot open for write: " + temporary.string()));
        if (size != 0)
            out.write(reinterpret_cast<const char*>(data),
                      static_cast<std::streamsize>(size));
        out.close();
        if (!out) return ca::core::Err(std::string("write failed: " + temporary.string()));
    }
    return renameReplaceUnique(temporary, dst);
}

ca::core::Result<void, std::string> copyFileAtomically(const std::filesystem::path& src,
                                                       const std::filesystem::path& dst)
{
    const std::filesystem::path temporary = temporaryFor(dst);
    std::error_code        ec;
    std::filesystem::copy_file(src, temporary, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return ca::core::Err(std::string("copy to temporary failed: ") + ec.message());
    // 非空校验（承 copyLibraryAtomically 语义）：空复制产物 = 源不可交付。
    if (!std::filesystem::is_regular_file(temporary, ec) || std::filesystem::file_size(temporary, ec) == 0)
    {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        return ca::core::Err(std::string("copied file is empty or unreadable: " + temporary.string()));
    }
    return renameReplaceUnique(temporary, dst);
}

}   // namespace mj2x

/// @file        zip_util.cpp
/// @brief       libarchive 解包实现（内容嗅探 + 逐块流式写出 + zip-slip 防线）。
#include "zip_util.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace mj2x {

// ── ZipUtil ──────────────────────────────────────────────────

// 将归档条目解压到 targetDir。
//
// 使用 libarchive 的"自动嗅探"能力：support_filter_all + support_format_all
// 在 archive_read_open_filename 后会按内容（而非后缀）识别压缩与归档格式，
// 因此天然覆盖 zip / tar / tar.xz / tar.gz / tar.zst 等组合，无需手工
// detectFormat（下载后解压场景不信任文件名后缀）。
namespace {

/// 判断 child 是否位于 parent 目录之下（含 parent 自身）。两边都先规范化。
/// 用 lexically_relative 做前缀比较，避免 find("/a/b" in "/a/bc") 这类误命中。
bool isUnderDir(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        c = fs::weakly_canonical(child, ec);
    fs::path        p = fs::weakly_canonical(parent, ec);
    if (ec) return false;
    // lexically_relative：若 c 不在 p 下，结果以 ".." 开头或为空。
    auto rel = c.lexically_relative(p);
    if (rel.empty()) return false;
    for (const auto& seg : rel) {
        if (seg == "..") return false;
    }
    return true;
}

/// 把单个条目写到磁盘（等价于 archive_read_extract，但显式落到 targetDir，
/// 避免依赖进程 CWD）。返回 Ok 表示继续下一条目。
/// 源侧 mbase throw 文本 → 错误码：
/// "entry escapes target dir: <name>" → PathEscape；"cannot open output: <p>" → Io；
/// read_data_block 失败（archive_error_string 文本）→ BadEntry。
ZipResult<void> writeEntryToDisk(struct archive* a, struct archive_entry* entry,
                                 const std::filesystem::path& targetDir)
{
    namespace fs      = std::filesystem;
    const char* pname = archive_entry_pathname(entry);
    if (!pname) return ca::core::Ok();   // 跳过无名条目
    fs::path out = targetDir / pname;

    // 防目录穿越：条目名可能含 ".." 或绝对路径，解析后必须仍在 targetDir 之下。
    if (!isUnderDir(out, targetDir)) return ca::core::Err(ZipError::PathEscape);

    if (archive_entry_filetype(entry) == AE_IFDIR) {
        std::error_code ec;
        fs::create_directories(out, ec);
    }
    else {
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        const void* buff   = nullptr;
        size_t      size   = 0;
        la_int64_t  offset = 0;
        // header 已由外层 read_next_header 读出，这里用 read_data_block 流式写
        // 文件内容（逐块读，不在内存里聚合整个条目）。
        std::ofstream ofs(out, std::ios::binary);
        if (!ofs) return ca::core::Err(ZipError::Io);
        while (true) {
            int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) break;
            if (r != ARCHIVE_OK) return ca::core::Err(ZipError::BadEntry);
            if (size > 0)
                ofs.write(static_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
    }
    // 保持文件权限位（best-effort，失败忽略——CI/解压场景不强求权限还原）。
    std::error_code ign;
    fs::permissions(out, static_cast<fs::perms>(archive_entry_perm(entry)), ign);
    return ca::core::Ok();
}

/// 打开归档并逐条目解压到 target（绝对路径，已 canonical）。onlyEntry 非空时仅
/// 解压 pathname 与之完全匹配的那一个条目，其余跳过；找不到匹配条目报 NotFound。
/// 源侧 mbase throw 文本 → 错误码映射：
/// - "archive_read_new failed"                         → Io
/// - "open archive failed: <p> — <msg>"（文件不存在）   → NotFound
/// - "open archive failed: <p> — <msg>"（格式识别失败） → UnsupportedFormat
/// - "read header failed: <msg>"                       → BadEntry
/// - "extract failed at '<entry>': <err>"              → 随内层（PathEscape/Io/BadEntry）
/// - "entry not found in archive '<p>': <entry>"       → NotFound
ZipResult<void> extractArchiveTo(const std::string&           archivePath,
                                 const std::filesystem::path& target,
                                 const std::string*           onlyEntry)
{
    struct archive* a = archive_read_new();
    if (!a) return ca::core::Err(ZipError::Io);
    // RAII：无论成败都释放 archive 句柄。
    std::unique_ptr<struct archive, decltype(&archive_read_free)> guard(a, archive_read_free);

    archive_read_support_filter_all(a);   // xz / gzip / bzip2 / zstd ...
    archive_read_support_format_all(a);   // zip / tar / ...

    // archivePath 是字节边界上的 std::string（libarchive C API 需要 NUL 结尾的
    // char*；UTF-8 字节直通，Windows 侧 libarchive 内部按 ACP 收窄解释——与
    // 源侧 mjbase 同口径，ASCII 路径恒安全）。
    const int r = archive_read_open_filename(a, archivePath.c_str(), 10240);
    if (r != ARCHIVE_OK) {
        // mbase 两类失败同报 "open archive failed"；此处按文件存在性细分码，
        // 原消息文本（含 archive_error_string）不进错误载荷。存在性检查按模块
        // 口径走 u8path（UTF-8 解释）。
        if (!std::filesystem::exists(std::filesystem::u8path(archivePath)))
            return ca::core::Err(ZipError::NotFound);
        return ca::core::Err(ZipError::UnsupportedFormat);
    }

    // 逐条目读取。read_next_header 正常读完返回 ARCHIVE_EOF（循环自然退出），
    // 只有返回 ARCHIVE_FATAL 等才视为失败——不要用 archive_errno 事后判断，
    // 它是"最近一次错误"，正常读完未必清零，会误报。
    struct archive_entry* entry = nullptr;
    bool                  found = false;
    for (;;) {
        const int hr = archive_read_next_header(a, &entry);
        if (hr == ARCHIVE_EOF) break;
        if (hr != ARCHIVE_OK) return ca::core::Err(ZipError::BadEntry);
        if (onlyEntry) {
            const char* pname = archive_entry_pathname(entry);
            // 未读的条目数据会在下次 read_next_header 时被 libarchive 自动跳过。
            if (!pname || *onlyEntry != pname) continue;
            found = true;
        }
        auto wr = writeEntryToDisk(a, entry, target);
        if (wr.is_err()) {
            // mbase 在此包装 "extract failed at '<entry>': <err>"；条目名不进
            // 错误载荷（enum 码），错误类别由内层给出。
            return wr;
        }
        if (onlyEntry) break;   // 目标条目已提取，无需继续扫描
    }
    if (onlyEntry && !found) return ca::core::Err(ZipError::NotFound);
    return ca::core::Ok();
}

}   // namespace

// 使用 libarchive 解压到目标目录（支持 zip / tar / tar.xz / tar.gz 等）
ZipResult<void> ZipUtil::extract(std::string_view archivePath, std::string_view targetDir)
{
    namespace fs = std::filesystem;
    // mbase 直接 create_directories/weakly_canonical（filesystem_error 抛出）；
    // 此处改 error_code 并归一 Err(Io)。路径按 UTF-8 解释（u8path，与仓内
    // SimpleClassFile* 同款口径）。
    std::error_code ec;
    const fs::path  target = fs::u8path(std::string(targetDir));
    fs::create_directories(target, ec);
    if (ec) return ca::core::Err(ZipError::Io);
    const fs::path canonical = fs::weakly_canonical(target, ec);
    if (ec) return ca::core::Err(ZipError::Io);
    return extractArchiveTo(std::string(archivePath), canonical, nullptr);
}

ZipResult<void> ZipUtil::unzip(std::string_view zipPath, std::string_view targetDir)
{
    return extract(zipPath, targetDir);
}

// 从压缩包 path 中提取名为 file 的单个条目到 destination 目录（保留条目内相对
// 路径结构）。file 需与归档内条目 pathname 完全匹配（如 "com/foo/Bar.class"）。
ZipResult<void> ZipUtil::extractFile(std::string_view path, std::string_view file,
                                     std::string_view destination)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path  dest = fs::u8path(std::string(destination));
    fs::create_directories(dest, ec);
    if (ec) return ca::core::Err(ZipError::Io);
    const fs::path canonical = fs::weakly_canonical(dest, ec);
    if (ec) return ca::core::Err(ZipError::Io);
    const std::string entryName(file);
    return extractArchiveTo(std::string(path), canonical, &entryName);
}

}   // namespace mj2x

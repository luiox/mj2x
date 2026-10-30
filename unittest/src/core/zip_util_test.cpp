// zip_util 单测：zip 抽取（libjzip 写面现场生成 fixture）、tar.gz（ustar 现场搓
// + libca gzip 压缩）、路径逃逸拒绝、不存在文件、extractFile 单条目面。
// fixture 全部现场生成，不提交二进制产物。
#include <gmock/gmock.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "jzip/zip_io.hpp"
#include "libca/core/datatype.hpp"
#include "libca/zip/checksum.hpp"
#include "libca/zip/gzip_writer.hpp"
#include "test_helper.hpp"
#include "mj2x/io/zip_util.hpp"

namespace fs = std::filesystem;

namespace {

// ── 通用小工具 ────────────────────────────────────────────────

std::vector<ca::u8> bytesOf(std::string_view text)
{
    return std::vector<ca::u8>(text.begin(), text.end());
}

void writeBytes(const fs::path& path, const std::vector<ca::u8>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// 测试专属临时目录（test/ 下，幂等重建）。
fs::path freshDir(const std::string& name)
{
    const auto dir = test::getTempOutPath("mj2x_zip_util_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

// ── zip fixture：libjzip 写面（STORED 条目，同 io_test 口径） ──

std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(const std::string&         name,
                                                       const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    ca::zip::Crc32 crc;
    crc.update(data.data(), data.size());
    local->setFileName(name);
    local->setGeneralPurposeBitFlag(0x0800);
    local->setCompressionMethod(0);   // STORED：fixture 生成无需压缩器
    local->setCrc32(crc.value());
    local->setCompressedSize(static_cast<ca::i64>(data.size()));
    local->setUncompressedSize(static_cast<ca::i64>(data.size()));
    local->setFileData(data);
    local->setOffset(0);
    return local;
}

fs::path writeZipFixture(const std::string&                              suffix,
                         const std::vector<std::pair<std::string, std::vector<ca::u8>>>& entries)
{
    jzip::ZipArchive archive;
    for (const auto& [name, data] : entries)
        archive.addPart(makeStoredEntry(name, data));
    const auto bytes = jzip::ZipIO::writeDirect(archive);
    const auto path  = test::getTempOutPath("mj2x_zip_util_" + suffix + ".zip");
    writeBytes(path, bytes);
    return path;
}

// ── tar.gz fixture：ustar 头现场搓 + libca gzip 压缩 ──────────

// 单个 ustar 条目：512 字节头 + 512 对齐数据。字段布局见 POSIX ustar；
// libarchive 对未填的 uname/devmajor 等零字段宽容。
std::vector<ca::u8> tarEntry(const std::string& name, const std::vector<ca::u8>& data)
{
    std::array<char, 512> header{};
    std::snprintf(header.data() + 0, 100, "%s", name.c_str());              // name
    std::snprintf(header.data() + 100, 8, "%07o", 0644);                    // mode
    std::snprintf(header.data() + 108, 8, "%07o", 0);                       // uid
    std::snprintf(header.data() + 116, 8, "%07o", 0);                       // gid
    std::snprintf(header.data() + 124, 12, "%011llo",
                  static_cast<unsigned long long>(data.size()));            // size
    std::snprintf(header.data() + 136, 12, "%011o", 0);                     // mtime
    std::memcpy(header.data() + 148, "        ", 8);                        // chksum（先置空格）
    header[156] = '0';                                                      // typeflag：普通文件
    std::memcpy(header.data() + 257, "ustar", 6);                           // magic + NUL
    std::memcpy(header.data() + 263, "00", 2);                              // version
    unsigned sum = 0;
    for (char b : header)
        sum += static_cast<unsigned char>(b);
    std::snprintf(header.data() + 148, 8, "%06o", sum);                     // 6 位八进制 + NUL + 空格

    std::vector<ca::u8> block(header.begin(), header.end());
    block.insert(block.end(), data.begin(), data.end());
    block.resize(block.size() + (512 - block.size() % 512) % 512, 0);
    return block;
}

fs::path writeTarGzFixture(const std::string& suffix,
                           const std::vector<std::pair<std::string, std::vector<ca::u8>>>& entries)
{
    std::vector<ca::u8> tar;
    for (const auto& [name, data] : entries) {
        const auto block = tarEntry(name, data);
        tar.insert(tar.end(), block.begin(), block.end());
    }
    tar.resize(tar.size() + 1024, 0);   // 结束标记：两个全零块

    auto gz = ca::zip::gzip_compress(tar);
    if (gz.is_err()) {
        ADD_FAILURE() << "gzip_compress failed";
        return {};
    }
    const auto path = test::getTempOutPath("mj2x_zip_util_" + suffix + ".tar.gz");
    writeBytes(path, std::move(gz).unwrap());
    return path;
}

}   // namespace

// ── extract/unzip：zip 整包抽取 ───────────────────────────────

TEST(ZipUtilTest, ExtractZipWritesEntriesAndDirectories)
{
    const auto zipPath = writeZipFixture("basic", {
        {"a.txt", bytesOf("alpha")},
        {"dir/b.txt", bytesOf("beta")},
    });
    const auto outDir = freshDir("out_basic");

    auto result = mj2x::ZipUtil::extract(zipPath.string(), outDir.string());
    ASSERT_TRUE(result.is_ok()) << mj2x::to_string(result.unwrap_err());
    EXPECT_EQ(readText(outDir / "a.txt"), "alpha");
    EXPECT_EQ(readText(outDir / "dir" / "b.txt"), "beta");
}

TEST(ZipUtilTest, UnzipDelegatesToExtract)
{
    const auto zipPath = writeZipFixture("unzip", {{"only.txt", bytesOf("unzip-ok")}});
    const auto outDir  = freshDir("out_unzip");

    auto result = mj2x::ZipUtil::unzip(zipPath.string(), outDir.string());
    ASSERT_TRUE(result.is_ok()) << mj2x::to_string(result.unwrap_err());
    EXPECT_EQ(readText(outDir / "only.txt"), "unzip-ok");
}

// ── extract：tar.gz（内容嗅探面，不看后缀）────────────────────

TEST(ZipUtilTest, ExtractTarGzByContentSniffing)
{
    // 文件名故意不带 .tar.gz 后缀（再命名为 .bin）：验证内容式格式识别。
    auto tarGzPath = writeTarGzFixture("tgz", {{"docs/readme.txt", bytesOf("gz-payload")}});
    const auto renamed = tarGzPath.parent_path() / "mj2x_zip_util_tgz.bin";
    std::error_code ec;
    fs::rename(tarGzPath, renamed, ec);
    ASSERT_FALSE(ec) << "rename failed: " << ec.message();
    const auto outDir = freshDir("out_tgz");

    auto result = mj2x::ZipUtil::extract(renamed.string(), outDir.string());
    ASSERT_TRUE(result.is_ok()) << mj2x::to_string(result.unwrap_err());
    EXPECT_EQ(readText(outDir / "docs" / "readme.txt"), "gz-payload");
}

// ── 路径逃逸防线（zip-slip）───────────────────────────────────

TEST(ZipUtilTest, ExtractRejectsDotDotPathEscape)
{
    const auto zipPath = writeZipFixture("escape", {{"../evil.txt", bytesOf("bad")}});
    const auto outDir  = freshDir("out_escape");
    // 目标目录必须真实存在且不在仓库根的父级有干扰文件。
    fs::create_directories(outDir);

    auto result = mj2x::ZipUtil::extract(zipPath.string(), outDir.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), mj2x::ZipError::PathEscape);
    EXPECT_FALSE(fs::exists(outDir.parent_path() / "evil.txt"));
}

TEST(ZipUtilTest, ExtractRejectsAbsolutePathEntry)
{
    const auto zipPath = writeZipFixture("abs", {{"/abs-evil.txt", bytesOf("bad")}});
    const auto outDir  = freshDir("out_abs");

    auto result = mj2x::ZipUtil::extract(zipPath.string(), outDir.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), mj2x::ZipError::PathEscape);
}

// ── 错误面 ────────────────────────────────────────────────────

TEST(ZipUtilTest, ExtractMissingArchiveReturnsNotFound)
{
    const auto outDir = freshDir("out_missing");
    const auto missing =
        (test::getTempOutPath("mj2x_zip_util_no_such_archive.zip")).string();

    auto result = mj2x::ZipUtil::extract(missing, outDir.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), mj2x::ZipError::NotFound);
}

TEST(ZipUtilTest, ExtractNonArchiveReturnsUnsupportedFormat)
{
    const auto notArchive = test::getTempOutPath("mj2x_zip_util_not_an_archive.bin");
    writeBytes(notArchive, bytesOf("this is not an archive"));
    const auto outDir = freshDir("out_notarchive");

    auto result = mj2x::ZipUtil::extract(notArchive.string(), outDir.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), mj2x::ZipError::UnsupportedFormat);
}

// ── extractFile：单条目抽取 ───────────────────────────────────

TEST(ZipUtilTest, ExtractFilePullsOnlyNamedEntry)
{
    const auto zipPath = writeZipFixture("single", {
        {"com/foo/Keep.class", bytesOf("keep-me")},
        {"com/foo/Skip.class", bytesOf("skip-me")},
    });
    const auto outDir = freshDir("out_single");

    auto result = mj2x::ZipUtil::extractFile(zipPath.string(), "com/foo/Keep.class",
                                             outDir.string());
    ASSERT_TRUE(result.is_ok()) << mj2x::to_string(result.unwrap_err());
    EXPECT_EQ(readText(outDir / "com" / "foo" / "Keep.class"), "keep-me");
    EXPECT_FALSE(fs::exists(outDir / "com" / "foo" / "Skip.class"));
}

TEST(ZipUtilTest, ExtractFileMissingEntryReturnsNotFound)
{
    const auto zipPath = writeZipFixture("single_missing", {{"a.txt", bytesOf("x")}});
    const auto outDir  = freshDir("out_single_missing");

    auto result = mj2x::ZipUtil::extractFile(zipPath.string(), "no/such/entry.txt",
                                             outDir.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), mj2x::ZipError::NotFound);
}

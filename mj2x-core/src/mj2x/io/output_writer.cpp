#include "output_writer.hpp"

#include "libca/zip/checksum.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "libca/log/log_macros.hpp"

#include "mj2x/core/dom_edit.hpp"
#include "mj2x/core/frame_recompute.hpp"
#include "mj2x/core/lca_type_resolver.hpp"

namespace mj2x {

namespace fs = std::filesystem;

struct OutputWriter::Impl
{};

OutputWriter::OutputWriter()
    : impl_(std::make_unique<Impl>())
{}

OutputWriter::~OutputWriter() = default;

/// @brief 类字节输出：DOM writeClass 直出。modified 类先过帧重算垫片
///        （对齐旧 COMPUTE_FRAMES）；未修改类保真直出（round-trip 恒等）。
///        @p lca 非空时帧重算走谱系 LCA 解析器（帧换装），缺省硬编码表。
static std::vector<ca::u8> writeClassBytes(CompilerIndex& index, CompilerClass& cls,
                                           const std::string& name,
                                           LcaTypeResolver* lca = nullptr)
{
    if (cls.dom == nullptr) {
        throw std::runtime_error("output_writer: DOM missing for " + name
                                 + " (批 6d 起类载体生而带 DOM)");
    }
    if (cls.modified) {
        auto session = openDomEdit(cls, name);
        recomputeSessionFrames(name, *session, lca);
        commitDomEdit(cls, name, std::move(session));
    }
    auto written = jbc::core::writeClass(*cls.dom);
    if (written.is_err()) {
        throw std::runtime_error("output_writer: writeClass failed for " + name + ": "
                                 + jbc::core::to_string(std::move(written).unwrap_err()));
    }
    return std::move(written).unwrap();
}

static void writeBytes(const std::string& path, const std::vector<ca::u8>& bytes)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        CA_LOGT_ERROR("io", "cannot write {}\n", path.c_str());
        throw std::runtime_error("cannot open output file: " + path);
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    // 磁盘满/配额/早关句柄等写失败必须上抛：Pipeline::run 据此返回非 0，
    // 否则产物缺失或截断仍报成功（静默失败）。
    if (!ofs.good()) {
        CA_LOGT_ERROR("io", "write failed ({} bytes to {})\n", bytes.size(), path.c_str());
        throw std::runtime_error("short write to output file: " + path);
    }
}

/// @brief STORED 条目直流写出（T1 性能：JAR 输出每条目全量拷贝退役）。
///
/// @details
/// 旧路径 entry.data → LocalFileHeader（jzip setFileData 库内整条拷贝）→
/// ZipArchive 驻留 → writeDirect 全量字节 → writeBytes 落盘，大 jar 内存
/// ~3× 峰值。STORED-only 契约下 zip 布局是确定的：这里直接流式写本地头 +
/// 数据 + 中央目录 + EOCD，jzip 退出输出面（读取侧 ZipReader 不动），
/// 内存降到 ~1×（index 的 entry.data 本体）。确定性保持：mod time/date 恒 0。
/// 越界面 fail-loud（条目数/偏移超 zip32 界即抛，不留静默截断）。
struct StoredJarWriter
{
    explicit StoredJarWriter(const std::string& path) : path_(path), ofs_(path, std::ios::binary)
    {
        if (!ofs_) throw std::runtime_error("cannot open output file: " + path);
    }

    void add(const std::string& name, const std::vector<ca::u8>& data)
    {
        if (entries_ >= 0xFFFE) {
            throw std::runtime_error("jar entry count over zip32 bound: " + name);
        }
        // offset_ 用 64 位累计，越 zip32 界在写头前抛（u32 会静默回绕产出坏包）。
        if (data.size() > 0xFFFFFFFFull ||
            offset_ + 30 + name.size() + data.size() > 0xFFFFFFFFull) {
            throw std::runtime_error("jar entry over zip32 bound: " + name);
        }

        ca::zip::Crc32 crc;
        crc.update(data.data(), data.size());

        const ca::u32 localOffset = static_cast<ca::u32>(offset_);
        std::vector<ca::u8> header;
        header.reserve(30 + name.size());
        putU32(header, 0x04034b50);
        putU16(header, 20);          // version needed
        putU16(header, 0x0800);      // gp flags：UTF-8 名
        putU16(header, 0);           // stored
        putU16(header, 0);           // mod time（确定性）
        putU16(header, 0);           // mod date
        putU32(header, crc.value());
        putU32(header, static_cast<ca::u32>(data.size()));
        putU32(header, static_cast<ca::u32>(data.size()));
        putU16(header, static_cast<ca::u16>(name.size()));
        putU16(header, 0);           // extra len
        header.insert(header.end(), name.begin(), name.end());
        write(header);
        write(data);

        std::vector<ca::u8> cen;
        cen.reserve(46 + name.size());
        putU32(cen, 0x02014b50);
        putU16(cen, 20);             // version made by
        putU16(cen, 20);             // version needed
        putU16(cen, 0x0800);
        putU16(cen, 0);
        putU16(cen, 0);              // mod time
        putU16(cen, 0);              // mod date
        putU32(cen, crc.value());
        putU32(cen, static_cast<ca::u32>(data.size()));
        putU32(cen, static_cast<ca::u32>(data.size()));
        putU16(cen, static_cast<ca::u16>(name.size()));
        putU16(cen, 0);              // extra len
        putU16(cen, 0);              // comment len
        putU16(cen, 0);              // disk start
        putU16(cen, 0);              // internal attrs
        putU32(cen, 0);              // external attrs
        putU32(cen, localOffset);
        cen.insert(cen.end(), name.begin(), name.end());
        central_.insert(central_.end(), cen.begin(), cen.end());
        ++entries_;
    }

    /// 中央目录 + EOCD 收尾；返回产物总字节数（日志口径）。
    ca::usize finish()
    {
        if (offset_ + central_.size() > 0xFFFFFFFFull) {
            throw std::runtime_error("jar central directory over zip32 bound");
        }
        const ca::u32 cdOffset = static_cast<ca::u32>(offset_);
        write(central_);

        std::vector<ca::u8> eocd;
        eocd.reserve(22);
        putU32(eocd, 0x06054b50);
        putU16(eocd, 0);
        putU16(eocd, 0);
        putU16(eocd, static_cast<ca::u16>(entries_));
        putU16(eocd, static_cast<ca::u16>(entries_));
        putU32(eocd, static_cast<ca::u32>(central_.size()));
        putU32(eocd, cdOffset);
        putU16(eocd, 0);
        write(eocd);

        ofs_.flush();
        if (!ofs_.good()) {
            throw std::runtime_error("short write to output jar: " + path_);
        }
        return offset_ + central_.size() + eocd.size();
    }

private:
    static void putU16(std::vector<ca::u8>& out, ca::u16 v)
    {
        out.push_back(static_cast<ca::u8>(v & 0xFF));
        out.push_back(static_cast<ca::u8>((v >> 8) & 0xFF));
    }
    static void putU32(std::vector<ca::u8>& out, ca::u32 v)
    {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<ca::u8>((v >> (8 * i)) & 0xFF));
        }
    }
    void write(const std::vector<ca::u8>& bytes)
    {
        ofs_.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!ofs_.good()) {
            throw std::runtime_error("short write to output jar: " + path_);
        }
        offset_ += static_cast<ca::u32>(bytes.size());
    }

    std::ofstream       ofs_;
    std::string         path_;
    std::vector<ca::u8> central_;
    ca::u64             offset_ = 0;
    ca::u32             entries_ = 0;
};

static void writeJar(const std::string& path, CompilerIndex& index, LcaTypeResolver* lca)
{
    size_t classCount    = 0;
    size_t resourceCount = 0;

    StoredJarWriter jar(path);
    for (const auto& entry : index.jarEntries()) {
        // modified 类走 DOM writeClass（帧垫片后直出）；其余条目引用原数据，
        // 未修改类字节原样保留（保真收益：旧 tree writer 不可复现的
        // verifier 敏感元数据不再失真）。
        const std::vector<ca::u8>* data      = &entry.data;
        std::vector<ca::u8>        rewritten;
        if (entry.isClass) {
            if (auto* cls = index.getClass(entry.className)) {
                if (cls->modified) {
                    rewritten = writeClassBytes(index, *cls, entry.className, lca);
                    data      = &rewritten;
                }
                ++classCount;
            }
        }
        else {
            ++resourceCount;
        }
        jar.add(entry.name, *data);
    }

    const ca::usize total = jar.finish();
    CA_LOGT_INFO("io", "Written JAR: {} ({} bytes, {} classes, {} resources)\n", path.c_str(),
           total,
           classCount,
           resourceCount);
}

static bool isGeneratedRuntimeClass(const std::string& name)
{
    // mj2x/cloader/Loader：RegisterNatives 桥接类。
    // mj2x/lambda/*：invokedynamic lambda 展开合成的 functional interface 实现类（R6.1 阶段2）。
    // 包内 Lambda$N（#1182）：包私有 SAM 场景合成类落 SAM 同包，形态 [pkg/]Lambda$<digits>；
    // 尾段全数字才识别，避免误吞用户手写的 Lambda$X 类。
    // 这些都不是 primary 输入类，且须相对 classpath 根写出（不跟 primary 包名走）。
    if (name == "mj2x/cloader/Loader" || name.rfind("mj2x/lambda/", 0) == 0) return true;
    const auto  slash = name.rfind('/');
    const auto& last  = slash == std::string::npos ? name : name.substr(slash + 1);
    if (last.size() <= 7 || last.rfind("Lambda$", 0) != 0) return false;
    for (const char c : last.substr(7)) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static fs::path sidecarClassPath(const fs::path& outputPath,
                                 const std::string& className,
                                 const std::string& primaryName)
{
    fs::path sidecar = outputPath.parent_path();
    // A single-class output may be written under the primary class package
    // (for example `pkg/A_protected.class`).  Generated runtime classes must
    // remain relative to the classpath root, not relative to `pkg/`.
    for (std::size_t i = 0; i < primaryName.size(); ++i) {
        if (primaryName[i] == '/') sidecar = sidecar.parent_path();
    }
    size_t   start   = 0;
    while (start < className.size()) {
        const auto slash = className.find('/', start);
        if (slash == std::string::npos) {
            sidecar /= className.substr(start) + ".class";
            break;
        }
        sidecar /= className.substr(start, slash - start);
        start = slash + 1;
    }
    return sidecar;
}

static void writeClassFile(const std::string& path, CompilerIndex& index,
                           LcaTypeResolver* lca)
{
    auto&          classes = index.getClasses();
    CompilerClass* primary = nullptr;
    std::string    primaryName;

    for (auto& [name, cls] : classes) {
        if (!isGeneratedRuntimeClass(name)) {
            primary     = &cls;
            primaryName = name;
            break;
        }
    }
    if (primary == nullptr) {
        auto it     = classes.begin();
        primary     = &it->second;
        primaryName = it->first;
    }

    auto bytes = writeClassBytes(index, *primary, primaryName, lca);
    writeBytes(path, bytes);
    CA_LOGT_INFO("io", "Written: {} ({} bytes)\n", path.c_str(), bytes.size());

    const fs::path outputPath(path);
    for (auto& [name, cls] : classes) {
        if (name == primaryName) continue;

        const auto      sidecarPath = sidecarClassPath(outputPath, name, primaryName);
        std::error_code ec;
        fs::create_directories(sidecarPath.parent_path(), ec);
        auto sidecarBytes = writeClassBytes(index, cls, name, lca);
        writeBytes(sidecarPath.string(), sidecarBytes);
        CA_LOGT_INFO("io", "Written sidecar: {} ({} bytes)\n", sidecarPath.string().c_str(),
               sidecarBytes.size());
    }
}

void OutputWriter::write(const std::string& path, CompilerIndex& index, LcaTypeResolver* lca)
{
    auto& classes = index.getClasses();
    if (classes.empty()) {
        CA_LOGT_INFO("io", "No classes to write\n");
        return;
    }

    if (index.isJarInput()) {
        writeJar(path, index, lca);
        return;
    }

    writeClassFile(path, index, lca);
}

}   // namespace mj2x

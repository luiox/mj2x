#include "output_writer.hpp"

#include "jzip/zip_io.hpp"
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
static std::vector<ca::u8> writeClassBytes(CompilerIndex& index, CompilerClass& cls,
                                           const std::string& name)
{
    if (cls.dom == nullptr) {
        throw std::runtime_error("output_writer: DOM missing for " + name
                                 + " (批 6d 起类载体生而带 DOM)");
    }
    if (cls.modified) {
        auto session = openDomEdit(cls, name);
        recomputeSessionFrames(name, *session);
        commitDomEdit(cls, name, std::move(session));
    }
    auto written = jbc::core::writeClass(*cls.dom);
    if (written.is_err()) {
        throw std::runtime_error("output_writer: writeClass failed for " + name + ": "
                                 + jbc::core::to_string(std::move(written).unwrap_err()));
    }
    return std::move(written).unwrap();
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(const std::string&         name,
                                                              const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    ca::zip::Crc32 crc;
    crc.update(data.data(), data.size());

    local->setFileName(name);
    local->setGeneralPurposeBitFlag(0x0800);
    local->setCompressionMethod(0);
    local->setCrc32(crc.value());
    local->setCompressedSize(static_cast<ca::i64>(data.size()));
    local->setUncompressedSize(static_cast<ca::i64>(data.size()));
    local->setFileData(data);
    local->setOffset(0);
    return local;
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

static void writeJar(const std::string& path, CompilerIndex& index)
{
    jzip::ZipArchive archive;
    size_t           classCount    = 0;
    size_t           resourceCount = 0;

    for (const auto& entry : index.jarEntries()) {
        // modified 类走 DOM writeClass（帧垫片后直出）；其余条目引用原数据，
        // 未修改类字节原样保留（保真收益：旧 tree writer 不可复现的
        // verifier 敏感元数据不再失真）。
        const std::vector<ca::u8>* data      = &entry.data;
        std::vector<ca::u8>        rewritten;
        if (entry.isClass) {
            if (auto* cls = index.getClass(entry.className)) {
                if (cls->modified) {
                    rewritten = writeClassBytes(index, *cls, entry.className);
                    data      = &rewritten;
                }
                ++classCount;
            }
        }
        else {
            ++resourceCount;
        }
        archive.addPart(makeStoredEntry(entry.name, *data));
    }

    // MVP JAR output writes STORED entries.  That keeps the implementation
    // deterministic while preserving resource payloads byte-for-byte.
    auto bytes = jzip::ZipIO::writeDirect(archive);
    writeBytes(path, bytes);
    CA_LOGT_INFO("io", "Written JAR: {} ({} bytes, {} classes, {} resources)\n", path.c_str(),
           bytes.size(),
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

static void writeClassFile(const std::string& path, CompilerIndex& index)
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

    auto bytes = writeClassBytes(index, *primary, primaryName);
    writeBytes(path, bytes);
    CA_LOGT_INFO("io", "Written: {} ({} bytes)\n", path.c_str(), bytes.size());

    const fs::path outputPath(path);
    for (auto& [name, cls] : classes) {
        if (name == primaryName) continue;

        const auto      sidecarPath = sidecarClassPath(outputPath, name, primaryName);
        std::error_code ec;
        fs::create_directories(sidecarPath.parent_path(), ec);
        auto sidecarBytes = writeClassBytes(index, cls, name);
        writeBytes(sidecarPath.string(), sidecarBytes);
        CA_LOGT_INFO("io", "Written sidecar: {} ({} bytes)\n", sidecarPath.string().c_str(),
               sidecarBytes.size());
    }
}

void OutputWriter::write(const std::string& path, CompilerIndex& index)
{
    auto& classes = index.getClasses();
    if (classes.empty()) {
        CA_LOGT_INFO("io", "No classes to write\n");
        return;
    }

    if (index.isJarInput()) {
        writeJar(path, index);
        return;
    }

    writeClassFile(path, index);
}

}   // namespace mj2x

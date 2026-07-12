#include "output_writer.hpp"

#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

namespace fs = std::filesystem;

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

struct OutputWriter::Impl
{};

OutputWriter::OutputWriter()
    : impl_(std::make_unique<Impl>())
{}

OutputWriter::~OutputWriter() = default;

static std::vector<ca::u8> writeClassBytes(CompilerIndex& index, CompilerClass& cls)
{
    const ca::i32 flags = cls.modified ? ClassWriter::COMPUTE_FRAMES : ClassWriter::COMPUTE_MAXS;
    ClassWriter   writer(index.arena(), flags);
    cls.classNode->accept(&writer);
    return writer.toByteArray();
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(const std::string&         name,
                                                              const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    zip::Crc32 crc;
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
        printf("[OutputWriter] ERROR: cannot write %s\n", path.c_str());
        return;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

static void writeJar(const std::string& path, CompilerIndex& index)
{
    jzip::ZipArchive archive;
    size_t           classCount    = 0;
    size_t           resourceCount = 0;

    for (const auto& entry : index.jarEntries()) {
        std::vector<ca::u8> data = entry.data;
        if (entry.isClass) {
            // Preserve untouched class bytes. Some real-world classes carry
            // verifier-sensitive StackMapTable/debug metadata that the tree
            // writer cannot yet reproduce byte-for-byte.
            if (auto* cls = index.getClass(entry.className)) {
                if (cls->modified) {
                    data = writeClassBytes(index, *cls);
                }
                ++classCount;
            }
        }
        else {
            ++resourceCount;
        }
        archive.addPart(makeStoredEntry(entry.name, data));
    }

    // MVP JAR output writes STORED entries.  That keeps the implementation
    // deterministic while preserving resource payloads byte-for-byte.
    auto bytes = jzip::ZipIO::writeDirect(archive);
    writeBytes(path, bytes);
    printf("[OutputWriter] Written JAR: %s (%zu bytes, %zu classes, %zu resources)\n",
           path.c_str(),
           bytes.size(),
           classCount,
           resourceCount);
}

static bool isGeneratedRuntimeClass(const std::string& name)
{
    return name == "mj2x/cloader/Loader";
}

static fs::path sidecarClassPath(const fs::path& outputPath, const std::string& className)
{
    fs::path sidecar = outputPath.parent_path();
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

    auto bytes = writeClassBytes(index, *primary);
    writeBytes(path, bytes);
    printf("[OutputWriter] Written: %s (%zu bytes)\n", path.c_str(), bytes.size());

    const fs::path outputPath(path);
    for (auto& [name, cls] : classes) {
        if (name == primaryName) continue;

        const auto      sidecarPath = sidecarClassPath(outputPath, name);
        std::error_code ec;
        fs::create_directories(sidecarPath.parent_path(), ec);
        auto sidecarBytes = writeClassBytes(index, cls);
        writeBytes(sidecarPath.string(), sidecarBytes);
        printf("[OutputWriter] Written sidecar: %s (%zu bytes)\n",
               sidecarPath.string().c_str(),
               sidecarBytes.size());
    }
}

void OutputWriter::write(const std::string& path, CompilerIndex& index)
{
    auto& classes = index.getClasses();
    if (classes.empty()) {
        printf("[OutputWriter] No classes to write\n");
        return;
    }

    if (index.isJarInput()) {
        writeJar(path, index);
        return;
    }

    writeClassFile(path, index);
}

}   // namespace mj2x

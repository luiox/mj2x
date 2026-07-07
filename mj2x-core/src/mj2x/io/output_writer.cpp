#include "output_writer.hpp"

#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"

#include <cstdio>
#include <fstream>

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

struct OutputWriter::Impl {};

OutputWriter::OutputWriter()
    : impl_(std::make_unique<Impl>())
{}

OutputWriter::~OutputWriter() = default;

static std::vector<ca::u8> writeClassBytes(CompilerIndex& index, CompilerClass& cls) {
    ClassWriter writer(index.arena(), ClassWriter::COMPUTE_MAXS);
    cls.classNode->accept(&writer);
    return writer.toByteArray();
}

static std::unique_ptr<jzip::LocalFileHeader> makeStoredEntry(
    const std::string& name,
    const std::vector<ca::u8>& data) {
    auto local = std::make_unique<jzip::LocalFileHeader>();
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

static void writeBytes(const std::string& path, const std::vector<ca::u8>& bytes) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        printf("[OutputWriter] ERROR: cannot write %s\n", path.c_str());
        return;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

static void writeJar(const std::string& path, CompilerIndex& index) {
    jzip::ZipArchive archive;
    size_t classCount = 0;
    size_t resourceCount = 0;

    for (const auto& entry : index.jarEntries()) {
        std::vector<ca::u8> data = entry.data;
        if (entry.isClass) {
            // Class entries are regenerated from the mutable CompilerClass so
            // native-method injection and backend metadata make it into the JAR.
            if (auto* cls = index.getClass(entry.className)) {
                data = writeClassBytes(index, *cls);
                ++classCount;
            }
        } else {
            ++resourceCount;
        }
        archive.addPart(makeStoredEntry(entry.name, data));
    }

    // MVP JAR output writes STORED entries.  That keeps the implementation
    // deterministic while preserving resource payloads byte-for-byte.
    auto bytes = jzip::ZipIO::writeDirect(archive);
    writeBytes(path, bytes);
    printf("[OutputWriter] Written JAR: %s (%zu bytes, %zu classes, %zu resources)\n",
           path.c_str(), bytes.size(), classCount, resourceCount);
}

void OutputWriter::write(const std::string& path, CompilerIndex& index) {
    auto& classes = index.getClasses();
    if (classes.empty()) {
        printf("[OutputWriter] No classes to write\n");
        return;
    }

    if (index.isJarInput()) {
        writeJar(path, index);
        return;
    }

    for (auto& [name, cls] : classes) {
        auto bytes = writeClassBytes(index, cls);
        writeBytes(path, bytes);

        printf("[OutputWriter] Written: %s (%zu bytes)\n",
               path.c_str(), bytes.size());
    }
}

} // namespace mj2x


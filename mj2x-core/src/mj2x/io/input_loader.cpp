#include "input_loader.hpp"

#include "jzip/memory_segment.hpp"
#include "jzip/zip_io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/log/log_macros.hpp"

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

struct InputLoader::Impl
{};

InputLoader::InputLoader()
    : impl_(std::make_unique<Impl>())
{}

InputLoader::~InputLoader() = default;

static std::vector<ca::u8> readFileBytes(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) throw std::runtime_error("Cannot open: " + path);
    auto                size = ifs.tellg();
    std::vector<ca::u8> bytes(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

static bool hasExtension(const std::string& path, const std::string& ext)
{
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return suffix == ext;
}

static CompilerClass parseClass(CompilerIndex& index, const std::vector<ca::u8>& bytes)
{
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<ca::i32>::max())) {
        throw std::runtime_error("class file too large");
    }
    // 批 6d 树退役：readClass 是唯一解析通道（DOM 权威），元数据快照随建。
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::runtime_error("readClass failed: "
                                 + std::string(jbc::core::to_string(read.unwrap_err())));
    }

    CompilerClass cls;
    cls.dom      = std::move(read).unwrap().file;
    cls.modified = false;
    refreshMethodSnapshots(cls);
    return cls;
}

static void addClassToIndex(CompilerIndex& index, CompilerClass cls)
{
    const std::string className(cls.dom->thisName());
    const auto numMethods = cls.methods.size();
    const auto numFields  = cls.fields.size();

    index.addClass(className, std::move(cls));

    CA_LOGT_INFO("io", "Loaded: {} ({} methods, {} fields)\n", className.c_str(),
           numMethods,
           numFields);
}

static bool isClassEntry(const std::string& name)
{
    return hasExtension(name, ".class") && name.find("META-INF/versions/") != 0;
}

static std::string upperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

static bool isSignedJarMetadata(const std::string& name)
{
    auto upper = upperAscii(name);
    if (upper.find("META-INF/") != 0) return false;
    return hasExtension(upper, ".sf") || hasExtension(upper, ".rsa") ||
           hasExtension(upper, ".dsa") || hasExtension(upper, ".ec");
}

std::unique_ptr<CompilerIndex> InputLoader::load(const std::string& path)
{
    auto bytes = readFileBytes(path);
    auto index = std::make_unique<CompilerIndex>();

    if (hasExtension(path, ".jar")) {
        index->setJarInput(true);
        auto   archive       = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
        size_t classCount    = 0;
        size_t resourceCount = 0;

        // Preserve the original local-file order so OutputWriter can rebuild a
        // whole JAR instead of emitting only the classes mj2x touched.
        for (const auto* local : archive.getLocalFiles()) {
            const auto& entryName = local->getFileName();
            auto        data      = jzip::ZipIO::decompressLocalFile(*local);

            // Rewriting class bytes invalidates existing JAR signatures.  The
            // MVP fails closed instead of producing a misleading signed output.
            if (isSignedJarMetadata(entryName)) {
                throw std::runtime_error("signed JAR metadata is not supported in MVP output: " +
                                         entryName);
            }

            JarEntryRecord entry;
            entry.name    = entryName;
            entry.isClass = isClassEntry(entryName);

            if (entry.isClass) {
                // The class name comes from the bytecode, not the ZIP path; this
                // keeps output replacement correct even for odd but valid paths.
                auto cls        = parseClass(*index, data);
                entry.className = std::string(cls.dom->thisName());
                addClassToIndex(*index, std::move(cls));
                ++classCount;
            }
            else {
                ++resourceCount;
            }
            // 解压缓冲在解析后移入记录，避免整条目拷贝。
            entry.data = std::move(data);
            index->addJarEntry(std::move(entry));
        }

        if (classCount == 0) {
            throw std::runtime_error("JAR contains no root class entries");
        }

        CA_LOGT_INFO("io", "Loaded JAR: {} ({} classes, {} resources)\n", path.c_str(),
               classCount,
               resourceCount);
        return index;
    }

    auto cls = parseClass(*index, bytes);
    addClassToIndex(*index, std::move(cls));
    return index;
}

}   // namespace mj2x

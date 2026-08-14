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

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

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
    // ClassReader 需要 arena&，arena 归 CompilerIndex 持有，比 reader 活得久。
    ClassReader reader(index.arena(), bytes.data(), static_cast<ca::i32>(bytes.size()));
    auto        classNode = std::make_unique<ClassNode>();
    reader.accept(classNode.get(), 0);

    CompilerClass cls;
    cls.classNode = std::move(classNode);
    cls.modified  = false;

    for (auto* mn : cls.classNode->methods) {
        CompilerMethod cm;
        cm.methodNode = mn;
        cls.methods.push_back(std::move(cm));
    }
    for (auto* fn : cls.classNode->fields) {
        CompilerField cf;
        cf.fieldNode = fn;
        cls.fields.push_back(std::move(cf));
    }
    return cls;
}

static void addClassToIndex(CompilerIndex& index, CompilerClass cls)
{
    auto className  = cls.classNode->name.to_std_string();
    auto numMethods = cls.classNode->methods.size();
    auto numFields  = cls.classNode->fields.size();

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
            entry.data    = data;
            entry.isClass = isClassEntry(entryName);

            if (entry.isClass) {
                // The class name comes from the bytecode, not the ZIP path; this
                // keeps output replacement correct even for odd but valid paths.
                auto cls        = parseClass(*index, data);
                entry.className = cls.classNode->name.to_std_string();
                addClassToIndex(*index, std::move(cls));
                ++classCount;
            }
            else {
                ++resourceCount;
            }

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

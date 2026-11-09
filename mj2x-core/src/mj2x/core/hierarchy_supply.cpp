#include "mj2x/core/hierarchy_supply.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <functional>
#include <unordered_set>

#include "jbc/core/reader.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "jzip/memory_segment.hpp"
#include "jzip/zip_io.hpp"

namespace mj2x::hierarchy_supply {

namespace {

using jbc::core::ByteView;
using jbc::core::ReadOptions;
using jbc::core::readClass;

/// class 字节 → 谱系骨架（只读 this/super/interfaces；解析失败返回 false）。
/// preserveSourceBytes=false：只取 summary 且字符串随即拷出，省一份输入等大拷贝。
bool skeletonFromBytes(const std::vector<ca::u8>& bytes, ClassSkeleton& out)
{
    auto result = readClass(ByteView{bytes.data(), bytes.size()},
                            ReadOptions{/*preserveSourceBytes=*/false});
    if (result.is_err()) return false;
    auto read = std::move(result).unwrap();
    if (read.file == nullptr) return false;
    auto file = std::move(read.file);
    out.name = std::string(file->thisName());
    out.superName = std::string(file->superName());
    out.interfaces.clear();
    out.interfaces.reserve(file->interfaces.size());
    for (const ca::u16 itf : file->interfaces) {
        out.interfaces.emplace_back(file->className(itf));
    }
    return !out.name.empty();
}

/// 容器条目 → 类内部名（JDK9+ jmod 的 classes/ 前缀；jar 顶级 .class）。
bool classInternalName(const std::string& entryName, std::string& out)
{
    constexpr std::string_view kPrefix = "classes/";
    if (entryName.size() < 7 || entryName.substr(entryName.size() - 6) != ".class") return false;
    if (entryName.rfind(kPrefix, 0) == 0) {
        out = entryName.substr(kPrefix.size(), entryName.size() - kPrefix.size() - 6);
    }
    else {
        out = entryName.substr(0, entryName.size() - 6);
    }
    return !out.empty() && out.find('/') != std::string::npos;
}

void scanZipBytes(const std::vector<ca::u8>& bytes, std::vector<ClassSkeleton>& out)
{
    // readJvm 对畸形容器抛异常：供给面容错——坏 jar 跳过不致命（谱系图
    // 容许悬挂键回退硬编码表）。
    try {
        auto archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
        for (const auto* local : archive.getLocalFiles()) {
            ClassSkeleton skeleton;
            std::string   internalName;
            if (!classInternalName(local->getFileName(), internalName)) continue;
            auto data = jzip::ZipIO::decompressLocalFile(*local);
            if (!skeletonFromBytes(data, skeleton)) continue;
            out.push_back(std::move(skeleton));
        }
    }
    catch (const std::exception&) {
    }
}

/// JDK8 兜底面：jre/lib/rt.jar（jmods 目录不存在的旧布局）。
std::filesystem::path legacyRtJar(const std::filesystem::path& home)
{
    auto path = home / "jre" / "lib" / "rt.jar";
    if (std::filesystem::exists(path)) return path;
    return home / "lib" / "rt.jar";
}

std::filesystem::path cacheFilePath(const jdk_finder::JavaInfo& info)
{
    const std::string key = info.java_version + "|" + info.runtime_version + "|" + info.vendor +
                            "|" + info.vendor_version;
    const auto h = std::hash<std::string>{}(key);
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    dir /= "mj2x_lca_cache";
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    char name[32];
    std::snprintf(name, sizeof(name), "jdk_%016zx.txt", h);
    return dir / name;
}

bool loadCache(const std::filesystem::path& path, std::vector<ClassSkeleton>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out = deserializeSkeletons(text);
    return !out.empty();
}

void saveCache(const std::filesystem::path& path, const std::vector<ClassSkeleton>& skeletons)
{
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << serializeSkeletons(skeletons);
}

}   // namespace

ca::usize registerSkeleton(mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool,
                           const ClassSkeleton& skeleton)
{
    if (skeleton.name.empty()) return 0;
    if (hierarchy.findClass(skeleton.name).valid()) return 0;   // 重复登记跳过
    mjbase::hierarchy::ClassSummary summary;
    summary.self = mjbase::classKeyOf(pool, skeleton.name);
    if (!skeleton.superName.empty()) {
        summary.super = mjbase::classKeyOf(pool, skeleton.superName);
    }
    summary.interfaces.reserve(skeleton.interfaces.size());
    for (const auto& itf : skeleton.interfaces) {
        summary.interfaces.push_back(mjbase::classKeyOf(pool, itf));
    }
    return hierarchy.addClass(summary).is_ok() ? 1 : 0;
}

ca::i64 scanContainerSkeletons(const std::string& path, std::vector<ClassSkeleton>& out,
                               std::string& note)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        note += " [open failed: " + path + "]";
        return -1;
    }
    std::vector<ca::u8> bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (bytes.size() > 4 && bytes[0] == 'J' && bytes[1] == 'M') {
        bytes.erase(bytes.begin(), bytes.begin() + 4);   // jmod "JM" 前缀 + 版本 2 字节
    }
    const auto before = out.size();
    scanZipBytes(bytes, out);
    return static_cast<ca::i64>(out.size() - before);
}

ca::usize supplyClasspathSkeletons(mjbase::hierarchy::ClassHierarchy& hierarchy,
                                   mjbase::KeyPool& pool, std::string_view classpath,
                                   std::string& note)
{
#if defined(_WIN32)
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    ca::usize                added = 0;
    std::vector<ClassSkeleton> skeletons;
    ca::usize                  begin = 0;
    while (begin <= classpath.size()) {
        const auto end = classpath.find(kSep, begin);
        const auto len = (end == std::string_view::npos ? classpath.size() : end) - begin;
        if (len > 0) {
            const std::string entry(classpath.substr(begin, len));
            if (entry.size() > 4 && entry.substr(entry.size() - 4) == ".jar") {
                skeletons.clear();
                if (scanContainerSkeletons(entry, skeletons, note) > 0) {
                    for (const auto& s : skeletons) added += registerSkeleton(hierarchy, pool, s);
                }
            }
            else if (std::filesystem::is_directory(entry)) {
                for (const auto& item :
                     std::filesystem::recursive_directory_iterator(entry)) {
                    if (!item.is_regular_file()) continue;
                    const auto p = item.path().string();
                    if (p.size() > 6 && p.substr(p.size() - 6) == ".class") {
                        std::ifstream in(p, std::ios::binary);
                        if (!in) continue;
                        std::vector<ca::u8> bytes((std::istreambuf_iterator<char>(in)),
                                                  std::istreambuf_iterator<char>());
                        ClassSkeleton       skeleton;
                        if (skeletonFromBytes(bytes, skeleton)) {
                            added += registerSkeleton(hierarchy, pool, skeleton);
                        }
                    }
                }
            }
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    note += " classpath(+";
    note += std::to_string(added);
    note += ")";
    return added;
}

ca::usize supplyJdkSkeletons(mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool,
                             std::string& note)
{
    const auto jdk = jdk_finder::find_jdk();
    if (!jdk.available) {
        note += " jdk(not found)";
        return 0;
    }
    note += " jdk(";
    note += jdk.java_home.string();
    note += ")";

    std::vector<ClassSkeleton> skeletons;

    // 缓存优先（key = release 标识哈希；info 不可得时 cachePath 为空 → 实扫）。
    std::filesystem::path cachePath;
    if (auto info = jdk_finder::java_info(jdk.java_home); info.is_ok()) {
        cachePath = cacheFilePath(std::move(info).unwrap());
    }
    bool cached = false;
    if (!cachePath.empty() && loadCache(cachePath, skeletons)) {
        cached = true;
        note += " cache(hit)";
    }

    if (!cached) {
        const auto jmodsDir = jdk.java_home / "jmods";
        if (std::filesystem::is_directory(jmodsDir)) {
            for (const auto& item : std::filesystem::directory_iterator(jmodsDir)) {
                if (!item.is_regular_file()) continue;
                if (item.path().extension() != ".jmod") continue;
                scanContainerSkeletons(item.path().string(), skeletons, note);
            }
        }
        else {
            const auto rt = legacyRtJar(jdk.java_home);
            if (std::filesystem::exists(rt)) {
                scanContainerSkeletons(rt.string(), skeletons, note);
            }
            else {
                note += " supply(none)";
            }
        }
        saveCache(cachePath, skeletons);
    }

    ca::usize added = 0;
    for (const auto& s : skeletons) added += registerSkeleton(hierarchy, pool, s);
    note += "(+";
    note += std::to_string(added);
    note += ")";
    return added;
}

std::string serializeSkeletons(const std::vector<ClassSkeleton>& skeletons)
{
    std::string text;
    text.reserve(skeletons.size() * 48);
    for (const auto& s : skeletons) {
        text += s.name;
        text += '\t';
        text += s.superName;
        for (const auto& itf : s.interfaces) {
            text += '\t';
            text += itf;
        }
        text += '\n';
    }
    return text;
}

std::vector<ClassSkeleton> deserializeSkeletons(std::string_view text)
{
    std::vector<ClassSkeleton> out;
    ca::usize                  begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        if (end == std::string_view::npos) break;
        const auto          line = text.substr(begin, end - begin);
        ClassSkeleton       skeleton;
        ca::usize           field = 0;
        ca::usize           fbegin = 0;
        while (fbegin <= line.size()) {
            const auto fend = line.find('\t', fbegin);
            const auto flen = (fend == std::string_view::npos ? line.size() : fend) - fbegin;
            const std::string value(line.substr(fbegin, flen));
            if (field == 0) skeleton.name = value;
            else if (field == 1) skeleton.superName = value;
            else if (!value.empty()) skeleton.interfaces.push_back(value);
            ++field;
            if (fend == std::string_view::npos) break;
            fbegin = fend + 1;
        }
        if (!skeleton.name.empty()) out.push_back(std::move(skeleton));
        begin = end + 1;
    }
    return out;
}

}   // namespace mj2x::hierarchy_supply

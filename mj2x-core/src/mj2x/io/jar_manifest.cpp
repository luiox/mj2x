#include "mj2x/io/jar_manifest.hpp"

#include <cstring>
#include <string>

namespace mj2x {

namespace {

/// 属性行（含插入时的 CRLF 终止）。
const std::string kAttributeLine = std::string(kEnableNativeAccess) + ": " +
                                   kEnableNativeAccessValue + "\r\n";

/// 全新主段模板（输出 JAR 原本无 manifest 时使用）。
const std::string kFreshManifest = "Manifest-Version: 1.0\r\n" + kAttributeLine + "\r\n";

/// @brief 取 [begin, end) 内下一行的边界。
///
/// @details
/// lineEnd 指向行内容末尾（不含终止符），nextBegin 指向下一行起点。
/// 终止符收 CRLF 与 LF（java.util.jar 读取面两种都解析）。
void nextLine(const std::vector<ca::u8>& bytes, std::size_t begin, std::size_t end,
              std::size_t& lineEnd, std::size_t& nextBegin)
{
    std::size_t i = begin;
    while (i < end && bytes[i] != '\n') ++i;
    lineEnd = i;
    if (i > begin && bytes[i - 1] == '\r') lineEnd = i - 1;
    nextBegin = (i < end) ? i + 1 : end;
}

bool lineStartsWith(const std::vector<ca::u8>& bytes, std::size_t begin, std::size_t end,
                    const char* prefix)
{
    const std::size_t n = std::strlen(prefix);
    if (end - begin < n) return false;
    return std::memcmp(bytes.data() + begin, prefix, n) == 0;
}

/// @brief 主段（第一段，到第一个空行为止）扫描结果。
struct MainSectionScan
{
    bool        declared = false;   ///< 主段已声明 Enable-Native-Access。
    std::size_t insertAt = 0;       ///< 声明行插入偏移（字节）。
};

/// @details
/// 空行即主段终止，其余段（per-entry 段）不扫。首行是 Manifest-Version
/// 时插在其行终止符之后，保持版本行居首的规范形态；否则插在主段开头。
/// 属性名前缀带冒号匹配，避免 "Enable-Native-AccessX" 这类前缀同名误判。
MainSectionScan scanMainSection(const std::vector<ca::u8>& src)
{
    MainSectionScan scan;
    std::size_t     pos       = 0;
    bool            firstLine = true;
    while (pos < src.size()) {
        std::size_t lineEnd = 0;
        std::size_t next    = 0;
        nextLine(src, pos, src.size(), lineEnd, next);
        if (lineEnd == pos) break;   // 空行：主段终止
        if (firstLine) {
            if (lineStartsWith(src, pos, lineEnd, "Manifest-Version:")) {
                scan.insertAt = next;
            }
            firstLine = false;
        }
        if (lineStartsWith(src, pos, lineEnd, kEnableNativeAccess)) {
            // 前缀匹配到属性名，还需确认冒号紧随（无空格的 canonical 形态
            // 与带空格的宽松形态都算声明）。
            const std::size_t nameEnd = pos + std::strlen(kEnableNativeAccess);
            if (nameEnd < lineEnd && (src[nameEnd] == ':' || src[nameEnd] == ' ')) {
                scan.declared = true;
            }
        }
        pos = next;
    }
    return scan;
}

}   // namespace

std::vector<ca::u8> buildNativeAccessManifest(const std::vector<ca::u8>* original)
{
    const std::vector<ca::u8> empty;
    const std::vector<ca::u8>& src = (original != nullptr) ? *original : empty;
    if (src.empty()) {
        return std::vector<ca::u8>(kFreshManifest.begin(), kFreshManifest.end());
    }

    const MainSectionScan scan = scanMainSection(src);
    if (scan.declared) return src;   // 已声明：用户显式取值优先，字节原样保留

    std::vector<ca::u8> out;
    out.reserve(src.size() + kAttributeLine.size() + 2);
    out.insert(out.end(), src.begin(), src.begin() + static_cast<std::ptrdiff_t>(scan.insertAt));
    // 插入点落在未终止行末尾（清单无换行结尾）时先补行终止符，否则属性值
    // 会被并入上一行、声明丢失。
    if (scan.insertAt > 0 && src[scan.insertAt - 1] != '\n') {
        out.push_back('\r');
        out.push_back('\n');
    }
    out.insert(out.end(), kAttributeLine.begin(), kAttributeLine.end());
    out.insert(out.end(), src.begin() + static_cast<std::ptrdiff_t>(scan.insertAt), src.end());
    return out;
}

bool manifestHasNativeAccess(const std::vector<ca::u8>& bytes)
{
    return scanMainSection(bytes).declared;
}

}   // namespace mj2x

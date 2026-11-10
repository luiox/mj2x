#include "mj2x/backend/vmx64/vmx64_postlink.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"

namespace mj2x {

namespace {

constexpr uint16_t kMzSignature      = 0x5A4D;    // "MZ"
constexpr uint32_t kPeSignature      = 0x00004550; // "PE\0\0"
constexpr uint16_t kMachineAmd64     = 0x8664;
constexpr uint16_t kOptionalMagicP32 = 0x020B;    // PE32+
constexpr uint8_t  kRelocDir64       = 10;        // IMAGE_REL_BASED_DIR64
constexpr uint8_t  kRelocAbsolute    = 0;         // IMAGE_REL_BASED_ABSOLUTE（装载器跳过）

constexpr size_t kDoshdrE_lfanewOff = 0x3C;
constexpr size_t kCoffMachineOff    = 0;
constexpr size_t kCoffNumSectionsOff = 2;
constexpr size_t kCoffSizeOptionalOff = 16;
constexpr size_t kCoffHeaderBytes   = 20;
constexpr size_t kOptMagicOff       = 0;
constexpr size_t kSectionEntryBytes = 40;
constexpr size_t kSecNameOff        = 0;
constexpr size_t kSecVirtualAddressOff = 12;
constexpr size_t kSecSizeRawOff     = 16;
constexpr size_t kSecPtrRawOff      = 20;

uint16_t rd16(const std::vector<uint8_t>& img, size_t off)
{
    return static_cast<uint16_t>(img[off] | (img[off + 1] << 8));
}

uint32_t rd32(const std::vector<uint8_t>& img, size_t off)
{
    return static_cast<uint32_t>(rd16(img, off)) |
           (static_cast<uint32_t>(rd16(img, off + 2)) << 16);
}

uint64_t rd64(const std::vector<uint8_t>& img, size_t off)
{
    return static_cast<uint64_t>(rd32(img, off)) |
           (static_cast<uint64_t>(rd32(img, off + 4)) << 32);
}

void wr16(std::vector<uint8_t>& img, size_t off, uint16_t v)
{
    img[off]     = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void wr32(std::vector<uint8_t>& img, size_t off, uint32_t v)
{
    wr16(img, off, static_cast<uint16_t>(v & 0xFFFF));
    wr16(img, off + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

void wr64(std::vector<uint8_t>& img, size_t off, uint64_t v)
{
    wr32(img, off, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    wr32(img, off + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

struct Section
{
    std::string name;
    uint32_t    virtualAddress = 0;
    uint32_t    rawSize        = 0;
    uint32_t    rawPtr         = 0;
};

/// @brief 单表改写（内存镜像上）。@p flipRvas 收集需熄灭的重定位目标 RVA。
///
/// @return 表尾 base qword 的文件偏移（扫描续行跳过表体，密文不再当定位符解析）。
ca::core::Result<size_t, std::string> rewriteTable(std::vector<uint8_t>& img,
                                                   size_t magicOff,
                                                   const Section& host,
                                                   std::set<uint32_t>& flipRvas)
{
    // 布局校验：整表（count 项 + k1 + base）必须完整落在宿主段原始数据内。
    if (magicOff + 8 > img.size()) return ca::core::Err(std::string("magic qword runs past EOF"));
    const size_t countOff   = magicOff + 8;
    const size_t entriesOff = countOff + 8;
    if (countOff + 8 > img.size()) return ca::core::Err(std::string("count qword runs past EOF"));
    const uint64_t count64  = rd64(img, countOff);
    if (count64 == 0 || count64 > 0xFFFF)
        return ca::core::Err(std::string("site count out of range at magic offset " +
                               std::to_string(magicOff)));
    const size_t k1Off   = entriesOff + static_cast<size_t>(count64) * 8;
    const size_t baseOff = k1Off + 8;
    if (baseOff + 8 > static_cast<size_t>(host.rawPtr) + host.rawSize)
        return ca::core::Err(std::string("table layout exceeds host section raw data"));
    const uint64_t k1 = rd64(img, k1Off);
    if (k1 == 0) return ca::core::Err(std::string("k1 qword is zero (already encrypted or corrupt)"));

    // 表项纯异或改写。
    const auto tableRva = host.virtualAddress + static_cast<uint32_t>(entriesOff - host.rawPtr);
    for (uint32_t t = 0; t < static_cast<uint32_t>(count64); ++t) {
        const size_t  entryOff = entriesOff + static_cast<size_t>(t) * 8;
        const uint64_t plain   = rd64(img, entryOff);
        const uint64_t mask    = vmx64Ks64(static_cast<uint64_t>(t), k1);
        wr64(img, entryOff, plain ^ mask);
        flipRvas.insert(tableRva + t * 8);
    }
    // base qword：自引用指针（重定位熄灭后保留链接期首选 VA）。
    flipRvas.insert(host.virtualAddress + static_cast<uint32_t>(baseOff - host.rawPtr));
    // 定位痕迹清零。
    wr64(img, magicOff, 0);
    wr64(img, countOff, 0);
    wr64(img, k1Off, 0);

    return ca::core::Ok(baseOff);
}

/// @brief .reloc 熄灭：@p flipRvas 内的 DIR64 项翻 ABSOLUTE。
///
/// @details
/// 校验先行——每个目标 RVA 必须恰好有一个 DIR64 项；见到同目标的非 DIR64
/// 项或缺失即整体失败（不写半改写产物）。
ca::core::Result<void, std::string> quenchRelocations(std::vector<uint8_t>& img,
                                                       const Section& reloc,
                                                       const std::set<uint32_t>& flipRvas)
{
    std::set<uint32_t> remaining(flipRvas);
    size_t             off = reloc.rawPtr;
    const size_t       end = reloc.rawPtr + reloc.rawSize;
    while (off + 8 <= end) {
        const uint32_t pageRva   = rd32(img, off);
        const uint32_t blockSize = rd32(img, off + 4);
        if (blockSize == 0 && pageRva == 0) break;   // MSVC 链接器的全零终结块
        if (blockSize < 8 || off + blockSize > end)
            return ca::core::Err(std::string("malformed .reloc block at raw offset " +
                                   std::to_string(off)));
        for (uint32_t e = off + 8; e + 2 <= off + blockSize; e += 2) {
            const uint16_t entry   = rd16(img, e);
            const uint8_t  type    = static_cast<uint8_t>(entry >> 12);
            const uint32_t target  = pageRva + static_cast<uint32_t>(entry & 0x0FFF);
            if (type == kRelocAbsolute) continue;
            const auto hit = flipRvas.find(target);
            if (hit == flipRvas.end()) continue;
            if (type != kRelocDir64)
                return ca::core::Err(std::string("unexpected relocation type " +
                                       std::to_string(static_cast<int>(type)) +
                                       " for vmx64 table RVA " + std::to_string(target)));
            wr16(img, e, static_cast<uint16_t>(entry & 0x0FFF));   // type → ABSOLUTE
            remaining.erase(target);   // 按键删：hit 是 flipRvas 的迭代器，跨容器 erase 是 UB
        }
        off += blockSize;
    }
    if (!remaining.empty())
        return ca::core::Err(std::string("missing DIR64 relocation for vmx64 table RVA " +
                               std::to_string(*remaining.begin())));
    return ca::core::Ok();
}

}   // namespace

ca::core::Result<void, std::string> vmx64EncryptSiteTable(const std::string& dllPath)
{
    std::ifstream in(dllPath, std::ios::binary);
    if (!in) return ca::core::Err(std::string("cannot open for read: " + dllPath));
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    in.close();
    if (img.size() < 0x40) return ca::core::Err(std::string("file too small to be a PE"));

    // --- PE 骨架校验（fail-closed，任何不满足即拒绝） ---
    // peOff 指向 PE 签名（4 字节），COFF 头在其后：字段全部相对 coffOff 取。
    if (rd16(img, 0) != kMzSignature) return ca::core::Err(std::string("missing MZ signature"));
    const uint32_t peOff = rd32(img, kDoshdrE_lfanewOff);
    if (static_cast<size_t>(peOff) + 4 + kCoffHeaderBytes > img.size()
        || rd32(img, peOff) != kPeSignature)
        return ca::core::Err(std::string("missing PE signature"));
    const size_t coffOff = static_cast<size_t>(peOff) + 4;
    if (rd16(img, coffOff + kCoffMachineOff) != kMachineAmd64)
        return ca::core::Err(std::string("not an AMD64 PE"));
    const uint16_t numSections = rd16(img, coffOff + kCoffNumSectionsOff);
    const uint16_t sizeOpt     = rd16(img, coffOff + kCoffSizeOptionalOff);
    const size_t   optOff      = coffOff + kCoffHeaderBytes;
    if (sizeOpt < 2) return ca::core::Err(std::string("optional header size too small"));
    if (optOff + sizeOpt > img.size()) return ca::core::Err(std::string("truncated optional header"));
    if (rd16(img, optOff + kOptMagicOff) != kOptionalMagicP32)
        return ca::core::Err(std::string("PE32+ optional header required"));
    const size_t secTab = optOff + sizeOpt;
    if (secTab + static_cast<size_t>(numSections) * kSectionEntryBytes > img.size())
        return ca::core::Err(std::string("truncated section table"));

    Section relocSec;
    std::vector<Section> dataSecs;
    for (uint16_t i = 0; i < numSections; ++i) {
        const size_t base = secTab + static_cast<size_t>(i) * kSectionEntryBytes;
        Section sec;
        sec.name.assign(reinterpret_cast<const char*>(&img[base + kSecNameOff]), 8);
        while (!sec.name.empty() && sec.name.back() == '\0') sec.name.pop_back();
        sec.virtualAddress = rd32(img, base + kSecVirtualAddressOff);
        sec.rawSize        = rd32(img, base + kSecSizeRawOff);
        sec.rawPtr         = rd32(img, base + kSecPtrRawOff);
        if (static_cast<size_t>(sec.rawPtr) + sec.rawSize > img.size())
            return ca::core::Err(std::string("section raw data runs past EOF: " + sec.name));
        if (sec.name == ".reloc") relocSec = sec;
        else dataSecs.push_back(sec);
    }
    if (relocSec.rawSize == 0)
        return ca::core::Err(std::string("no .reloc section (vmx64 artifacts are relocatable PE)"));

    // --- 逐表改写（先全部改内存，校验齐了才落盘） ---
    std::set<uint32_t>  flipRvas;
    size_t              tables = 0;
    for (const auto& sec : dataSecs) {
        if (sec.rawSize < 8) continue;
        for (size_t off = sec.rawPtr;
             off + 8 <= static_cast<size_t>(sec.rawPtr) + sec.rawSize; off += 8) {
            if (rd64(img, off) != kVmx64TableMagic) continue;
            auto rewritten = rewriteTable(img, off, sec, flipRvas);
            if (rewritten.is_err()) return ca::core::Err(std::move(rewritten).unwrap_err());
            ++tables;
            off = std::move(rewritten).unwrap();   // 落到 base：循环 +8 越过表体续扫
        }
    }
    if (tables == 0)
        return ca::core::Err(std::string("no vmx64 site table found (already encrypted, or not "
                               "a vmx64 artifact)"));
    auto quenched = quenchRelocations(img, relocSec, flipRvas);
    if (quenched.is_err()) return ca::core::Err(std::move(quenched).unwrap_err());

    // --- 单次回写（校验全过才触盘；临时文件 + rename 原子替换，中途失败不毁产物） ---
    const std::filesystem::path finalPath(dllPath);
    const std::filesystem::path tmpPath = finalPath.string() + ".mj2x-enc-tmp";
    {
        std::ofstream outF(tmpPath, std::ios::binary | std::ios::trunc);
        if (!outF) return ca::core::Err(std::string("cannot open for write: " + tmpPath.string()));
        outF.write(reinterpret_cast<const char*>(img.data()),
                   static_cast<std::streamsize>(img.size()));
        outF.close();
        if (!outF) return ca::core::Err(std::string("write-back failed: " + tmpPath.string()));
    }
    std::error_code renameEc;
    std::filesystem::rename(tmpPath, finalPath, renameEc);
    if (renameEc)
        return ca::core::Err(std::string("atomic rename failed: ") + renameEc.message());
    return ca::core::Ok();
}

}   // namespace mj2x

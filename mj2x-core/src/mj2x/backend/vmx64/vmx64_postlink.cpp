#include "mj2x/backend/vmx64/vmx64_postlink.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/io/fs_atomic.hpp"

namespace mj2x {

namespace {

// --- PE/COFF 布局速查 -------------------------------------------------------
//
// 本文件不 include 任何 Windows SDK 头，全部字段偏移按 PE spec 固定布局
// 手取（linux CI 也能编）。结构链与字段起点：
//
//   文件偏移 0           DOS header（IMAGE_DOS_HEADER，e_magic = "MZ"）
//   DOS 头 + 0x3C        e_lfanew（u32：PE 签名的文件偏移）
//   e_lfanew             PE 签名 "PE\0\0"（4 字节）
//   签名后               COFF header（20 字节）：
//     COFF + 0             Machine（u16，AMD64 = 0x8664）
//     COFF + 2             NumberOfSections（u16）
//     COFF + 16            SizeOfOptionalHeader（u16）
//   COFF + 20            Optional header（本加密器只认 PE32+，Magic = 0x020B）
//   COFF + 20 + sizeOpt  段表（每项 40 字节）：
//     项 + 0               Name（8 字节 ANSI，NUL 填充）
//     项 + 12              VirtualAddress（u32，段 RVA）
//     项 + 16              SizeOfRawData（u32，文件内字节数）
//     项 + 20              PointerToRawData（u32，原始数据文件偏移）
//
// .reloc 的块链结构（熄灭遍历的对象）：pageRVA(u32) + blockSize(u32) +
// (blockSize - 8) 字节的项数组；每项 2 字节 = type（高 4 位）| 页内偏移
// （低 12 位）。DIR64（type 10）指示装载器把「实际装载基址 - 首选基址」
// 加到目标 qword 上；ABSOLUTE（type 0）是占位项，装载器跳过。
// MSVC link.exe 以一个全零块（pageRVA = blockSize = 0）收尾。

constexpr uint16_t kMzSignature      = 0x5A4D;    // DOS e_magic："MZ"
constexpr uint32_t kPeSignature      = 0x00004550; // PE 签名："PE\0\0"
constexpr uint16_t kMachineAmd64     = 0x8664;    // COFF Machine：AMD64
constexpr uint16_t kOptionalMagicP32 = 0x020B;    // Optional Magic：PE32+
constexpr uint8_t  kRelocDir64       = 10;        // .reloc 项 type：DIR64
constexpr uint8_t  kRelocAbsolute    = 0;         // .reloc 项 type：ABSOLUTE（装载器跳过）

// 字段偏移，均相对所属结构起点（结构链见上方速查）。
constexpr size_t kDoshdrE_lfanewOff = 0x3C;           // DOS header 内：e_lfanew
constexpr size_t kCoffMachineOff    = 0;              // COFF header 内：Machine
constexpr size_t kCoffNumSectionsOff = 2;             // COFF header 内：NumberOfSections
constexpr size_t kCoffSizeOptionalOff = 16;           // COFF header 内：SizeOfOptionalHeader
constexpr size_t kCoffHeaderBytes   = 20;             // COFF header 总长
constexpr size_t kOptMagicOff       = 0;              // Optional header 内：Magic
constexpr size_t kSectionEntryBytes = 40;             // 段表项总长
constexpr size_t kSecNameOff        = 0;              // 段表项内：Name
constexpr size_t kSecVirtualAddressOff = 12;          // 段表项内：VirtualAddress
constexpr size_t kSecSizeRawOff     = 16;             // 段表项内：SizeOfRawData
constexpr size_t kSecPtrRawOff      = 20;             // 段表项内：PointerToRawData

// 小端序字段读取/写入（PE 序数字段一律小端）。off 相对镜像起点；每次读取
// 的越界安全由调用方的 fail-closed 校验先行保证。
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

// 段表项的文件视图子集（本加密器只用到这四个字段）。
struct Section
{
    std::string name;
    uint32_t    virtualAddress = 0;
    uint32_t    rawSize        = 0;
    uint32_t    rawPtr         = 0;
};

/// @brief 对内存镜像上的一个站点表执行三步改写中的第 1、3 步（就地）。
///
/// @p magicOff 指向 magic qword（调用方已核对值等于 kVmx64TableMagic）。
/// 第 2 步（重定位熄灭）统一在 quenchRelocations 做：@p flipRvas 收集需要
/// 熄灭的目标 RVA（n 个表项 + base qword），由调用方汇总后一次性处理
/// .reloc。
///
/// 校验先行，全部发生在改写任何字节之前：
///   - 整表（count 项 + k1 + base）必须完整落在宿主段原始数据内——挡
///     magic 碰撞（任意数据里恰好出现 "VMX64TBL"）与被截断的产物，同时
///     保证后续 rd64 不越界；
///   - 站点数必须在 u16 域（发射器只产 1..0xFFFF 个站点）——0 或越界即
///     「这不是发射器产出的表」；
///   - k1 不得为 0——已加密产物（上一次加密已把 k1 清零）或损坏产物。
///     若放行，掩码退化为固定公开常数 ks64(t, 0)，产出任何人都能解开的
///     「伪加密」。
///
/// @return 表尾 base qword 的文件偏移。调用方把扫描游标落在这里（循环
///         +8 后越过表体续扫）：改写后的表体是密文，可能碰巧含 magic
///         位型，跳过表体避免把密文误当新表解析。
ca::core::Result<size_t, std::string> rewriteTable(std::vector<uint8_t>& img,
                                                   size_t magicOff,
                                                   const Section& host,
                                                   std::set<uint32_t>& flipRvas)
{
    // 布局校验（fail-closed）：magic → count → 表项区 → k1 → base 依序落位。
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

    // --- 第 1 步：表项纯异或 ---
    // 落盘值（链接期形态）= handler 首选 VA - ks64(t)（源级 `dq sym - imm`
    // 的产物）。改写为 entry[t] = 落盘值 XOR ks64(t, k1)。运行时 handler
    // 重算同一 keystream（r11）：先 xor 摘掉掩码回到落盘值，再 add 补回
    // ks64(t) 得 handler 首选 VA——完整推导见 doc:vm-site-table-encryption。
    // 每个被改写的表项 RVA 记入 flipRvas，对应 DIR64 项稍后统一熄灭（第
    // 2 步）。
    const auto tableRva = host.virtualAddress + static_cast<uint32_t>(entriesOff - host.rawPtr);
    for (uint32_t t = 0; t < static_cast<uint32_t>(count64); ++t) {
        const size_t  entryOff = entriesOff + static_cast<size_t>(t) * 8;
        const uint64_t plain   = rd64(img, entryOff);
        const uint64_t mask    = vmx64Ks64(static_cast<uint64_t>(t), k1);
        wr64(img, entryOff, plain ^ mask);
        flipRvas.insert(tableRva + t * 8);
    }
    // base qword（表尾自引用指针，落盘值 = 表首选 VA）同样熄灭其重定位：
    // 首选 VA 保持常驻，运行时 prologue 的 r12 = 首选 VA - 表运行时地址 =
    // -delta，NEXT 用它补回基差。
    flipRvas.insert(host.virtualAddress + static_cast<uint32_t>(baseOff - host.rawPtr));
    // --- 第 3 步：定位痕迹清零 ---
    // magic/站点数/k1 归零：文件里不再有「这里有一张 vmx64 表」的特征字节
    // （表项与 base 保留——它们本身是 dispatch 运行时要用的数据）。magic
    // 缺失同时是「已加密」的标志：二次加密扫描零命中 → fail-closed。
    wr64(img, magicOff, 0);
    wr64(img, countOff, 0);
    wr64(img, k1Off, 0);

    return ca::core::Ok(baseOff);
}

/// @brief 第 2 步「重定位熄灭」：把 @p flipRvas 里各 RVA 的 DIR64 重定位
///        项的 type 字段翻成 ABSOLUTE（0）。
///
/// @details
/// DIR64（type 10）指示装载器把「实际装载基址 - 首选基址」加到目标 qword。
/// 表项熄灭前已是密文——若留着 DIR64，装载器会把基差加进密文，运行时
/// xor/add 无论怎么算都解不回真地址；翻成 ABSOLUTE 后装载器跳过该项，密文
/// 原样驻留内存。base qword 同理：熄灭后保持首选 VA，r12 才等于 -delta。
///
/// 校验先行：每个目标 RVA 必须恰好有一条 DIR64 项；见到同目标的非 DIR64
/// 项或遍历完仍缺失即整体失败（不写半改写产物）。缺失说明产物不是预期的
/// 链接形态（重定位被剥掉、表被改过）——熄灭一半留一半会让装载器改写部分
/// 表项，dispatch 静默走错，必须先拒后改。
///
/// MSVC link.exe 的 .reloc 以全零块（pageRVA = blockSize = 0）收尾——遍历
/// 到它即正常结束，不算畸形。
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
                                   std::to_string(off)));   // 块头至少 8 字节且不得越出段
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

    // --- PE 骨架校验（fail-closed，逐层验完再读下一层） ---
    // 字段偏移见文件顶「PE/COFF 布局速查」。每一道挡的是一类输错文件/坏
    // 产物：不是 PE（无 MZ/PE 签名）、不是 AMD64（Machine 不符——所有字段
    // 偏移按 PE32+ 布局取，不猜）、Optional header 截断或非 PE32+（段表定
    // 位不可信）、段表截断（段字段不可信）。
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

    // 收集段表：.reloc 单独记（第 2 步的熄灭对象在里面），其余段全部进
    // magic 扫描清单。段原始数据越出文件即拒绝——后续一切表内偏移以段界
    // 为界。
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

    // --- 逐表扫描改写（全部发生在内存镜像上；任一表失败即整体作废） ---
    // magic 按 8 字节步扫描全部段的原始数据；命中即改写并把游标推到表尾
    // base qword（+8 后续扫，越过表体密文，见 rewriteTable 返回值说明）。
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

    // --- 单次回写（校验全过才触盘） ---
    // 原子替换写单源（mj2x/io/fs_atomic.hpp）：唯一临时文件 + rename 收尾
    // （Windows 走 MoveFileExW REPLACE_EXISTING|WRITE_THROUGH）。进程在写
    // 一半时死掉或替换失败，磁盘上要么是原产物要么是完整加密产物，不会
    // 出现半加密文件；失败路径同时清理临时文件，不留尸体。
    const std::filesystem::path finalPath(dllPath);
    auto written = writeFileAtomically(finalPath, img.data(), img.size());
    if (written.is_err()) return ca::core::Err(std::move(written).unwrap_err());
    return ca::core::Ok();
}

}   // namespace mj2x

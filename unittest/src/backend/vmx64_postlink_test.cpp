// vmx64 post-link 站点表加密器单元测试：合成最小 AMD64 PE（DOS/PE 头 + 两
// 段 + .reloc），按发射器数据表布局（vmx64_asm_emit.hpp Phase 3 节）铺一个
// 三站点表实例，验证加密器的三步改写与全部 fail-closed 面。纯字节运算，
// linux/windows 双平台 CI 可跑（真实 DLL 的端到端加密由 vmx64 E2E 的
// --compile 路径覆盖）。

#include <gmock/gmock.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_helper.hpp"
#include "libca/random/split_mix64.hpp"
#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"
#include "mj2x/backend/vmx64/vmx64_postlink.hpp"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kImageBase  = 0x140000000ULL;
constexpr uint32_t kDataVa     = 0x1000;
constexpr uint32_t kDataRawPtr = 0x400;
constexpr uint32_t kDataRawSz  = 0x200;
constexpr uint32_t kRelocVa    = 0x2000;
constexpr uint32_t kRelocRawPtr = 0x600;
constexpr uint32_t kRelocRawSz  = 0x20;

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

uint64_t rd64(const std::vector<uint8_t>& img, size_t off)
{
    return static_cast<uint64_t>(img[off]) | (static_cast<uint64_t>(img[off + 1]) << 8) |
           (static_cast<uint64_t>(img[off + 2]) << 16) |
           (static_cast<uint64_t>(img[off + 3]) << 24) |
           (static_cast<uint64_t>(img[off + 4]) << 32) |
           (static_cast<uint64_t>(img[off + 5]) << 40) |
           (static_cast<uint64_t>(img[off + 6]) << 48) |
           (static_cast<uint64_t>(img[off + 7]) << 56);
}

/// 合成最小 PE64：.data（magic/计数/3 表项/k1/base 自引用）+ .reloc
///（4 个 DIR64 项：3 表项 + base）。返回 (镜像, 表项链接值, k1)。
struct SyntheticPe
{
    std::vector<uint8_t>   img;
    std::vector<uint64_t>  linkValues;
    uint64_t               k1 = 0;
    size_t                 entriesOff = 0;
    size_t                 baseOff    = 0;
    uint32_t               tableRva   = 0;
    uint32_t               baseRva    = 0;
};

SyntheticPe makeSyntheticPe()
{
    SyntheticPe sp;
    const uint64_t k1 = ca::random::SplitMix64(0x5EED5EEDULL).next();
    sp.k1             = k1;
    sp.img.assign(0x800, 0);

    // --- DOS 头 ---
    sp.img[0] = 'M';
    sp.img[1] = 'Z';
    wr32(sp.img, 0x3C, 0x40);
    // --- PE 签名 + COFF（2 段，optional 0xF0）---
    wr32(sp.img, 0x40, 0x00004550);
    wr16(sp.img, 0x44, 0x8664);
    wr16(sp.img, 0x46, 2);
    wr16(sp.img, 0x54, 0xF0);
    // --- Optional header PE32+ ---
    wr16(sp.img, 0x58, 0x020B);
    wr64(sp.img, 0x58 + 24, kImageBase);
    // --- 段表 @0x148 ---
    const auto writeSection = [&](size_t off, const char* name, uint32_t va, uint32_t rawSz,
                                  uint32_t rawPtr) {
        std::memcpy(&sp.img[off], name, std::strlen(name));
        wr32(sp.img, off + 12, va);
        wr32(sp.img, off + 16, rawSz);
        wr32(sp.img, off + 20, rawPtr);
    };
    writeSection(0x148, ".data", kDataVa, kDataRawSz, kDataRawPtr);
    writeSection(0x170, ".reloc", kRelocVa, kRelocRawSz, kRelocRawPtr);

    // --- .data：表布局（magic/计数/3 表项/k1/base）---
    const size_t magicOff = kDataRawPtr;
    wr64(sp.img, magicOff, mj2x::kVmx64TableMagic);
    wr64(sp.img, magicOff + 8, 3);
    sp.entriesOff = magicOff + 16;
    // 链接期落盘值 = handler 首选 VA - ks64(t)（`dq sym - imm` 的产物形态）。
    const uint64_t prefHandlers[3] = {0x140001000ULL, 0x140002000ULL, 0x140003000ULL};
    for (uint32_t t = 0; t < 3; ++t) {
        const uint64_t link = prefHandlers[t] - mj2x::vmx64Ks64(t, k1);
        sp.linkValues.push_back(link);
        wr64(sp.img, sp.entriesOff + t * 8, link);
    }
    const size_t k1Off = sp.entriesOff + 24;
    wr64(sp.img, k1Off, k1);
    sp.baseOff = k1Off + 8;
    sp.tableRva = kDataVa + static_cast<uint32_t>(sp.entriesOff - kDataRawPtr);
    sp.baseRva  = kDataVa + static_cast<uint32_t>(sp.baseOff - kDataRawPtr);
    // base qword：自引用指针的链接值 = 表首选 VA。
    wr64(sp.img, sp.baseOff, kImageBase + sp.tableRva);

    // --- .reloc：一个 page 块 4 个 DIR64 项（3 表项 + base），补零对齐 ---
    const size_t block = kRelocRawPtr;
    wr32(sp.img, block, kDataVa);          // pageRVA
    wr32(sp.img, block + 4, 8 + 8 + 2);    // 4 项 + 2 字节补零（块大小 4 对齐）
    const uint32_t offs[4] = {sp.tableRva - kDataVa, sp.tableRva - kDataVa + 8,
                              sp.tableRva - kDataVa + 16, sp.baseRva - kDataVa};
    for (int i = 0; i < 4; ++i)
        wr16(sp.img, block + 8 + i * 2, static_cast<uint16_t>((10 << 12) | offs[i]));
    return sp;
}

bool readFile(const fs::path& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

fs::path caseFile(const std::string& name)
{
    return test::getOutPath("mj2x/vmx64_postlink/" + name + "/.keep").parent_path() / name;
}

}   // namespace

// ===========================================================================
// 正路：三步改写逐项核对（表项异或 / 重定位熄灭 / 痕迹清零）+ 确定性 +
// 幂等护栏（二次加密拒绝）。
// ===========================================================================
TEST(Vmx64PostlinkTest, EncryptsTableAndQuenchesRelocations)
{
    const SyntheticPe sp = makeSyntheticPe();
    const fs::path    file = caseFile("sample.dll");
    ASSERT_TRUE(writeFile(file, sp.img));

    const auto r = mj2x::vmx64EncryptSiteTable(file.string());
    ASSERT_TRUE(r.is_ok()) << r.unwrap_err();

    std::vector<uint8_t> out;
    ASSERT_TRUE(readFile(file, out));

    // 1) 表项 = 链接值 XOR ks64(t, k1)；base 内容不变。
    for (uint32_t t = 0; t < 3; ++t) {
        EXPECT_EQ(rd64(out, sp.entriesOff + t * 8),
                  sp.linkValues[t] ^ mj2x::vmx64Ks64(t, sp.k1))
            << "entry " << t;
    }
    EXPECT_EQ(rd64(out, sp.baseOff), kImageBase + sp.tableRva) << "base qword must survive";
    // 2) 重定位熄灭：4 个 DIR64 项的 type 位清零（offset 保留）。
    const uint32_t offs[4] = {sp.tableRva - kDataVa, sp.tableRva - kDataVa + 8,
                              sp.tableRva - kDataVa + 16, sp.baseRva - kDataVa};
    for (int i = 0; i < 4; ++i) {
        const uint16_t e = static_cast<uint16_t>(out[kRelocRawPtr + 8 + i * 2] |
                                                 (out[kRelocRawPtr + 9 + i * 2] << 8));
        EXPECT_EQ(e & 0x0FFF, offs[i] & 0x0FFF) << "entry " << i;
        EXPECT_EQ(e >> 12, 0) << "entry " << i << " must be ABSOLUTE";
    }
    // 3) 痕迹清零：magic/计数/k1。
    EXPECT_EQ(rd64(out, kDataRawPtr), 0u);
    EXPECT_EQ(rd64(out, kDataRawPtr + 8), 0u);
    EXPECT_EQ(rd64(out, sp.entriesOff + 24), 0u);

    // 4) 确定性：同输入重放逐字节相同。
    const fs::path again = caseFile("again.dll");
    ASSERT_TRUE(writeFile(again, sp.img));
    const auto r2 = mj2x::vmx64EncryptSiteTable(again.string());
    ASSERT_TRUE(r2.is_ok()) << r2.unwrap_err();
    std::vector<uint8_t> out2;
    ASSERT_TRUE(readFile(again, out2));
    EXPECT_EQ(out, out2) << "encryption must be deterministic";

    // 5) 幂等护栏：对已加密产物二次加密 = 定位符缺失 = fail-closed。
    const auto r3 = mj2x::vmx64EncryptSiteTable(file.string());
    EXPECT_TRUE(r3.is_err()) << "double encryption must be rejected";
}

// ===========================================================================
// fail-closed 面：非 PE / 非 AMD64 / 重定位项缺失——全部拒绝且不落半改写。
// ===========================================================================
TEST(Vmx64PostlinkTest, FailClosedOnMalformedInputs)
{
    // 非 MZ。
    {
        const fs::path f = caseFile("notpe.dll");
        std::vector<uint8_t> junk(0x100, 0x41);
        ASSERT_TRUE(writeFile(f, junk));
        const auto r = mj2x::vmx64EncryptSiteTable(f.string());
        EXPECT_TRUE(r.is_err()) << "non-PE input must be rejected";
        std::vector<uint8_t> after;
        ASSERT_TRUE(readFile(f, after));
        EXPECT_EQ(after, junk) << "rejected input must be untouched";
    }
    // 非 AMD64 machine。
    {
        const SyntheticPe sp = makeSyntheticPe();
        auto               img = sp.img;
        wr16(img, 0x44, 0x014C);   // i386
        const fs::path f = caseFile("i386.dll");
        ASSERT_TRUE(writeFile(f, img));
        const auto r = mj2x::vmx64EncryptSiteTable(f.string());
        EXPECT_TRUE(r.is_err()) << "non-AMD64 PE must be rejected";
    }
    // 表项重定位缺失（第 2 个 DIR64 项抹成 ABSOLUTE：高字节的 type 位清零）。
    {
        const SyntheticPe sp = makeSyntheticPe();
        auto               img = sp.img;
        img[kRelocRawPtr + 11] = 0x00;   // entry1 = 0xA018 → 0x0018（type 10 → 0）
        const fs::path f = caseFile("missing_reloc.dll");
        ASSERT_TRUE(writeFile(f, img));
        const auto r = mj2x::vmx64EncryptSiteTable(f.string());
        EXPECT_TRUE(r.is_err()) << "missing table relocation must be rejected";
        std::vector<uint8_t> after;
        ASSERT_TRUE(readFile(f, after));
        EXPECT_EQ(after, img) << "rejected input must be untouched";
    }
    // 计数越界。
    {
        const SyntheticPe sp = makeSyntheticPe();
        auto               img = sp.img;
        wr64(img, kDataRawPtr + 8, 0x10000);
        const fs::path f = caseFile("badcount.dll");
        ASSERT_TRUE(writeFile(f, img));
        const auto r = mj2x::vmx64EncryptSiteTable(f.string());
        EXPECT_TRUE(r.is_err()) << "site count out of u16 domain must be rejected";
    }
}

// vmc4_trace_surface_consistency_test.cpp
//
// #820 步骤 6（对抗闭环）的 mj2x 侧闭环：红队 §4.3「trace-surface 一致性核对」
// 与 §4.1「跨实例分析复用性」在 mjdbg 对抗工具就绪前的自动化替代。
//
// 1. DescribeFileRoundTripsToInstanceState：把 describe() 输出当作红队读到的
//    trace-surface 文件解析，逐项与 VmInstance 产物状态比对——声明的结构
//    必须与产物完全一致（防 describe 与产物漂移；红队观测面不可靠 = 闭环失效）。
// 2. CrossInstanceSemanticMappingIsUnreusable：§4.1 低配量化——实例 A 的语义
//    表对实例 B 的字节归属命中率应接近随机水平（≈1/|支持集|），断言远低于
//    可复用阈值（0.2），证明「实例独立成立」（跨实例复用性 ≈ 0 的判据）。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"

namespace {

using mj2x::VmDispatcherKind;
using mj2x::VmInstance;
using mj2x::VmOpcode;
using mj2x::Vmc4Options;
using mj2x::vmOpcodeName;

/// 与 vmc4_instance.cpp 的 kAllOpcodes 同一集合（受支持语义，含 Halt 哨兵）。
const VmOpcode kSupportedOpcodes[] = {
    VmOpcode::Halt,       VmOpcode::Copy,     VmOpcode::Unary,    VmOpcode::Binary,
    VmOpcode::Compare,    VmOpcode::Convert,  VmOpcode::Jump,     VmOpcode::Branch,
    VmOpcode::Return,     VmOpcode::SuperInc, VmOpcode::SuperDec, VmOpcode::Call,
    VmOpcode::LoadField,  VmOpcode::StoreField, VmOpcode::ArrayLoad, VmOpcode::ArrayStore,
    VmOpcode::Allocate,   VmOpcode::Intrinsic,  VmOpcode::Throw,  VmOpcode::Switch,
};

Vmc4Options makeOpts(uint64_t seed, int alias, int bogus, int drift = 0, int period = 16)
{
    Vmc4Options o;
    o.seed           = seed;
    o.aliasCount     = alias;
    o.bogusHandlers  = bogus;
    o.driftVariants  = drift;
    o.driftPeriod    = period;
    o.dispatcherKind = VmDispatcherKind::SwitchTable;
    return o;
}

/// 从 describe 文本解析 opcodeTable 的每一行。
/// 行格式："    COPY 0x01 0x22 [alt-form]"（名字在前，0x 字节在后，可选标记）。
struct ParsedRow
{
    std::string           name;
    std::vector<uint8_t>  bytes;
    bool                  altForm = false;
};

std::vector<ParsedRow> parseOpcodeTable(const std::string& desc)
{
    std::vector<ParsedRow> rows;
    std::istringstream     in(desc);
    std::string            line;
    bool                   inTable = false;
    while (std::getline(in, line)) {
        if (line.find("opcodeTable:") != std::string::npos) {
            inTable = true;
            continue;
        }
        if (!inTable) continue;
        if (line.find("  ") == 0 && line.find("    ") != 0) break;   // 下一个缩进层级
        std::istringstream ls(line);
        std::string        name;
        if (!(ls >> name)) continue;
        if (name.find("0x") == 0 || name.empty()) continue;          // 非语义行
        ParsedRow row;
        row.name = name;
        std::string tok;
        while (ls >> tok) {
            if (tok.rfind("0x", 0) == 0) {
                row.bytes.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
            }
            else if (tok == "[alt-form]") {
                row.altForm = true;
            }
        }
        if (!row.bytes.empty()) rows.push_back(std::move(row));
    }
    return rows;
}

/// describe 的 handlerOrder 行 → 字节序列。
std::vector<uint8_t> parseHandlerOrder(const std::string& desc)
{
    std::vector<uint8_t> bytes;
    std::istringstream   in(desc);
    std::string          line;
    while (std::getline(in, line)) {
        if (line.find("handlerOrder:") != std::string::npos) {
            std::istringstream ls(line);
            std::string        header;
            ls >> header;
            std::string tok;
            while (ls >> tok) {
                if (tok.rfind("0x", 0) == 0) {
                    bytes.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
                }
            }
            break;
        }
    }
    return bytes;
}

/// describe 单字段值解析（"  aliasCount: 2" → "2"）。
std::string fieldValue(const std::string& desc, const char* field)
{
    const std::string needle = std::string(field) + ": ";
    const std::size_t pos    = desc.find(needle);
    if (pos == std::string::npos) return {};
    const std::size_t start = pos + needle.size();
    const std::size_t end   = desc.find('\n', start);
    return desc.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

TEST(Vmc4TraceSurfaceConsistencyTest, DescribeFileRoundTripsToInstanceState)
{
    // 无 drift 实例：describe 的每项声明必须与实例状态逐字节一致。
    VmInstance inst(makeOpts(0xC0FFEE, 2, 1));

    const std::string desc = inst.describe();
    EXPECT_EQ(fieldValue(desc, "aliasCount"), "2");
    EXPECT_EQ(fieldValue(desc, "bogusHandlers"), "1");
    EXPECT_EQ(fieldValue(desc, "driftVariants"), "0");
    EXPECT_EQ(fieldValue(desc, "driftPeriod"), "16");

    // opcodeTable：每个受支持语义一行，字节集合与实例状态一致，alt-form 标记一致。
    const auto rows = parseOpcodeTable(desc);
    ASSERT_EQ(rows.size(), sizeof(kSupportedOpcodes) / sizeof(kSupportedOpcodes[0]));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const VmOpcode op = kSupportedOpcodes[i];
        SCOPED_TRACE(std::string("semantics ") + vmOpcodeName(op));
        EXPECT_EQ(rows[i].name, vmOpcodeName(op));
        EXPECT_EQ(rows[i].bytes, inst.opcodeBytesFor(op))
            << "trace surface 声明的字节集合与实例产物不一致";
        EXPECT_EQ(rows[i].altForm, inst.usesAltHandlerForm(op))
            << "alt-form 标记与实例产物不一致";
    }

    // handlerOrder：声明的 canonical 字节序列与实例状态一致。
    const auto declaredOrder = parseHandlerOrder(desc);
    ASSERT_EQ(declaredOrder.size(), inst.handlerOrder().size());
    for (std::size_t i = 0; i < inst.handlerOrder().size(); ++i) {
        EXPECT_EQ(declaredOrder[i], inst.opcodeFor(inst.handlerOrder()[i]))
            << "handlerOrder 声明的字节与实例产物不一致";
    }

    // 双射：全部受支持语义的字节集合互不相交且覆盖 [0, K*(1+alias))。
    std::vector<uint8_t> all;
    for (const VmOpcode op : kSupportedOpcodes) {
        const auto& bytes = inst.opcodeBytesFor(op);
        for (uint8_t b : bytes) all.push_back(b);
    }
    EXPECT_EQ(all.size(), inst.aliasCount() == 0
                              ? sizeof(kSupportedOpcodes) / sizeof(kSupportedOpcodes[0])
                              : (sizeof(kSupportedOpcodes) / sizeof(kSupportedOpcodes[0])) *
                                    static_cast<std::size_t>(1 + inst.aliasCount()));
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            EXPECT_NE(all[i], all[j]) << "字节集合非双射（重复字节 0x" << std::hex
                                      << static_cast<unsigned>(all[i]) << "）";
        }
    }
}

TEST(Vmc4TraceSurfaceConsistencyTest, DescribeFileRoundTripsDriftInstance)
{
    // drift 开启：声明字段 + 双射 + 轮转真实性（§4.3 观测面的一部分）。
    VmInstance inst(makeOpts(0x1234, 1, 0, 3, 8));
    EXPECT_TRUE(inst.driftEnabled());

    const std::string desc = inst.describe();
    EXPECT_EQ(fieldValue(desc, "driftVariants"), "3");
    EXPECT_EQ(fieldValue(desc, "driftPeriod"), "8");

    // 每个变体下 unmap 在全部支持语义字节上构成双射（输出 canonical 全排列）：
    // 同一语义集合的字节解出同一 canonical，去重后恰好覆盖 0..K-1。
    const std::size_t kCount = sizeof(kSupportedOpcodes) / sizeof(kSupportedOpcodes[0]);
    for (int d = 0; d < inst.driftVariants(); ++d) {
        std::vector<uint8_t> outputs;
        for (const VmOpcode op : kSupportedOpcodes) {
            for (uint8_t raw : inst.opcodeBytesFor(op)) {
                outputs.push_back(inst.unmapByte(raw, d));
            }
        }
        std::sort(outputs.begin(), outputs.end());
        outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
        ASSERT_EQ(outputs.size(), kCount) << "变体 " << d << " 的 unmap 未覆盖全部 canonical";
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            EXPECT_EQ(outputs[i], static_cast<uint8_t>(i))
                << "变体 " << d << " 的 unmap 输出非 canonical 全排列";
        }
    }

    // 轮转真实发生：同一字节在不同变体下解出不同语义（跨变体有差异）。
    bool rotated = false;
    for (const VmOpcode op : kSupportedOpcodes) {
        for (uint8_t raw : inst.opcodeBytesFor(op)) {
            const uint8_t c0 = inst.unmapByte(raw, 0);
            for (int d = 1; d < inst.driftVariants(); ++d) {
                if (inst.unmapByte(raw, d) != c0) rotated = true;
            }
        }
    }
    EXPECT_TRUE(rotated) << "drift 变体间 unmap 完全相同（轮转未发生）";
}

TEST(Vmc4TraceSurfaceConsistencyTest, CrossInstanceSemanticMappingIsUnreusable)
{
    // §4.1 低配量化：用实例 A 的语义表解析实例 B 的字节——命中率应接近随机
    // 水平（≈1/|支持集|）。跨实例复用性 ≈ 0（实例独立成立）的 mj2x 自验判据。
    VmInstance a(makeOpts(0x1111, 1, 0));
    VmInstance b(makeOpts(0x2222, 1, 0));
    ASSERT_NE(a.instanceSeed(), b.instanceSeed());

    const std::size_t kCount = sizeof(kSupportedOpcodes) / sizeof(kSupportedOpcodes[0]);
    std::size_t       total  = 0;
    std::size_t       match  = 0;
    for (const VmOpcode opB : kSupportedOpcodes) {
        for (uint8_t raw : b.opcodeBytesFor(opB)) {
            ++total;
            // raw 在 A 的语义表中属于哪个语义。
            VmOpcode mapped = VmOpcode::Halt;
            bool     found  = false;
            for (const VmOpcode opA : kSupportedOpcodes) {
                const auto& bytes = a.opcodeBytesFor(opA);
                if (std::find(bytes.begin(), bytes.end(), raw) != bytes.end()) {
                    mapped = opA;
                    found  = true;
                    break;
                }
            }
            ASSERT_TRUE(found) << "字节 0x" << std::hex << static_cast<unsigned>(raw)
                               << " 不在实例 A 的支持集内";
            if (mapped == opB) ++match;
        }
    }

    // 随机水平的期望命中率 ≈ 1/|支持集|（≈5%）；可复用阈值 20% 远高于随机，
    // 断言必须低于它——若接近 100% 则实例独立失效（强度归零判据 2）。
    const double hitRate = static_cast<double>(match) / static_cast<double>(total);
    std::printf("[trace-surface] cross-instance semantic hit rate: %.2f%% "
                "(random expectation ~%.2f%%, reuse threshold 20%%)\n",
                hitRate * 100.0, 100.0 / static_cast<double>(kCount));
    EXPECT_LT(hitRate, 0.20) << "跨实例语义命中率 " << hitRate
                             << "（期望接近随机 " << 1.0 / static_cast<double>(kCount)
                             << "）；跨实例可复用 → 强度归零";
}

}   // namespace

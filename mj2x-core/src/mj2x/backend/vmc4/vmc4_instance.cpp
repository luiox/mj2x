#include "mj2x/backend/vmc4/vmc4_instance.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace mj2x {

namespace {

/// 确定性伪随机流（splitmix64）：同 seed 同序列，无未播种状态。
class SplitMix64
{
public:
    explicit SplitMix64(uint64_t seed)
        : state_(seed)
    {}

    uint64_t next()
    {
        state_ += 0x9E3779B97F4A7C15ull;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// [0, bound) 均匀分布（bound 非 2 幂时轻微偏差可接受——只影响映射的
    /// 形状，不影响语义或可复现性）。
    uint64_t nextBounded(uint64_t bound) { return next() % bound; }

private:
    uint64_t state_;
};

}   // namespace

VmInstance::VmInstance(const Vmc4Options& options, uint64_t instanceSeed)
    : dispatcherKind_(options.dispatcherKind)
    , seed_(options.seed)
    , instanceSeed_(instanceSeed == 0 ? options.seed : instanceSeed)
    , aliasCount_(std::max(0, std::min(options.aliasCount, 3)))
    , bogusHandlers_(std::max(0, std::min(options.bogusHandlers, 4)))
    , driftVariants_(std::max(0, std::min(options.driftVariants, 4)))
    , driftPeriod_(std::max(1, options.driftPeriod))
{
    buildOpcodeBytes();
    buildHandlerOrder();
    buildDriftVariants();
}

std::string VmInstance::instanceTag() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_%016llx",
                  vmDispatcherName(dispatcherKind_),
                  static_cast<unsigned long long>(instanceSeed_));
    return buf;
}

/// 受支持的全部 opcode（含 Halt 哨兵）。与 kSupportedOpcode 保持同一集合。
static const VmOpcode kAllOpcodes[] = {
    VmOpcode::Halt,
    VmOpcode::Copy,
    VmOpcode::Unary,
    VmOpcode::Binary,
    VmOpcode::Compare,
    VmOpcode::Convert,
    VmOpcode::Jump,
    VmOpcode::Branch,
    VmOpcode::Return,
    VmOpcode::SuperInc,
    VmOpcode::SuperDec,
    VmOpcode::Call,
    VmOpcode::LoadField,
    VmOpcode::StoreField,
    VmOpcode::ArrayLoad,
    VmOpcode::ArrayStore,
    VmOpcode::Allocate,
    VmOpcode::Intrinsic,
};

/// 某语义是否属于支持集。
bool isSupportedOpcode(VmOpcode op)
{
    for (const auto& candidate : kAllOpcodes) {
        if (candidate == op) return true;
    }
    return false;
}

std::string VmInstance::describe() const
{
    std::string out;
    out += "VMC4 VmInstance (expected trace surface)\n";
    out += "  dispatcher: " + std::string(vmDispatcherName(dispatcherKind_)) + "\n";
    char seedBuf[32];
    std::snprintf(seedBuf, sizeof(seedBuf), "0x%016llx",
                  static_cast<unsigned long long>(instanceSeed_));
    out += std::string("  instanceSeed: ") + seedBuf + "\n";
    out += "  aliasCount: " + std::to_string(aliasCount_) + "\n";
    out += "  bogusHandlers: " + std::to_string(bogusHandlers_) + "\n";
    out += "  driftVariants: " + std::to_string(driftVariants_) + "\n";
    out += "  driftPeriod: " + std::to_string(driftPeriod_) + "\n";
    out += "  handlerOrder:";
    for (const auto op : handlerOrder_) {
        char b[8];
        std::snprintf(b, sizeof(b), " 0x%02x", static_cast<unsigned>(opcodeFor(op)));
        out += b;
    }
    out += "\n";
    out += "  opcodeTable:\n";
    for (const auto& candidate : kAllOpcodes) {
        char line[64];
        std::snprintf(line, sizeof(line), "    %-11s", vmOpcodeName(candidate));
        out += line;
        for (uint8_t b : opcodeBytes_[static_cast<uint8_t>(candidate)]) {
            char bb[8];
            std::snprintf(bb, sizeof(bb), " 0x%02x", static_cast<unsigned>(b));
            out += bb;
        }
        if (usesAltHandlerForm(candidate)) out += "  [alt-form]";
        out += "\n";
    }
    return out;
}

void VmInstance::buildOpcodeBytes()
{
    // 阶段 1：主字节。把 kAllOpcodes 洗牌后按位置赋给各语义的 canonical。
    // 结果是全集上的双射：每个语义分到唯一主字节（0..kAll-1）。
    std::vector<VmOpcode> shuffled(kAllOpcodes, kAllOpcodes + sizeof(kAllOpcodes) / sizeof(kAllOpcodes[0]));
    SplitMix64 rng(instanceSeed_ ^ 0xD1B54A32D192ED03ull);
    for (size_t i = shuffled.size(); i > 1; --i) {
        const size_t j = static_cast<size_t>(rng.nextBounded(i));
        std::swap(shuffled[i - 1], shuffled[j]);
    }
    std::vector<uint8_t> canonicalByOp(256, 0);
    for (size_t i = 0; i < shuffled.size(); ++i) {
        canonicalByOp[static_cast<uint8_t>(shuffled[i])] = static_cast<uint8_t>(i);
    }

    // 阶段 2：ALT 别名。剩余字节槽 [kAll, kAll*(1+aliasCount)) 洗牌后切成
    // aliasCount 组，每组 kAll 个；组内第 k 个字节分给 canonical 字节值为 k
    // 的语义。保证：全局双射、同语义内字节互不相同、不同语义集合不相交。
    const size_t total = shuffled.size() * static_cast<size_t>(1 + aliasCount_);
    std::vector<uint8_t> aliasBytes;
    aliasBytes.reserve(total - shuffled.size());
    for (size_t i = shuffled.size(); i < total; ++i) aliasBytes.push_back(static_cast<uint8_t>(i));
    SplitMix64 aliasRng(instanceSeed_ ^ 0xE7037ED1A0B428DBull);
    for (size_t i = aliasBytes.size(); i > 1; --i) {
        const size_t j = static_cast<size_t>(aliasRng.nextBounded(i));
        std::swap(aliasBytes[i - 1], aliasBytes[j]);
    }
    std::vector<std::vector<uint8_t>> aliasByOp(256);
    for (size_t group = 0; group < static_cast<size_t>(aliasCount_); ++group) {
        for (size_t k = 0; k < shuffled.size(); ++k) {
            const uint8_t byte = aliasBytes[group * shuffled.size() + k];
            // canonical 字节值为 k 的语义拿到这个别名字节。
            VmOpcode op = VmOpcode::Halt;
            for (size_t i = 0; i < shuffled.size(); ++i) {
                if (canonicalByOp[static_cast<uint8_t>(shuffled[i])] == static_cast<uint8_t>(k)) {
                    op = shuffled[i];
                    break;
                }
            }
            aliasByOp[static_cast<uint8_t>(op)].push_back(byte);
        }
    }

    for (size_t i = 0; i < 256; ++i) opcodeBytes_[i].clear();
    for (const auto& candidate : kAllOpcodes) {
        const size_t idx = static_cast<uint8_t>(candidate);
        opcodeBytes_[idx].push_back(canonicalByOp[idx]);
        for (uint8_t b : aliasByOp[idx]) opcodeBytes_[idx].push_back(b);
    }
}

uint8_t VmInstance::pickOpcodeByte(VmOpcode op, uint64_t nonce) const
{
    const auto& bytes = opcodeBytes_[static_cast<uint8_t>(op)];
    if (bytes.size() == 1) return bytes[0];
    SplitMix64 rng(nonce ^ instanceSeed_);
    return bytes[static_cast<size_t>(rng.nextBounded(bytes.size()))];
}

uint8_t VmInstance::variantPickOpcodeByte(VmOpcode op, uint64_t nonce, int variant) const
{
    if (!driftEnabled() || variant < 0 || variant >= driftVariants_) {
        return pickOpcodeByte(op, nonce);
    }
    // 变体 d 下语义 op 使用「集合索引 variantPerm_[d][canonical(op)]」的字节。
    // 集合索引 = canonical 字节值（0..K-1 双射），先反查对应枚举再取集合。
    const uint8_t c = opcodeFor(op);
    const uint8_t targetIdx = variantPerm_[variant][c];
    VmOpcode      targetOp  = VmOpcode::Halt;
    for (const auto& candidate : kAllOpcodes) {
        if (opcodeFor(candidate) == targetIdx) {
            targetOp = candidate;
            break;
        }
    }
    const auto& bytes = opcodeBytes_[static_cast<uint8_t>(targetOp)];
    if (bytes.size() == 1) return bytes[0];
    SplitMix64 rng(nonce ^ instanceSeed_ ^ 0x4D595DF4D0F33173ull);
    return bytes[static_cast<size_t>(rng.nextBounded(bytes.size()))];
}

uint8_t VmInstance::unmapByte(uint8_t raw, int variant) const
{
    if (!driftEnabled() || variant < 0 || variant >= driftVariants_) {
        // drift 关闭：直接返回（发射器不用 unmap 表）。
        return raw;
    }
    // raw 属于哪个支持语义集合 → 该集合索引 hIdx（= 集合 canonical 字节值）。
    uint8_t h = 0xFF;
    for (const auto& candidate : kAllOpcodes) {
        const auto& bytes = opcodeBytes_[static_cast<uint8_t>(candidate)];
        for (uint8_t b : bytes) {
            if (b == raw) {
                h = static_cast<uint8_t>(candidate);
                break;
            }
        }
        if (h != 0xFF) break;
    }
    if (h == 0xFF) return 0xFF;   // 不属于任何支持语义集合
    // 集合 h 在变体 d 下被 canonical 字节 c 的语义使用（逆排列）：
    // variantPerm_[d][c] == hIdx → unmap 输出 c（canonical 字节，即 case 标签）。
    const uint8_t hIdx = opcodeFor(static_cast<VmOpcode>(h));
    for (size_t c = 0; c < variantPerm_[variant].size(); ++c) {
        if (variantPerm_[variant][c] == hIdx) {
            return static_cast<uint8_t>(c);
        }
    }
    return 0xFF;
}

void VmInstance::buildDriftVariants()
{
    for (auto& perm : variantPerm_) perm.clear();
    if (!driftEnabled()) return;

    const size_t k = sizeof(kAllOpcodes) / sizeof(kAllOpcodes[0]);
    std::vector<uint8_t> base(k);
    for (size_t i = 0; i < k; ++i) base[i] = static_cast<uint8_t>(i);

    // 每个变体：对语义集合索引 [0, K) 做独立洗牌排列。canonical 字节值
    // 恰为 0..K-1 的排列（buildOpcodeBytes 双射），故可作集合索引。
    for (int d = 0; d < driftVariants_; ++d) {
        SplitMix64 rng(instanceSeed_ ^ 0xC2B2AE3D27D4EB4Full ^
                       static_cast<uint64_t>(d) * 0x9E3779B97F4A7C15ull);
        std::vector<uint8_t> perm = base;
        for (size_t i = perm.size(); i > 1; --i) {
            const size_t j = static_cast<size_t>(rng.nextBounded(i));
            std::swap(perm[i - 1], perm[j]);
        }
        variantPerm_[static_cast<size_t>(d)] = std::move(perm);
    }
}

bool VmInstance::usesAltHandlerForm(VmOpcode op) const
{
    // 独立种子段 + 语义身份，确定性、与 opcode 映射不耦合。
    SplitMix64 rng(instanceSeed_ ^ 0x2545F4914F6CDD1Dull ^ static_cast<uint64_t>(op));
    return (rng.next() & 1u) != 0u;
}

void VmInstance::buildHandlerOrder()
{
    // 计算/控制流 + object-op opcode（不含 Halt），与 kAllOpcodes 的子集一致。
    handlerOrder_ = {VmOpcode::Copy,    VmOpcode::Unary,     VmOpcode::Binary,
                     VmOpcode::Compare, VmOpcode::Convert,   VmOpcode::Jump,
                     VmOpcode::Branch,  VmOpcode::Return,    VmOpcode::SuperInc,
                     VmOpcode::SuperDec, VmOpcode::Call,     VmOpcode::LoadField,
                     VmOpcode::StoreField, VmOpcode::ArrayLoad, VmOpcode::ArrayStore,
                     VmOpcode::Allocate, VmOpcode::Intrinsic};
    // 用独立种子段洗牌，避免与 opcode 映射的随机流耦合。
    SplitMix64 rng(instanceSeed_ ^ 0x9E3779B97F4A7C15ull);
    for (size_t i = handlerOrder_.size(); i > 1; --i) {
        const size_t j = static_cast<size_t>(rng.nextBounded(i));
        std::swap(handlerOrder_[i - 1], handlerOrder_[j]);
    }
}

uint64_t vmc4DeriveInstanceSeed(uint64_t mainSeed, uint64_t identityHash)
{
    SplitMix64 rng(mainSeed ^ 0xA0761D6478BD642Full);
    // 两轮混合 + 身份哈希，保证不同身份（即使哈希低位相同）产生不同实例 seed。
    const uint64_t a = rng.next() ^ identityHash;
    return a ^ rng.next();
}

}   // namespace mj2x

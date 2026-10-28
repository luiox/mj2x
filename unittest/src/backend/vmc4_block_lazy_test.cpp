// VMC4 块粒度链式懒解密单测（issue #1116 切片 B --vmc4-block-lazy-decrypt）。
//
// 本测试不调用 JNI/DLL（E2E 语义闭环由 vmc4_backend_e2e_test 的
// Vmc4BlockLazyE2E.RoundTripTiers 覆盖），只验证发射文本与派生函数：
//   1. 默认关：无块懒解密痕迹，产物与历史明文形态一致（关闭路径字节级不变）。
//   2. 无加密单开：VmInstance.blockLazy 归一为关闭（恒以 encrypt 为前提）。
//   3. 开启路径：blob 按 blockTargets 划界的 CFG 块独立加密（链式密钥，
//      首块 prevKey = seed），发射 bstate 表 + block_touch helper + CRT 调用；
//      不发射独立边界表（复用 targets）与 cend/cstate（与 64B 档互斥可辨）。
//   4. 块密钥链派生 vmc4DeriveBlockChainKey：确定性 / 恒非 0 / 对 seed、
//      blockIdx、prevKey 各自敏感；首块 k_0 = F(seed, 0, seed) 显式向量。
//   5. roundtrip：解析密文 + targets，测试侧独立实现链推导逐块解密，
//      还原 VmProgram.bytecodes；尾块含 trailing Halt 哨兵。
//   6. 与 drift 叠加：解密产物 == drift 编码后的最终字节流（正交性）。
//   7. 与 shuffle 叠加：targets 密文落盘（解密后链按洗牌后 id 空间推导）、
//      blockTargets 恒升序（物理序 == id 序不变量）、roundtrip 仍逐字节还原。
//   8. 与 64B 档互斥：两档同开时 block 档胜出（blobLazy 归一为关）。
//   9. backend 级：blockLazy 时 JNI_OnLoad 无该 blob 的整体解密调用，且确定性。
//  10. trace surface 契约：describe() 不受 blockLazy 开关影响。

#include "garena/arena.hpp"
#include "test_helper.hpp"
#include <gmock/gmock.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/vmc4/vm_encoder.hpp"
#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"
#include "mj2x/backend/vmc4/vmc4_backend.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

/// @brief 构造 MethodNode（name/desc 须静态存储期：MethodNode 只持视图）。
std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access,
                                           ca::str::Utf8StringRef::from_cstr(name),
                                           ca::str::Utf8StringRef::from_cstr(desc),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

garena::Arena& labelScratchArena()
{
    static garena::Arena arena;
    return arena;
}

/// 完整编码管线：MethodNode -> VmProgram（与 vmc4_encrypt_program_test 同口径）。
std::unique_ptr<VmProgram> encodeMethodNode(std::unique_ptr<MethodNode> mn,
                                            const VmInstance& instance, std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = test::convertMethodNode(converter, *mn, arena);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "InlineVmc4Enc", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    return VmEncoder::encode(*reg, instance, reason);
}

std::unique_ptr<VmProgram> encodeMethod(const std::string& name, const std::string& desc,
                                        std::initializer_list<AbstractInsnNode*> insns,
                                        int maxStack, int maxLocals, const VmInstance& instance,
                                        std::string& reason)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name.c_str(), desc.c_str(),
                         insns, maxStack, maxLocals);
    return encodeMethodNode(std::move(mn), instance, reason);
}

/// 独立实现的 xorshift32 密钥流解密（与 mj2x-rt mj2x_vm_blob_decrypt /
/// native_lib_crypto.hpp::xorStreamBytes 同算法、独立成文交叉验证）。
void xorDecryptForTest(std::vector<uint8_t>& data, uint32_t key)
{
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    for (auto& b : data) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        b = static_cast<uint8_t>(b ^ static_cast<uint8_t>(state & 0xFFu));
    }
}

/// 独立实现的块密钥链派生（与 vmc4DeriveBlockChainKey / CRT
/// mj2x_vm_block_chain_key 同公式、独立成文交叉验证）。
uint32_t blockChainKeyForTest(uint32_t seed, uint32_t blockIdx, uint32_t prevKey)
{
    uint32_t s = seed ^ prevKey ^ (0x9E3779B9u + 0x9E3779B9u * (blockIdx + 1u));
    for (int i = 0; i < 4; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
    }
    return s != 0u ? s : 0x9E3779B9u;
}

/// 独立实现的 64B 档块密钥派生（互斥断言用）。
uint32_t chunkKeyForTest(uint32_t blobKey, uint32_t chunkIndex)
{
    uint32_t s = blobKey ^ (0x9E3779B9u + 0x9E3779B9u * (chunkIndex + 1u));
    for (int i = 0; i < 4; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
    }
    return s != 0u ? s : 0x9E3779B9u;
}

/// 从产物 C 源解析 `unsigned char <arrayName>[] = {0xNN, ...};` 数组字节
/// （手写 hex 解析：避免 scanf %n 的 CRT 差异）。
bool extractArrayBytesForTest(const std::string& src, const std::string& arrayName,
                              std::vector<uint8_t>& out)
{
    const std::string marker = "unsigned char " + arrayName + "[] = {";
    const size_t      start  = src.find(marker);
    if (start == std::string::npos) return false;
    size_t      pos = start + marker.size();
    const size_t end = src.find("};", pos);
    if (end == std::string::npos) return false;
    out.clear();
    while (pos + 1 < end) {
        // 数组体只有 "0xNN," token 与空白/换行（emitByteArray 的固定形态）。
        if (src[pos] == '0' && src[pos + 1] == 'x') {
            size_t      hexPos = pos + 2;
            unsigned    v      = 0;
            int         digits = 0;
            while (hexPos < end && digits < 2) {
                const char c = src[hexPos];
                unsigned    d;
                if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
                else break;
                v = (v << 4) | d;
                ++digits;
                ++hexPos;
            }
            if (digits == 2) {
                out.push_back(static_cast<uint8_t>(v));
                pos = hexPos;
                continue;
            }
        }
        ++pos;
    }
    return true;
}

/// 复刻发射器 emitByteArray 的格式化形态（前 count 字节）。
std::string formattedPrefixForTest(const std::vector<uint8_t>& bytes, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count && i < bytes.size(); ++i) {
        if (i != 0) out += " ";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02x,", static_cast<unsigned>(bytes[i]));
        out += buf;
    }
    return out;
}

/// 从产物 C 源解析 `unsigned int <arrayName>[N] = {NNu, ...};` 数组。
bool extractU32ArrayForTest(const std::string& src, const std::string& arrayName,
                            std::vector<uint32_t>& out)
{
    const std::string declHead = "unsigned int " + arrayName + "[";
    const size_t       start   = src.find(declHead);
    if (start == std::string::npos) return false;
    const size_t brace = src.find("= {", start);
    if (brace == std::string::npos) return false;
    size_t            pos = brace + 3;
    const size_t      end = src.find("};", pos);
    if (end == std::string::npos) return false;
    out.clear();
    while (pos < end) {
        if (src[pos] >= '0' && src[pos] <= '9') {
            unsigned long long v = 0;
            while (pos < end && src[pos] >= '0' && src[pos] <= '9') {
                v = v * 10ull + static_cast<unsigned long long>(src[pos] - '0');
                ++pos;
            }
            out.push_back(static_cast<uint32_t>(v));
            continue;
        }
        ++pos;
    }
    return true;
}

/// 循环 + 分支方法（entry / loop header / body / done 至少 4 块）。
std::unique_ptr<VmProgram> encodeLoopMethod(const VmInstance& instance, std::string& reason,
                                            const char* name = "blockLazy")
{
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    return encodeMethod(name, "(I)I",
                        {
                            new InsnNode(Opcodes::ICONST_0),
                            new VarInsnNode(Opcodes::ISTORE, 1),
                            new InsnNode(Opcodes::ICONST_0),
                            new VarInsnNode(Opcodes::ISTORE, 2),
                            loop,
                            new VarInsnNode(Opcodes::ILOAD, 2),
                            new VarInsnNode(Opcodes::ILOAD, 0),
                            new JumpInsnNode(Opcodes::IF_ICMPGE, done->label),
                            new VarInsnNode(Opcodes::ILOAD, 1),
                            new VarInsnNode(Opcodes::ILOAD, 2),
                            new InsnNode(Opcodes::IADD),
                            new VarInsnNode(Opcodes::ISTORE, 1),
                            new VarInsnNode(Opcodes::ILOAD, 1),
                            new VarInsnNode(Opcodes::ILOAD, 2),
                            new InsnNode(Opcodes::IMUL),
                            new VarInsnNode(Opcodes::ISTORE, 1),
                            new IincInsnNode(2, 1),
                            new JumpInsnNode(Opcodes::GOTO, loop->label),
                            done,
                            new VarInsnNode(Opcodes::ILOAD, 1),
                            new InsnNode(Opcodes::IRETURN),
                        },
                        2, 3, instance, reason);
}

/// backend 级编译辅助：编译一个类的单个静态方法，返回生成 C 文本。
std::string compileOneMethod(Vmc4Backend& backend, const std::string& cls,
                             const std::string& method)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, method.c_str(), "(II)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         },
                         2, 2);
    CompilerMethod cm = test::snapshotMethod(*mn);
    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = test::convertMethodNode(cc, *mn, arena);
    auto result = backend.compileMethod(cls, cm, blocks);
    return result.kind == BackendResult::Kind::C_SOURCE ? result.text : std::string{};
}

/// 按块链式解密（测试侧独立实现，与发射侧加密互为对合）：cipher 原地还原。
void chainDecryptForTest(std::vector<uint8_t>& cipher, const std::vector<uint32_t>& targets,
                         uint32_t blobKey)
{
    const size_t n = targets.size();
    uint32_t     k = blobKey;
    for (size_t c = 0; c < n; ++c) {
        k = blockChainKeyForTest(blobKey, static_cast<uint32_t>(c), k);
        const size_t begin = targets[c];
        const size_t end   = (c + 1 < n) ? targets[c + 1] : cipher.size();
        std::vector<uint8_t> block(cipher.begin() + static_cast<long>(begin),
                                   cipher.begin() + static_cast<long>(end));
        xorDecryptForTest(block, k);
        for (size_t i = 0; i < block.size(); ++i) cipher[begin + i] = block[i];
    }
}

}   // namespace

// ===========================================================================
// 1. 默认关：零痕迹（关闭路径产物形态不变）。
// ===========================================================================
TEST(Vmc4BlockLazyTest, OffByDefaultKeepsLegacyShape)
{
    Vmc4Options opts;
    EXPECT_FALSE(opts.programBlockLazyDecrypt);
    VmInstance instance(opts);
    EXPECT_FALSE(instance.blockLazy());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "bloff");
    // 明文 const 数组 + 零块懒解密痕迹。
    EXPECT_NE(src.find("static const unsigned char mj2x_vm_code_bloff[] = {"), std::string::npos)
        << src;
    EXPECT_EQ(src.find("mj2x_vm_bstate_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_block_touch_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_block_lazy_decrypt"), std::string::npos) << src;
    EXPECT_EQ(src.find("block-chunked at rest"), std::string::npos) << src;
}

// ===========================================================================
// 2. 无加密单开：归一为关闭（blockLazy 恒以 blobEncrypt 为前提）。
// ===========================================================================
TEST(Vmc4BlockLazyTest, WithoutEncryptNormalizesToOff)
{
    Vmc4Options opts;
    opts.seed                    = 0x7E5;
    opts.programBlockLazyDecrypt = true;   // 无 programBlobEncrypt
    VmInstance instance(opts);
    EXPECT_FALSE(instance.blockLazy());
    EXPECT_FALSE(instance.blobEncrypt());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "blne");
    EXPECT_NE(src.find("static const unsigned char mj2x_vm_code_blne[] = {"), std::string::npos)
        << src;
    EXPECT_NE(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_bstate_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_block_touch_"), std::string::npos) << src;
}

// ===========================================================================
// 3. 开启路径：块密文 + bstate/block_touch 基础设施 + 链式 roundtrip；
//    块边界 == blockTargets 划界（尾块含 Halt）；零独立边界表。
// ===========================================================================
TEST(Vmc4BlockLazyTest, EmitsBlockCipherAndChainRoundtrip)
{
    Vmc4Options opts;
    opts.seed                    = 0x5EED3;
    opts.programBlobEncrypt      = true;
    opts.programBlockLazyDecrypt = true;
    VmInstance instance(opts);
    ASSERT_TRUE(instance.blockLazy());
    ASSERT_FALSE(instance.blobLazy());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // 多块：循环方法至少 entry/header/body/done 4 块（+合成边块）。
    ASSERT_GE(prog->blockTargets.size(), 4u) << "fixture must span multiple blocks";
    ASSERT_FALSE(prog->bytecodes.empty());

    const std::string tag = "blc1";
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, tag);
    // 密文 non-const 数组 + 块档注释标记。
    EXPECT_NE(src.find("static unsigned char mj2x_vm_code_blc1[] = {"), std::string::npos) << src;
    EXPECT_NE(src.find("block-chunked at rest, chain keys"), std::string::npos) << src;
    // 基础设施：bstate 状态表 + block_touch helper + CRT 块定位/懒解密调用；
    // 边界复用 targets（不发射 cend），与 64B 档命名面互斥可辨。
    EXPECT_NE(src.find("static volatile unsigned char mj2x_vm_bstate_blc1["), std::string::npos)
        << src;
    EXPECT_NE(src.find("static void mj2x_vm_block_touch_blc1("), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_block_locate(mj2x_vm_targets_blc1,"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_block_lazy_decrypt(mj2x_vm_code_blc1,"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cend_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cstate_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_lazy_touch_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_blob_decrypt"), std::string::npos) << src;

    // 产物断言：明文字节序列搜不到。
    EXPECT_EQ(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos) << src;

    // 链式 roundtrip：解析密文 + targets（shuffle 关 = 明文 targets），测试侧
    // 独立实现链推导逐块解密，还原结果必须与 VmProgram.bytecodes 逐字节一致
    // （尾块含 trailing Halt 哨兵也随末块解密）。
    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_blc1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());
    std::vector<uint32_t> targets;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_targets_blc1", targets));
    ASSERT_EQ(targets.size(), prog->blockTargets.size());
    EXPECT_EQ(targets, prog->blockTargets);
    chainDecryptForTest(cipher, targets, vmc4DeriveBlobKey(instance.instanceSeed()));
    EXPECT_EQ(cipher, prog->bytecodes)
        << "per-block chain decryption must restore the exact VmProgram bytecodes";
    // Halt 哨兵在流尾，属末块区间：末块解密后最后一字节 == 明文末字节。
    EXPECT_EQ(cipher.back(), prog->bytecodes.back());
}

// ===========================================================================
// 4. 块密钥链派生性质：确定性 / 恒非 0 / seed·blockIdx·prevKey 各自敏感。
// ===========================================================================
TEST(Vmc4BlockLazyTest, ChainKeyDerivationProperties)
{
    // 确定性 + 恒非 0 + 镜像公式一致性（blockChainKeyForTest 独立成文）。
    for (uint32_t seed = 1u; seed < 16u; ++seed) {
        for (uint32_t c = 0u; c < 8u; ++c) {
            const uint32_t prev = 0x13579BDFu ^ (c * 0x9E3779B9u);
            const uint32_t k    = vmc4DeriveBlockChainKey(seed, c, prev);
            EXPECT_EQ(k, vmc4DeriveBlockChainKey(seed, c, prev)) << "deterministic";
            EXPECT_EQ(k, blockChainKeyForTest(seed, c, prev)) << "mirror formula";
            EXPECT_NE(k, 0u) << "never the xorshift32 deadlock state";
        }
    }
    // 首块口径：k_0 = F(seed, 0, seed)（prevKey = seed）。
    EXPECT_EQ(vmc4DeriveBlockChainKey(0x0BADC0DEu, 0u, 0x0BADC0DEu),
              blockChainKeyForTest(0x0BADC0DEu, 0u, 0x0BADC0DEu));
    // blockIdx 敏感。
    EXPECT_NE(vmc4DeriveBlockChainKey(0x5EEDu, 0u, 0x42u),
              vmc4DeriveBlockChainKey(0x5EEDu, 1u, 0x42u));
    // seed 敏感。
    EXPECT_NE(vmc4DeriveBlockChainKey(0x5EEDu, 3u, 0x42u),
              vmc4DeriveBlockChainKey(0x5EEEu, 3u, 0x42u));
    // prevKey 敏感（链式核心性质：前块密钥参与派生）。
    EXPECT_NE(vmc4DeriveBlockChainKey(0x5EEDu, 3u, 0x42u),
              vmc4DeriveBlockChainKey(0x5EEDu, 3u, 0x43u));
    // 与 64B 档派生面互不耦合（同 (key, index) 不同公式）。
    EXPECT_NE(vmc4DeriveBlockChainKey(0x5EEDu, 5u, 0x5EEDu),
              vmc4DeriveChunkKey(0x5EEDu, 5u));
}

// ===========================================================================
// 5. 与 drift 叠加：加密作用于 drift 编码后的最终字节流，解密还原该流。
// ===========================================================================
TEST(Vmc4BlockLazyTest, ComposesWithDrift)
{
    Vmc4Options opts;
    opts.seed                    = 0xD21Fu;
    opts.driftVariants           = 3;
    opts.driftPeriod             = 16;
    opts.programBlobEncrypt      = true;
    opts.programBlockLazyDecrypt = true;
    VmInstance instance(opts);
    ASSERT_TRUE(instance.blockLazy());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason, "blockDrift");
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "bld1");
    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_bld1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());
    chainDecryptForTest(cipher, prog->blockTargets,
                        vmc4DeriveBlobKey(instance.instanceSeed()));
    // drift 编码后的流即 VmProgram.bytecodes（加密施加于其上），解密还原之。
    EXPECT_EQ(cipher, prog->bytecodes);
}

// ===========================================================================
// 6. 与 shuffle 叠加：targets 密文落盘；解密 targets 后链按洗牌后 id 空间
//    推导，roundtrip 仍逐字节还原；targets 恒升序（物理序 == id 序）。
// ===========================================================================
TEST(Vmc4BlockLazyTest, ComposesWithShuffle)
{
    Vmc4Options opts;
    opts.seed                    = 0x5A1;
    opts.shuffleBlocks           = true;
    opts.programBlobEncrypt      = true;
    opts.programBlockLazyDecrypt = true;
    VmInstance instance(opts);
    ASSERT_TRUE(instance.blockLazy());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason, "blockShuffle");
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "bls1");
    // shuffle 面：targets 以密文落盘（non-const），JNI_OnLoad 解密条目由
    // backend 发射（slice A 既有面，此处只验发射形态）。
    EXPECT_NE(src.find("static unsigned int mj2x_vm_targets_bls1[] = {"), std::string::npos)
        << src;
    EXPECT_NE(src.find("shuffled block ids, encrypted at rest"), std::string::npos) << src;
    // blockTargets 恒升序不变量（切片 A：物理序 == id 序）。
    ASSERT_TRUE(std::is_sorted(prog->blockTargets.begin(), prog->blockTargets.end()));

    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_bls1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());
    std::vector<uint32_t> targetsCipher;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_targets_bls1", targetsCipher));
    ASSERT_EQ(targetsCipher.size(), prog->blockTargets.size());
    // targets 表密文 ≠ 明文（独立加密面）。
    EXPECT_NE(targetsCipher, prog->blockTargets);
    // 测试侧解密 targets（slice A 密钥派生）→ 链式逐块解密 → 全量还原。
    std::vector<uint8_t> targetsBytes;
    for (uint32_t v : targetsCipher) {
        targetsBytes.push_back(static_cast<uint8_t>(v & 0xFFu));
        targetsBytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        targetsBytes.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        targetsBytes.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }
    xorDecryptForTest(targetsBytes, vmc4DeriveTargetsKey(instance.instanceSeed()));
    std::vector<uint32_t> targets(targetsCipher.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i] = static_cast<uint32_t>(targetsBytes[4 * i]) |
                     (static_cast<uint32_t>(targetsBytes[4 * i + 1]) << 8) |
                     (static_cast<uint32_t>(targetsBytes[4 * i + 2]) << 16) |
                     (static_cast<uint32_t>(targetsBytes[4 * i + 3]) << 24);
    }
    EXPECT_EQ(targets, prog->blockTargets) << "targets decrypt restores the layout table";
    chainDecryptForTest(cipher, targets, vmc4DeriveBlobKey(instance.instanceSeed()));
    EXPECT_EQ(cipher, prog->bytecodes);
}

// ===========================================================================
// 7. 与 64B 档互斥：两档同开时 block 档胜出（blobLazy 归一为关），发射面
//    只含块档基础设施。
// ===========================================================================
TEST(Vmc4BlockLazyTest, BothLazyFlagsBlockWins)
{
    Vmc4Options opts;
    opts.seed                    = 0xB10C;
    opts.programBlobEncrypt      = true;
    opts.programLazyDecrypt      = true;
    opts.programBlockLazyDecrypt = true;
    VmInstance instance(opts);
    EXPECT_TRUE(instance.blockLazy());
    EXPECT_FALSE(instance.blobLazy());
    std::string reason;
    auto prog = encodeLoopMethod(instance, reason, "blockWins");
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "blw1");
    EXPECT_NE(src.find("mj2x_vm_block_touch_blw1("), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cend_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cstate_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_lazy_touch_"), std::string::npos) << src;
}

// ===========================================================================
// 8. backend 级：blockLazy 时 JNI_OnLoad 无该 blob 的整体解密调用，产物确定性。
// ===========================================================================
TEST(Vmc4BlockLazyTest, BackendSkipsJniOnLoadDecryptAndIsDeterministic)
{
    Vmc4Options opts;
    opts.seed                    = 0xBEEF5;
    opts.programBlobEncrypt      = true;
    opts.programBlockLazyDecrypt = true;
    Vmc4Backend backend;
    backend.setOptions(opts);
    const std::string body = compileOneMethod(backend, "BlkLazy", "add");
    ASSERT_FALSE(body.empty());
    const std::string tag = codegen::hex64(codegen::fnv1a64("BlkLazy.add(II)I"));
    // 方法体发射面：块档基础设施齐备，网格档/批量解密标记缺席。
    EXPECT_NE(body.find("static void mj2x_vm_block_touch_" + tag + "("), std::string::npos)
        << body;
    EXPECT_NE(body.find("static volatile unsigned char mj2x_vm_bstate_" + tag), std::string::npos)
        << body;
    EXPECT_EQ(body.find("mj2x_vm_blob_decrypt"), std::string::npos) << body;
    EXPECT_EQ(body.find("mj2x_vm_cstate_"), std::string::npos) << body;

    backend.finalize(CompilerIndex{});
    const std::string full = backend.generatedSource();
    // JNI_OnLoad 全文：无任何整体解密调用（块保持密文到首达），确定性复现。
    EXPECT_EQ(full.find("mj2x_vm_blob_decrypt"), std::string::npos) << full;
    Vmc4Backend again;
    again.setOptions(opts);
    ASSERT_FALSE(compileOneMethod(again, "BlkLazy", "add").empty());
    again.finalize(CompilerIndex{});
    EXPECT_EQ(again.generatedSource(), full) << "same seed must reproduce the product";
}

// ===========================================================================
// 9. trace surface 契约：describe() 不受 blockLazy 开关影响。
// ===========================================================================
TEST(Vmc4BlockLazyTest, TraceSurfaceUnaffectedByBlockLazyFlag)
{
    Vmc4Options base;
    base.seed = 0x77;
    VmInstance plain(base);
    Vmc4Options on     = base;
    on.programBlobEncrypt      = true;
    on.programBlockLazyDecrypt = true;
    VmInstance lazy(on, plain.instanceSeed());
    EXPECT_EQ(plain.describe(), lazy.describe());
}

// ===========================================================================
// 10. 选项流入实例：programBlockLazyDecrypt → blockLazy()（加密前提内）。
// ===========================================================================
TEST(Vmc4BlockLazyTest, OptionFlowsIntoInstance)
{
    Vmc4Options on;
    on.seed                    = 9;
    on.programBlobEncrypt      = true;
    on.programBlockLazyDecrypt = true;
    VmInstance iOn(on, vmc4DeriveInstanceSeed(on.seed, 0x42));
    EXPECT_TRUE(iOn.blockLazy());
    EXPECT_TRUE(iOn.blobEncrypt());
    // 开关不参与实例 seed 派生：同 seed 开/关是同一实例（仅发射形态不同）。
    EXPECT_EQ(iOn.instanceSeed(), vmc4DeriveInstanceSeed(on.seed, 0x42));
}

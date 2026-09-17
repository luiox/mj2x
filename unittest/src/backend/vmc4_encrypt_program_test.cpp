// VMC4 程序 blob at-rest 加密单测（issue #760 切片 1 --vmc4-encrypt-program +
// 切片 2 --vmc4-lazy-decrypt 分块懒解密）。
//
// 本测试不调用 JNI/DLL（E2E 语义闭环由 vmc4_backend_e2e_test 的
// ProgramBlobEncryptRoundTrip / LazyChunkDecryptRoundTrip 覆盖），
// 只验证发射文本与派生函数：
//   1. 关闭路径（默认）：数据段保持明文 const 数组形态，无解密调用
//      （关闭路径产物字节级不变——既有 vmc4 测试与 trace surface 契约）。
//   2. 开启路径：数据段为 non-const 密文数组，产物文本搜不到关闭态的
//      已知明文字节序列（产物断言测试）。
//   3. roundtrip：从产物 C 源解析密文数组，用独立实现的 xorshift32 密钥流
//      解密，逐字节还原 VmProgram.bytecodes。
//   4. 错误 seed：设计不做显式 fail-closed 检测（无校验和）——错误密钥解出
//      垃圾流，断言其与明文不同；运行期由解释器 InvalidBytecode（status=2）
//      防御停机兜底，属隐式 fail-closed。
//   5. 与 dispatch drift 的叠加：加密作用于 drift 编码后的最终字节流
//      （两者正交：drift 是明文流的语义解释，加密是 at-rest 形态）。
//   6. trace surface 契约：describe() 不受加密开关影响。
//   7. backend 级：finalize 的 JNI_OnLoad 只在开启时发射解密调用，且确定性。
// 切片 2（lazy 分块）追加：
//   8. lazy 无加密：开关被归一为关闭（VmInstance.blobLazy 恒以 encrypt 为
//      前提），产物回退关闭路径形态。
//   9. lazy 开启：数据段按指令对齐 64 字节目标块独立加密（块边界 ⊆
//      instructionOffsets、指令不跨块、超长 SWITCH 自成一块），发射
//      cend/cstate 表与 mj2x_vm_lazy_touch_ 辅助，JNI_OnLoad 无解密调用；
//      逐块独立解密可还原完整明文流。
//  10. 块密钥派生（vmc4DeriveChunkKey）：确定性 / 恒非 0 / 块下标与主密钥敏感。
//  11. lazy 与 drift 叠加、trace surface / 实例 seed 不受 lazy 开关影响。

#include "garena/arena.hpp"
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

/// @brief 构造 MethodNode。
///
/// @note name/desc 必须是静态存储期的 C 字符串（字面量）：MethodNode 只持有
/// Utf8StringRef 视图不拷贝（与 vmc4_backend_test 的同名 helper 同约定）。
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

/// @brief 同上，指令序列为运行期构造的 vector（超长指令 fixture 用）。
std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       const std::vector<AbstractInsnNode*>& insns,
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

/// 完整编码管线：MethodNode -> VmProgram（与 vmc4_backend_test 同口径）。
std::unique_ptr<VmProgram> encodeMethodNode(std::unique_ptr<MethodNode> mn,
                                            const VmInstance& instance, std::string& reason)
{
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
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
/// native_lib_crypto.hpp::xorStreamBytes 同算法、独立成文交叉验证发射产物）。
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

/// 独立实现的分块密钥派生（与 vmc4DeriveChunkKey / CRT mj2x_vm_chunk_key 同
/// 公式、独立成文交叉验证）：golden-ratio 扰动 + 4 轮 xorshift32，恒非 0。
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

/// 复刻发射器 emitByteArray 的格式化形态（前 count 字节，count <= 16 时
/// 与产物逐字符一致），用于"已知字节序列在产物中搜得到/搜不到"断言。
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

size_t countOccurrencesForTest(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

/// 从产物 C 源解析 `unsigned int <arrayName>[N] = {NNu, ...};` 数组
/// （emitU32Array 的固定形态：只有十进制数字 + 'u' 后缀 + 逗号/空白）。
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
    CompilerMethod cm;
    cm.methodNode = mn.get();
    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod(cls, cm, blocks);
    return result.kind == BackendResult::Kind::C_SOURCE ? result.text : std::string{};
}

}   // namespace

// ===========================================================================
// 1. 关闭路径（默认）：明文 const 数组形态不变，无解密调用。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, EncryptOffKeepsLegacyPlaintextArray)
{
    Vmc4Options opts;   // programBlobEncrypt 默认 false
    ASSERT_FALSE(opts.programBlobEncrypt);
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("offAdd", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_GE(prog->bytecodes.size(), 8u);

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "off1");
    // 明文 const 数组形态（历史发射形态，字节级不变）。
    EXPECT_NE(src.find("static const unsigned char mj2x_vm_code_off1[] = {"), std::string::npos)
        << src;
    // 明文字节序列直接出现在产物中（关闭态语义）。
    EXPECT_NE(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos) << src;
    // 不存在 non-const 形态与解密调用。
    EXPECT_EQ(src.find("static unsigned char mj2x_vm_code_off1[] = {"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_blob_decrypt"), std::string::npos) << src;
}

// ===========================================================================
// 2. 开启路径：密文 non-const 数组；产物断言——关闭态已知明文序列搜不到。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, EncryptOnHidesKnownPlaintextSequence)
{
    Vmc4Options opts;
    opts.seed                = 0xC0FFEE;
    opts.programBlobEncrypt  = true;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("onAdd", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_GE(prog->bytecodes.size(), 8u);

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "on1");
    // non-const 密文数组形态 + 注释标记。
    EXPECT_NE(src.find("static unsigned char mj2x_vm_code_on1[] = {"), std::string::npos) << src;
    EXPECT_NE(src.find("encrypted at rest, decrypted once in JNI_OnLoad"), std::string::npos)
        << src;
    EXPECT_EQ(src.find("static const unsigned char mj2x_vm_code_on1[] = {"), std::string::npos)
        << src;
    // 产物断言：同一实例的明文字节序列（关闭态产物中的已知字节序列）在开启态
    // 产物中搜不到（前 8 字节的 emitByteArray 形态）。
    EXPECT_EQ(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos)
        << "plaintext opcode sequence must not appear in the encrypted product\n"
        << src;

    // 密文数组可解析，且与明文不同（加密真实发生）。
    std::vector<uint8_t> emitted;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_on1", emitted));
    ASSERT_EQ(emitted.size(), prog->bytecodes.size());
    EXPECT_NE(emitted, prog->bytecodes) << "data segment must differ from plaintext";
}

// ===========================================================================
// 3. roundtrip：产物密文 --独立密钥流解密--> VmProgram.bytecodes。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, RoundtripFromProductRestoresBytecodes)
{
    Vmc4Options opts;
    opts.seed                = 0x5EED;
    opts.programBlobEncrypt  = true;
    VmInstance  instance(opts);
    std::string reason;
    // 循环方法：字节流更长，覆盖多行密文数组。
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("encLoop", "(I)I",
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
                                 new IincInsnNode(2, 1),
                                 new JumpInsnNode(Opcodes::GOTO, loop->label),
                                 done,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_GT(prog->bytecodes.size(), 16u);

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "rt1");
    std::vector<uint8_t> emitted;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_rt1", emitted));
    ASSERT_EQ(emitted.size(), prog->bytecodes.size());
    // 用独立实现的密钥流 + 同一派生密钥解密：还原结果必须与编码器输出逐字节一致。
    xorDecryptForTest(emitted, vmc4DeriveBlobKey(instance.instanceSeed()));
    EXPECT_EQ(emitted, prog->bytecodes)
        << "product ciphertext must decrypt back to the exact VmProgram bytecodes";
}

// ===========================================================================
// 4. 错误 seed：无显式 fail-closed 检测（首切片无校验和），错误密钥解出
//    垃圾流 ≠ 明文；运行期由解释器 InvalidBytecode 防御停机隐式兜底。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, WrongSeedYieldsDifferentStream)
{
    Vmc4Options opts;
    opts.seed                = 0xA11CE;
    opts.programBlobEncrypt  = true;
    VmInstance  instance(opts);
    std::string reason;
    auto prog = encodeMethod("wrongKey", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "wk1");
    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_wk1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());

    // 错误 seed（不同实例身份 → 不同密钥）：解出的流与明文不同（非明文即
    // 无法被 dispatcher 正确取码，绝大多数首个 opcode 即落 default → status=2）。
    std::vector<uint8_t> bad = cipher;
    xorDecryptForTest(bad, vmc4DeriveBlobKey(instance.instanceSeed() ^ 0xDEADBEEFull));
    EXPECT_NE(bad, prog->bytecodes)
        << "wrong-seed decryption must not accidentally reproduce the program";
}

// ===========================================================================
// 5. 密钥派生：确定性 + seed 敏感 + 恒非 0（xorshift32 死锁态回避）。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, BlobKeyDerivationIsDeterministicSeedSensitiveNonZero)
{
    EXPECT_EQ(vmc4DeriveBlobKey(0x1234), vmc4DeriveBlobKey(0x1234));
    EXPECT_EQ(vmc4DeriveBlobKey(0), vmc4DeriveBlobKey(0));
    for (uint64_t s = 0; s < 256; ++s) {
        EXPECT_NE(vmc4DeriveBlobKey(s), vmc4DeriveBlobKey(s ^ 0x1FFFFull))
            << "seed " << s << ": key derivation must be seed sensitive";
        EXPECT_NE(vmc4DeriveBlobKey(s), 0u) << "derived key must never be 0";
    }
    // 跨实例身份派生路径：不同 instanceSeed → 不同密钥（取两组固定值对拍）。
    EXPECT_NE(vmc4DeriveBlobKey(0x11111111ull), vmc4DeriveBlobKey(0x22222222ull));
}

// ===========================================================================
// 6. 与 dispatch drift 的叠加：加密作用于 drift 编码后的最终字节流。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, EncryptComposesWithDriftOnFinalStream)
{
    Vmc4Options opts;
    opts.seed                = 0x7777;
    opts.aliasCount          = 2;
    opts.driftVariants       = 2;
    opts.driftPeriod         = 8;
    opts.programBlobEncrypt  = true;
    VmInstance  instance(opts);
    std::string reason;
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("driftEnc", "(I)I",
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
                                 new IincInsnNode(2, 1),
                                 new JumpInsnNode(Opcodes::GOTO, loop->label),
                                 done,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // prog->bytecodes 已是 drift 编码后的最终字节流；加密在发射期施加其上。
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "de1");
    std::vector<uint8_t> emitted;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_de1", emitted));
    ASSERT_EQ(emitted.size(), prog->bytecodes.size());
    EXPECT_NE(emitted, prog->bytecodes) << "drift stream must be encrypted at rest";
    // 解密还原 = drift 编码流（drift unmap 表按明文 pc 偏移工作，正交叠加）。
    xorDecryptForTest(emitted, vmc4DeriveBlobKey(instance.instanceSeed()));
    EXPECT_EQ(emitted, prog->bytecodes);
    // drift 结构（unmap 表）不受加密影响，仍按明文流语义发射。
    EXPECT_NE(src.find("mj2x_vm_unmap_de1_0"), std::string::npos) << src;
}

// ===========================================================================
// 7. trace surface 契约：describe() 不受加密开关影响（红队契约字段集稳定）。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, TraceSurfaceUnaffectedByEncryptFlag)
{
    Vmc4Options plain;
    plain.seed = 7;
    Vmc4Options enc = plain;
    enc.programBlobEncrypt = true;
    VmInstance a(plain);
    VmInstance b(enc);
    EXPECT_EQ(a.describe(), b.describe());
}

// ===========================================================================
// 8. backend 级：JNI_OnLoad 只在开启时发射解密调用；同配置确定性。
//    产物断言：关闭态产物中的已知字节序列在开启态产物中搜不到。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, BackendEmitsJniOnLoadDecryptOnlyWhenEnabled)
{
    // --- 关闭态：提取已知明文序列（同一 seed 同一身份 → 同一编码流）。
    Vmc4Options offOpts;
    offOpts.seed = 0x2B;
    Vmc4Backend offBackend;
    offBackend.setOptions(offOpts);
    const std::string offText = compileOneMethod(offBackend, "EncProd", "add");
    ASSERT_FALSE(offText.empty());
    std::vector<uint8_t> plainFromProduct;
    ASSERT_TRUE(extractArrayBytesForTest(offText, "mj2x_vm_code_" +
                                                   codegen::hex64(codegen::fnv1a64(
                                                       "EncProd.add(II)I")),
                                         plainFromProduct))
        << offText;
    ASSERT_GE(plainFromProduct.size(), 8u);
    offBackend.finalize(CompilerIndex{});
    EXPECT_EQ(offBackend.generatedSource().find("mj2x_vm_blob_decrypt"), std::string::npos)
        << "encrypt-off product must not reference the decrypt primitive\n"
        << offBackend.generatedSource();

    // --- 开启态：产物搜不到关闭态的已知明文字节序列。
    Vmc4Options onOpts = offOpts;
    onOpts.programBlobEncrypt = true;
    Vmc4Backend onBackend;
    onBackend.setOptions(onOpts);
    const std::string onText = compileOneMethod(onBackend, "EncProd", "add");
    ASSERT_FALSE(onText.empty());
    const std::string onTag =
        codegen::hex64(codegen::fnv1a64("EncProd.add(II)I"));
    EXPECT_EQ(onText.find(formattedPrefixForTest(plainFromProduct, 8)), std::string::npos)
        << "known plaintext sequence from the encrypt-off product must not appear\n"
        << onText;
    EXPECT_NE(onText.find("static unsigned char mj2x_vm_code_" + onTag + "[] = {"),
              std::string::npos)
        << onText;

    // JNI_OnLoad（finalize 产物）发射每方法一条解密调用：数组名 + 长度 + 密钥。
    onBackend.finalize(CompilerIndex{});
    const std::string full = onBackend.generatedSource();
    EXPECT_EQ(countOccurrencesForTest(full, "mj2x_vm_blob_decrypt(mj2x_vm_code_" + onTag), 1u)
        << full;
    size_t pos = full.find("mj2x_vm_blob_decrypt(mj2x_vm_code_" + onTag);
    ASSERT_NE(pos, std::string::npos);
    const size_t eol     = full.find('\n', pos);
    const std::string line = full.substr(pos, eol - pos);
    // 形态：mj2x_vm_blob_decrypt(mj2x_vm_code_<tag>, <N>u, 0xKKKKKKKKu);
    EXPECT_NE(line.find("u, 0x"), std::string::npos) << line;
    EXPECT_EQ(line.back(), ';') << line;
    // 解密调用置于 JNI_OnLoad 体内（加载期一次性解密，先于任何 native 调用）。
    const size_t onLoad = full.find("JNIEXPORT jint JNICALL JNI_OnLoad");
    ASSERT_NE(onLoad, std::string::npos);
    EXPECT_GT(pos, onLoad) << "decrypt calls must live inside JNI_OnLoad";

    // 确定性：同 seed 同输入 → finalize 产物逐字节一致。
    Vmc4Backend again;
    again.setOptions(onOpts);
    ASSERT_FALSE(compileOneMethod(again, "EncProd", "add").empty());
    again.finalize(CompilerIndex{});
    EXPECT_EQ(again.generatedSource(), full) << "same seed must reproduce the product";
}

// ===========================================================================
// 9. 开关透传：Vmc4Options.programBlobEncrypt -> VmInstance.blobEncrypt()。
// ===========================================================================
TEST(Vmc4EncryptProgramTest, OptionFlowsIntoInstance)
{
    Vmc4Options off;
    VmInstance  iOff(off);
    EXPECT_FALSE(iOff.blobEncrypt());
    Vmc4Options on;
    on.programBlobEncrypt = true;
    on.seed               = 9;
    VmInstance iOn(on, vmc4DeriveInstanceSeed(on.seed, 0x42));
    EXPECT_TRUE(iOn.blobEncrypt());
    // 加密开关不参与实例 seed 派生：同 seed 开/关是同一实例（仅发射形态不同）。
    EXPECT_EQ(iOn.instanceSeed(), vmc4DeriveInstanceSeed(on.seed, 0x42));
}

// ===========================================================================
// 切片 2（--vmc4-lazy-decrypt，分块懒解密）
// ===========================================================================

// 10. lazy 单独开启（无 encrypt）：VmInstance.blobLazy 归一为关闭，发射产物
//     与关闭路径同形态（明文 const 数组、无懒解密基础设施、无解密调用）。
TEST(Vmc4EncryptProgramTest, LazyWithoutEncryptNormalizesToOff)
{
    Vmc4Options opts;
    opts.seed                = 0x7E5;
    opts.programLazyDecrypt  = true;   // 无 programBlobEncrypt
    VmInstance instance(opts);
    // 归一化在构造期完成：blobLazy 恒以 blobEncrypt 为前提。
    EXPECT_FALSE(instance.blobLazy());
    EXPECT_FALSE(instance.blobEncrypt());
    std::string reason;
    auto prog = encodeMethod("lazyNoEnc", "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 2, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "lne1");
    // 关闭路径形态：明文 const 数组、明文序列可见、零懒解密/解密痕迹。
    EXPECT_NE(src.find("static const unsigned char mj2x_vm_code_lne1[] = {"), std::string::npos)
        << src;
    EXPECT_NE(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cstate_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_cend_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_lazy_touch_"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_lazy_decrypt"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_blob_decrypt"), std::string::npos) << src;
}

// 11. lazy 开启：密文块数组 + cend/cstate 表 + touch helper + CRT 调用形态；
//     逐块独立解密（独立镜像的块密钥派生）还原完整明文流。
TEST(Vmc4EncryptProgramTest, LazyDecryptEmitsChunkedCipherAndInfrastructure)
{
    Vmc4Options opts;
    opts.seed                 = 0x5EED2;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    VmInstance instance(opts);
    ASSERT_TRUE(instance.blobLazy());
    std::string reason;
    // 循环方法（体含两段算术链）：字节流 > 64 字节，保证多块。
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("lazyChunk", "(I)I",
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
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::IXOR),
                                 new VarInsnNode(Opcodes::ISTORE, 1),
                                 new IincInsnNode(2, 1),
                                 new JumpInsnNode(Opcodes::GOTO, loop->label),
                                 done,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_GT(prog->bytecodes.size(), 64u) << "fixture must span multiple chunks";

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "lc1");
    // 密文 non-const 数组 + lazy 注释标记；不再有 JNI_OnLoad 整体解密标记。
    EXPECT_NE(src.find("static unsigned char mj2x_vm_code_lc1[] = {"), std::string::npos) << src;
    EXPECT_NE(src.find("chunked at rest, decrypted lazily on first"), std::string::npos) << src;
    // 懒解密基础设施：块终点表 / 状态表 / touch helper / CRT 懒解密调用。
    EXPECT_NE(src.find("static const unsigned int mj2x_vm_cend_lc1["), std::string::npos) << src;
    EXPECT_NE(src.find("static volatile unsigned char mj2x_vm_cstate_lc1["), std::string::npos)
        << src;
    EXPECT_NE(src.find("static void mj2x_vm_lazy_touch_lc1("), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_chunk_locate(mj2x_vm_cend_lc1,"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_lazy_decrypt(mj2x_vm_code_lc1,"), std::string::npos) << src;
    EXPECT_EQ(src.find("mj2x_vm_blob_decrypt"), std::string::npos) << src;

    // 产物断言：明文字节序列搜不到。
    EXPECT_EQ(src.find(formattedPrefixForTest(prog->bytecodes, 8)), std::string::npos) << src;

    // 逐块独立解密：解析密文 + cend 表，用独立镜像的块密钥派生逐块解密，
    // 还原结果必须与 VmProgram.bytecodes 逐字节一致。
    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_lc1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());
    std::vector<uint32_t> ends;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_cend_lc1", ends));
    ASSERT_FALSE(ends.empty());
    const uint32_t blobKey = vmc4DeriveBlobKey(instance.instanceSeed());
    uint32_t       begin   = 0;
    for (size_t c = 0; c < ends.size(); ++c) {
        std::vector<uint8_t> chunk(cipher.begin() + begin, cipher.begin() + ends[c]);
        xorDecryptForTest(chunk, chunkKeyForTest(blobKey, static_cast<uint32_t>(c)));
        for (size_t i = 0; i < chunk.size(); ++i) cipher[begin + i] = chunk[i];
        begin = ends[c];
    }
    EXPECT_EQ(cipher, prog->bytecodes)
        << "per-chunk decryption must restore the exact VmProgram bytecodes";
}

// 12. 块边界对齐指令边界（正确性不变量）：边界 ⊆ instructionOffsets；任何
//     指令不跨块（起止同块）；blockTargets 全部落在指令起点上。
TEST(Vmc4EncryptProgramTest, LazyChunkBoundariesRespectInstructionBoundaries)
{
    Vmc4Options opts;
    opts.seed                 = 0xB0D;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    VmInstance instance(opts);
    std::string reason;
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("chunkAlign", "(I)I",
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
                                 new IincInsnNode(2, 1),
                                 new JumpInsnNode(Opcodes::GOTO, loop->label),
                                 done,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    // 编码器恒记录指令边界（确定性派生，与开关无关）。
    ASSERT_FALSE(prog->instructionOffsets.empty());
    EXPECT_EQ(prog->instructionOffsets.front(), 0u);

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "ca1");
    std::vector<uint32_t> ends;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_cend_ca1", ends));
    const uint32_t blobSize = static_cast<uint32_t>(prog->bytecodes.size());
    ASSERT_FALSE(ends.empty());
    EXPECT_EQ(ends.back(), blobSize);
    for (size_t c = 1; c < ends.size(); ++c) {
        EXPECT_LT(ends[c - 1], ends[c]) << "chunk ends must be strictly increasing";
    }
    // 每个内部块边界都是某条指令的起点。
    for (size_t c = 0; c + 1 < ends.size(); ++c) {
        EXPECT_NE(std::find(prog->instructionOffsets.begin(), prog->instructionOffsets.end(),
                            ends[c]),
                  prog->instructionOffsets.end())
            << "chunk boundary " << ends[c] << " must fall on an instruction start";
    }
    // 指令不跨块：指令起点与终点 - 1 属同一块。
    auto chunkOf = [&](uint32_t pc) {
        size_t c = 0;
        while (c < ends.size() && pc >= ends[c]) ++c;
        return c;
    };
    for (size_t i = 0; i < prog->instructionOffsets.size(); ++i) {
        const uint32_t start = prog->instructionOffsets[i];
        const uint32_t end   = (i + 1 < prog->instructionOffsets.size())
                                   ? prog->instructionOffsets[i + 1]
                                   : blobSize;
        EXPECT_EQ(chunkOf(start), chunkOf(end - 1))
            << "instruction [" << start << ", " << end << ") must live in one chunk";
    }
    // 块入口（控制流目标）恒为指令起点：blockTargets ⊆ instructionOffsets。
    for (uint32_t t : prog->blockTargets) {
        EXPECT_NE(std::find(prog->instructionOffsets.begin(), prog->instructionOffsets.end(), t),
                  prog->instructionOffsets.end())
            << "block target " << t << " must be an instruction start";
    }
}

// 13. 超长指令（大 tableswitch > 64 字节）自成一块，不被网格切裂。
TEST(Vmc4EncryptProgramTest, LazyChunkingKeepsOversizedSwitchInstructionWhole)
{
    Vmc4Options opts;
    opts.seed                 = 0x5717C;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    VmInstance instance(opts);
    std::string reason;
    // 24 个 case：Switch 指令 = 6 + 24*6 = 150 字节，远超 64 字节目标块。
    std::vector<Label*> labels;
    for (int i = 0; i < 24; ++i) labels.push_back(labelScratchArena().create<Label>());
    std::vector<AbstractInsnNode*> insns;
    insns.push_back(new VarInsnNode(Opcodes::ILOAD, 0));
    insns.push_back(new TableSwitchInsnNode(0, 23, labels[0],
                                            std::vector<Label*>(labels.begin(), labels.end())));
    for (int i = 0; i < 24; ++i) {
        insns.push_back(new LabelNode(labels[i]));
        insns.push_back(new InsnNode(Opcodes::ICONST_0));
        insns.push_back(new InsnNode(Opcodes::IRETURN));
    }
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "bigSwitch", "(I)I", insns,
                         1, 1);
    auto prog = encodeMethodNode(std::move(mn), instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_FALSE(prog->instructionOffsets.empty());
    // Switch 指令长度（起点到下一条指令起点）必须超过块目标大小。
    uint32_t switchLen = 0;
    for (size_t i = 0; i + 1 < prog->instructionOffsets.size(); ++i) {
        const uint32_t len = prog->instructionOffsets[i + 1] - prog->instructionOffsets[i];
        if (len > switchLen) switchLen = len;
    }
    ASSERT_GT(switchLen, 64u) << "fixture switch instruction must exceed one chunk";

    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "bs1");
    std::vector<uint32_t> ends;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_cend_bs1", ends));
    // 该超长指令必须独占一个块（块长 = 指令长 > 64）。
    bool wholeInOneChunk = false;
    for (size_t c = 0; c < ends.size(); ++c) {
        const uint32_t begin = (c == 0) ? 0u : ends[c - 1];
        if (ends[c] - begin == switchLen) wholeInOneChunk = true;
    }
    EXPECT_TRUE(wholeInOneChunk)
        << "oversized instruction must occupy a single dedicated chunk (ends: "
        << testing::PrintToString(ends) << ")";
}

// 14. backend 级：lazy 开启时 JNI_OnLoad 无解密调用，懒解密基础设施齐全，
//     同配置确定性。
TEST(Vmc4EncryptProgramTest, BackendLazyDecryptSkipsJniOnLoadAndIsDeterministic)
{
    Vmc4Options opts;
    opts.seed                 = 0x2C;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    Vmc4Backend backend;
    backend.setOptions(opts);
    const std::string tag = codegen::hex64(codegen::fnv1a64("LazyProd.add(II)I"));
    ASSERT_FALSE(compileOneMethod(backend, "LazyProd", "add").empty());
    backend.finalize(CompilerIndex{});
    const std::string full = backend.generatedSource();
    // JNI_OnLoad 不发整体解密（块保持密文到首次执行）。
    EXPECT_EQ(full.find("mj2x_vm_blob_decrypt"), std::string::npos) << full;
    // 懒解密基础设施在 finalize 产物中齐备。
    EXPECT_NE(full.find("static void mj2x_vm_lazy_touch_" + tag), std::string::npos) << full;
    EXPECT_NE(full.find("mj2x_vm_cstate_" + tag), std::string::npos) << full;
    EXPECT_NE(full.find("mj2x_vm_cend_" + tag), std::string::npos) << full;
    // 确定性：同 seed 同输入 → finalize 产物逐字节一致。
    Vmc4Backend again;
    again.setOptions(opts);
    ASSERT_FALSE(compileOneMethod(again, "LazyProd", "add").empty());
    again.finalize(CompilerIndex{});
    EXPECT_EQ(again.generatedSource(), full);
}

// 15. 块密钥派生：确定性 / 恒非 0 / 块下标与主密钥敏感。
TEST(Vmc4EncryptProgramTest, ChunkKeyDerivationIsDeterministicNonZeroAndSensitive)
{
    EXPECT_EQ(vmc4DeriveChunkKey(0x12345678u, 0), vmc4DeriveChunkKey(0x12345678u, 0));
    EXPECT_EQ(vmc4DeriveChunkKey(0u, 7u), vmc4DeriveChunkKey(0u, 7u));
    for (uint32_t k : {0u, 1u, 0x9E3779B9u, 0xFFFFFFFFu}) {
        for (uint32_t c = 0; c < 64; ++c) {
            EXPECT_NE(vmc4DeriveChunkKey(k, c), 0u)
                << "chunk key must never be 0 (key=" << k << ", chunk=" << c << ")";
            EXPECT_NE(vmc4DeriveChunkKey(k, c), vmc4DeriveChunkKey(k, c + 1u))
                << "chunk key must be index sensitive (key=" << k << ", chunk=" << c << ")";
            EXPECT_NE(vmc4DeriveChunkKey(k, c), vmc4DeriveChunkKey(k ^ 0xA5A5A5A5u, c))
                << "chunk key must be blob-key sensitive (key=" << k << ", chunk=" << c << ")";
        }
    }
}

// 16. lazy 与 drift 叠加：块级解密还原的是 drift 编码流（两者正交）。
TEST(Vmc4EncryptProgramTest, LazyDecryptComposesWithDrift)
{
    Vmc4Options opts;
    opts.seed                 = 0x7778;
    opts.aliasCount           = 2;
    opts.driftVariants        = 2;
    opts.driftPeriod          = 8;
    opts.programBlobEncrypt   = true;
    opts.programLazyDecrypt   = true;
    VmInstance instance(opts);
    std::string reason;
    auto* loop = new LabelNode(labelScratchArena().create<Label>());
    auto* done = new LabelNode(labelScratchArena().create<Label>());
    auto prog = encodeMethod("driftLazy", "(I)I",
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
                                 new IincInsnNode(2, 1),
                                 new JumpInsnNode(Opcodes::GOTO, loop->label),
                                 done,
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2, 3, instance, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, instance, "dl1");
    // drift 结构不受影响，仍按明文流语义发射。
    EXPECT_NE(src.find("mj2x_vm_unmap_dl1_0"), std::string::npos) << src;
    std::vector<uint8_t> cipher;
    ASSERT_TRUE(extractArrayBytesForTest(src, "mj2x_vm_code_dl1", cipher));
    ASSERT_EQ(cipher.size(), prog->bytecodes.size());
    std::vector<uint32_t> ends;
    ASSERT_TRUE(extractU32ArrayForTest(src, "mj2x_vm_cend_dl1", ends));
    const uint32_t blobKey = vmc4DeriveBlobKey(instance.instanceSeed());
    uint32_t       begin   = 0;
    for (size_t c = 0; c < ends.size(); ++c) {
        std::vector<uint8_t> chunk(cipher.begin() + begin, cipher.begin() + ends[c]);
        xorDecryptForTest(chunk, chunkKeyForTest(blobKey, static_cast<uint32_t>(c)));
        for (size_t i = 0; i < chunk.size(); ++i) cipher[begin + i] = chunk[i];
        begin = ends[c];
    }
    EXPECT_EQ(cipher, prog->bytecodes);
}

// 17. trace surface / 实例 seed 不受 lazy 开关影响（红队契约字段集稳定）。
TEST(Vmc4EncryptProgramTest, TraceSurfaceAndInstanceSeedUnaffectedByLazyFlag)
{
    Vmc4Options plain;
    plain.seed = 11;
    Vmc4Options lazy = plain;
    lazy.programBlobEncrypt = true;
    lazy.programLazyDecrypt = true;
    VmInstance a(plain);
    VmInstance b(lazy);
    EXPECT_EQ(a.describe(), b.describe());
    EXPECT_EQ(a.instanceSeed(), b.instanceSeed());
    // lazy 无加密 → 归一为关闭（与既有 OptionFlowsIntoInstance 对应）。
    Vmc4Options lazyOnly;
    lazyOnly.programLazyDecrypt = true;
    VmInstance c(lazyOnly);
    EXPECT_FALSE(c.blobLazy());
}

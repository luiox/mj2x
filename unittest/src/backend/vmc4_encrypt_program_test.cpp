// VMC4 程序 blob at-rest 加密单测（issue #760 首切片，--vmc4-encrypt-program）。
//
// 本测试不调用 JNI/DLL（E2E 语义闭环由 vmc4_backend_e2e_test 的
// ProgramBlobEncryptRoundTrip 覆盖），只验证发射文本与派生函数：
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

#include "garena/arena.hpp"
#include <gmock/gmock.h>

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

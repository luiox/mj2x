/// @file strings_encrypt_test.cpp
/// @brief --strings-encrypt 字符串面 at-rest 加密单测（StackC 池 / RegC 字面量）。
///
/// 覆盖三面：
/// 1. 关闭态（默认）产物与既有形态字节级一致（硬要求：const 池 + 明文字面量，
///    且无解密调用注入）。
/// 2. 开启态产物无明文泄漏（StackC：密文池 + 解密调用注入；RegC：ldc_string
///    改密文缓冲引用 + JNI_OnLoad 批量解密）。
/// 3. 发射端加密与 rt 侧解密互逆（经渲染产物反解 roundtrip；rt 侧 C 实现与
///    发射端为同一 xorshift32 算法的镜像，exec 级互证由 E2E 承担）。
#include <gmock/gmock.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/frontend/code_converter.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                       int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto                                mn = std::make_unique<MethodNode>(access,
                                           ca::str::ZUtf8StringRef::from_static(name),
                                           ca::str::ZUtf8StringRef::from_static(desc),
                                           ca::str::ZUtf8StringRef::from_static(""),
                                           noExcepts);
    mn->maxStack                           = maxStack;
    mn->maxLocals                          = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

size_t countSubstring(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

/// 提取 StackC 池定义体（"_mj2x_string_pool[] = {" 到 "}; " 之间）并去掉全部
/// 空白，得到规范化 token 序列——同明文池必然同序列，可作关闭态基线反证。
std::string normalizedStringPoolBody(const std::string& source)
{
    const auto begin = source.find("_mj2x_string_pool[] = {");
    if (begin == std::string::npos) return {};
    const auto bodyStart = source.find('{', begin) + 1;
    const auto bodyEnd   = source.find("};", bodyStart);
    std::string body     = source.substr(bodyStart, bodyEnd - bodyStart);
    body.erase(std::remove_if(body.begin(),
                              body.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               body.end());
    return body;
}

/// 收集全部字符串池消费点的 offset/length 文本（"_mj2x_string_pool + N), L)"），
/// 开/关加密两态语义等价 ⇒ 两集合完全一致。
std::vector<std::string> collectPoolCallSites(const std::string& source)
{
    std::vector<std::string> sites;
    const std::string        marker = "_mj2x_string_pool + ";
    size_t                   pos    = 0;
    const auto               digitAt = [&source](size_t i) {
        return i < source.size() && std::isdigit(static_cast<unsigned char>(source[i])) != 0;
    };
    while ((pos = source.find(marker, pos)) != std::string::npos) {
        size_t d = pos + marker.size();
        while (digitAt(d)) ++d;
        // 常量发射形态：`...(_mj2x_string_pool + N), L)`。
        if (d + 2 < source.size() && source[d] == ')' && source[d + 1] == ',' && source[d + 2] == ' ') {
            size_t e = d + 3;
            while (digitAt(e)) ++e;
            if (e < source.size() && source[e] == ')') {
                sites.push_back(source.substr(pos, e - pos));
                pos = e;
                continue;
            }
        }
        sites.push_back(source.substr(pos, d - pos));   // 兜底：仅记录 offset 段
        pos = d;
    }
    std::sort(sites.begin(), sites.end());
    return sites;
}

/// rt 侧解密算法镜像（mj2x_regc_runtime.c::_mj2x_regc_decrypt_bytes /
/// mj2x_stackc_runtime.c::mj2x_stackc_string_pool_decrypt 同式）。
std::uint32_t testXorshift32(std::uint32_t state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

std::string decryptBytes(const std::vector<std::uint8_t>& cipher, std::uint32_t seed)
{
    std::uint32_t state = (seed != 0u) ? seed : 0x9E3779B9u;
    std::string   out;
    out.reserve(cipher.size());
    for (const std::uint8_t byte : cipher) {
        state = testXorshift32(state);
        out.push_back(static_cast<char>(byte ^ static_cast<std::uint8_t>(state & 0xFFu)));
    }
    while (!out.empty() && out.back() == '\0') out.pop_back();   // 去掉结尾 NUL
    return out;
}

std::string trimCopy(const std::string& text)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto       begin   = text.begin();
    while (begin != text.end() && isSpace(static_cast<unsigned char>(*begin))) ++begin;
    auto end = text.end();
    while (end != begin && isSpace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

/// RegCCachePool::renderEncryptedLiterals() 产物反解析（name/密文字节/seed）。
struct RenderedLiteral
{
    std::string               name;
    std::vector<std::uint8_t> bytes;
    std::uint32_t             seed = 0;
};

std::vector<RenderedLiteral> parseRenderedLiterals(const std::string& defs)
{
    std::vector<RenderedLiteral> out;
    size_t                       pos = 0;
    static const char            kDeclHead[] = "static char ";
    while ((pos = defs.find(kDeclHead, pos)) != std::string::npos) {
        RenderedLiteral entry;
        const size_t nameStart = pos + (sizeof(kDeclHead) - 1);
        const size_t nameEnd   = defs.find('[', nameStart);
        entry.name             = defs.substr(nameStart, nameEnd - nameStart);
        const size_t open      = defs.find('{', nameEnd);
        const size_t close     = defs.find('}', open);
        const std::string body = defs.substr(open + 1, close - open - 1);
        size_t            i    = 0;
        while (i < body.size()) {
            size_t j = body.find(',', i);
            if (j == std::string::npos) j = body.size();
            const auto token = trimCopy(body.substr(i, j - i));
            if (!token.empty())
                entry.bytes.push_back(static_cast<std::uint8_t>(std::stoi(token)));
            i = j + 1;
        }
        const size_t seedMark = defs.find("seed=0x", close);
        entry.seed = static_cast<std::uint32_t>(std::stoul(defs.substr(seedMark + 7, 8), nullptr, 16));
        out.push_back(std::move(entry));
        pos = close;
    }
    return out;
}

/// 编译一个 RegC 字面量返回方法并 finalize，返回完整 TU 源。
std::string compileRegCGreeter(const std::string& literal, bool encrypt)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "greet",
                             "()Ljava/lang/String;",
                             {
                                 new LdcInsnNode(literal),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             1,
                             0);
    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    if (encrypt) backend.setStringsEncrypt(true);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*method);
    auto result = backend.compileMethod("StrTarget", cls.methods[0], blocks);
    EXPECT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    return backend.generatedSource();
}

}   // namespace

// ============================================================
// StackC：UTF-16 池加密
// ============================================================

TEST(StringsEncryptTest, StackCDisabledDefaultEmitsPlainConstPool)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "greet",
                             "()Ljava/lang/String;",
                             {
                                 new LdcInsnNode(std::string("PLAIN_MARKER")),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             2,
                             0);
    CompilerMethod cm;
    cm.methodNode       = method.get();
    cm.markedForCompile = true;

    StackCBackend defaultBackend;
    StackCBackend explicitOffBackend;
    explicitOffBackend.setStringsEncrypt(false);

    ca::str::Utf8StringArena arena;
    for (auto* backend : {&defaultBackend, &explicitOffBackend}) {
        CodeConverter converter(arena);
        auto          blocks = converter.convert(*method);
        backend->compileMethod("StrTarget", cm, blocks);
        CompilerIndex emptyIndex;
        backend->finalize(emptyIndex);
    }

    const auto& defaultSource = defaultBackend.generatedSource();
    // 关闭态硬要求：明文 const 池 + 无解密调用，且显式 setStringsEncrypt(false)
    // 与不调用 setter 产物逐字节一致。
    EXPECT_NE(defaultSource.find("static const uint16_t _mj2x_string_pool[] = {"),
              std::string::npos)
        << defaultSource;
    EXPECT_EQ(defaultSource.find("mj2x_stackc_string_pool_decrypt"), std::string::npos)
        << defaultSource;
    EXPECT_EQ(defaultSource, explicitOffBackend.generatedSource());
}

TEST(StringsEncryptTest, StackCEncryptedPoolHidesPlaintextUnits)
{
    const std::string literal = "SECRET_POOL_MARKER_LITERAL";
    auto              method  = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "greet",
                             "()Ljava/lang/String;",
                             {
                                 new LdcInsnNode(literal),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             2,
                             0);
    CompilerMethod cm;
    cm.methodNode       = method.get();
    cm.markedForCompile = true;

    StackCBackend plainBackend;
    StackCBackend encryptedBackend;
    encryptedBackend.setStringsEncrypt(true);

    ca::str::Utf8StringArena arena;
    CodeConverter            plainConverter(arena);
    auto                     plainBlocks = plainConverter.convert(*method);
    plainBackend.compileMethod("StrTarget", cm, plainBlocks);
    CompilerIndex plainIndex;
    plainBackend.finalize(plainIndex);

    CodeConverter            encConverter(arena);
    auto                     encBlocks = encConverter.convert(*method);
    encryptedBackend.compileMethod("StrTarget", cm, encBlocks);
    CompilerIndex encIndex;
    encryptedBackend.finalize(encIndex);

    const auto& plainSource = plainBackend.generatedSource();
    const auto& encSource   = encryptedBackend.generatedSource();

    // 开启态：非 const 池（原地解密）+ JNI_OnLoad 解密调用注入。
    EXPECT_NE(encSource.find("static uint16_t _mj2x_string_pool[] = {"), std::string::npos)
        << encSource;
    EXPECT_NE(encSource.find("mj2x_stackc_string_pool_decrypt(_mj2x_string_pool, "),
              std::string::npos)
        << encSource;

    // 密文基线反证：关闭态池的规范化 token 序列不得出现在开启态（加密后逐单位
    // 变化；池序列相同即视为明文泄漏）。
    const auto plainBody = normalizedStringPoolBody(plainSource);
    const auto encBody   = normalizedStringPoolBody(encSource);
    ASSERT_FALSE(plainBody.empty());
    EXPECT_NE(plainBody, encBody);
    EXPECT_EQ(encSource.find(plainBody), std::string::npos);

    // 语义不变量：池消费点（offset/length 文本）两态完全一致。
    EXPECT_EQ(collectPoolCallSites(plainSource), collectPoolCallSites(encSource));
}

// ============================================================
// RegC：LDC 字面量 / indy bsm 字符串加密
// ============================================================

TEST(StringsEncryptTest, RegCDisabledKeepsPlaintextLiteral)
{
    const auto source = compileRegCGreeter("PLAIN_REGC_MARKER", false);
    EXPECT_NE(source.find("\"PLAIN_REGC_MARKER\""), std::string::npos) << source;
    EXPECT_EQ(source.find("static char _mj2x_str_"), std::string::npos) << source;
    EXPECT_EQ(source.find("_mj2x_regc_decrypt_bytes((uint8_t*)"), std::string::npos) << source;
}

TEST(StringsEncryptTest, RegCEncryptedLiteralHidesPlaintextAndInjectsDecrypt)
{
    const std::string literal = "SECRET_REGC_MARKER_LITERAL";
    const auto        source  = compileRegCGreeter(literal, true);

    // 明文（含 C 转义形态拆分后的特征片段）不得出现。
    EXPECT_EQ(source.find(literal), std::string::npos) << source;

    // 调用点改密文缓冲引用；TU 头部有密文缓冲定义；JNI_OnLoad 注入批量解密。
    EXPECT_NE(source.find("_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), "
                          "_mj2x_str_"),
              std::string::npos)
        << source;
    EXPECT_NE(source.find("static char _mj2x_str_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_decrypt_bytes((uint8_t*)_mj2x_str_"), std::string::npos)
        << source;

    // 无栈锚点结构门保持通过（加密面不引入栈形态）。
    std::string reason;
    EXPECT_TRUE(validateRegCNoStackAnchors(source, reason)) << reason << "\n" << source;
}

TEST(StringsEncryptTest, RegCEncryptedLiteralDedupesByContent)
{
    // 同内容字面量两个调用点 → 单个密文缓冲定义 + 两处引用。
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "pick",
                             "(I)Ljava/lang/String;",
                             {
                                 new LdcInsnNode(std::string("SHARED_SECRET")),
                                 new InsnNode(Opcodes::ARETURN),
                             },
                             1,
                             1);
    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    backend.setStringsEncrypt(true);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*method);
    backend.compileMethod("StrTarget", cls.methods[0], blocks);
    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();

    EXPECT_EQ(countSubstring(source, "static char _mj2x_str_"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), "
                                     "_mj2x_str_"),
              1u)
        << source;
    EXPECT_EQ(countSubstring(source, "_mj2x_regc_decrypt_bytes((uint8_t*)_mj2x_str_"), 1u) << source;
}

TEST(StringsEncryptTest, RegCLiteralCipherRoundtripsViaRenderedArtifact)
{
    const std::string content = "roundtrip_payload_UTF16_multibyte_\xE9\x9D\xA2";   // 含多字节 UTF-8

    // 未启用时渲染面为空（发射路径也不会调用登记）。
    RegCCachePool disabledPool;
    EXPECT_FALSE(disabledPool.stringsEncryptEnabled());
    EXPECT_TRUE(disabledPool.renderEncryptedLiterals().empty());
    EXPECT_TRUE(disabledPool.renderEncryptedLiteralDecryptCalls().empty());

    // seedBase=0：渲染产物含密文与 seed；按 rt 侧同式解密必须还原明文。
    RegCCachePool pool;
    pool.setStringsEncrypt(true, 0);
    const auto nameA = pool.encryptedLiteralBuffer(content);
    const auto defs  = pool.renderEncryptedLiterals();
    const auto calls = pool.renderEncryptedLiteralDecryptCalls();
    ASSERT_FALSE(defs.empty());
    EXPECT_THAT(nameA, testing::StartsWith("_mj2x_str_"));

    auto entries = parseRenderedLiterals(defs);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, nameA);
    // 密文长度 = 明文字节数 + 结尾 NUL；解密调用行与缓冲名/长度一致。
    EXPECT_EQ(entries[0].bytes.size(), content.size() + 1);
    EXPECT_NE(calls.find("_mj2x_regc_decrypt_bytes((uint8_t*)" + nameA + ", "), std::string::npos);
    EXPECT_EQ(decryptBytes(entries[0].bytes, entries[0].seed), content);

    // 同内容再登记：去重返回同名，不新增缓冲。
    EXPECT_EQ(pool.encryptedLiteralBuffer(content), nameA);
    EXPECT_EQ(parseRenderedLiterals(pool.renderEncryptedLiterals()).size(), 1u);

    // 不同 seedBase：密文不同、缓冲名不变（名只由内容派生），解密仍还原明文。
    RegCCachePool seededPool;
    seededPool.setStringsEncrypt(true, 0x9E3779B97F4A7C15ULL);
    const auto nameB = seededPool.encryptedLiteralBuffer(content);
    EXPECT_EQ(nameB, nameA);
    auto seededEntries = parseRenderedLiterals(seededPool.renderEncryptedLiterals());
    ASSERT_EQ(seededEntries.size(), 1u);
    EXPECT_NE(seededEntries[0].bytes, entries[0].bytes);
    EXPECT_EQ(decryptBytes(seededEntries[0].bytes, seededEntries[0].seed), content);
}

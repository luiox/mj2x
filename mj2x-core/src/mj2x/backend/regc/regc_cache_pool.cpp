/// @file regc_cache_pool.cpp
/// @brief RegCCachePool 实现：生成 C 翻译单元的共享 cache 声明池。
///
/// @details
/// RegCEmitter 在无池模式（cachePool == nullptr）下发射函数局部 cache 声明；
/// RegCBackend 提供一个池让整个生成 C 文件的相同 owner/name/desc 或类描述符
/// 共享同一个 static cache 对象。本文件只实现池本身，与发射路径解耦。
#include "regc_emitter.hpp"

#include "libca/str/format.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mj2x {

// 与 regc_emitter.cpp 一致：CSourceStream 从 codegen 起别名，让既有的
// 调用点保持一行；cStringLiteral 以 codegen:: 限定形式调用。
using codegen::CSourceStream;

namespace {

std::string poolCacheKey(const ca::str::Utf8StringRef& first, const ca::str::Utf8StringRef& second,
                         const ca::str::Utf8StringRef& third)
{
    return first.to_std_string() + "\n" + second.to_std_string() + "\n" + third.to_std_string();
}

}   // namespace

ca::str::Utf8String RegCCachePool::staticMethodCache(const ca::str::Utf8StringRef& owner,
                                                      const ca::str::Utf8StringRef& name,
                                                      const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticMethodCacheNames_.find(key);
    if (it != staticMethodCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_cache_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", staticMethodCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: static ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(owner));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(name));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    staticMethodCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::instanceMethodCache(const ca::str::Utf8StringRef& owner,
                                                       const ca::str::Utf8StringRef& name,
                                                       const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceMethodCacheNames_.find(key);
    if (it != instanceMethodCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_instance_cache_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", instanceMethodCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: instance ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_instance_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(owner));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(name));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(desc));
    decl.append(", NULL, NULL, NULL, NULL};\n");
    declarations_.push_back(decl.build());
    instanceMethodCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::resolvedCallSite(const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = resolvedCallSiteNames_.find(key);
    if (it != resolvedCallSiteNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_resolved_site_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", resolvedCallSiteIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: resolved call site ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_resolved_call_site_t ");
    decl.append(cacheName);
    decl.append(" = {NULL, NULL, NULL, NULL};\n");
    declarations_.push_back(decl.build());
    resolvedCallSiteNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::staticFieldCache(const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticFieldCacheNames_.find(key);
    if (it != staticFieldCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_field_cache_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", staticFieldCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: static field ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(owner));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(name));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    staticFieldCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::instanceFieldCache(const ca::str::Utf8StringRef& owner,
                                                      const ca::str::Utf8StringRef& name,
                                                      const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceFieldCacheNames_.find(key);
    if (it != instanceFieldCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_instance_field_cache_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", instanceFieldCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: instance field ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_instance_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(owner));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(name));
    decl.append(", ");
    decl.append(codegen::cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    instanceFieldCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::classCache(const ca::str::Utf8StringRef& desc)
{
    auto       it  = classCacheNames_.find(desc.to_std_string());
    if (it != classCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_class_cache_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", classCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: class ");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_class_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(desc));
    decl.append(", NULL};\n");
    declarations_.push_back(decl.build());
    classCacheNames_.emplace(desc.to_std_string(), cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::enumValuesSite(const ca::str::Utf8StringRef& owner)
{
    const std::string key = owner.to_std_string();
    auto              it  = enumValuesSiteNames_.find(key);
    if (it != enumValuesSiteNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_enum_values_site_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", enumValuesSiteIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: enum values() clone ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_enum_values_site ");
    decl.append(cacheName);
    decl.append(" = {NULL, NULL};\n");
    declarations_.push_back(decl.build());
    enumValuesSiteNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::enumValueOfSite(const ca::str::Utf8StringRef& owner)
{
    const std::string key = owner.to_std_string();
    auto              it  = enumValueOfSiteNames_.find(key);
    if (it != enumValueOfSiteNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_enum_valueof_site_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", enumValueOfSiteIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: enum valueOf ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_enum_valueof_site ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(codegen::cStringLiteral(owner));
    decl.append(", NULL, NULL, NULL};\n");
    declarations_.push_back(decl.build());
    enumValueOfSiteNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::classNameHashSite()
{
    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_cnh_site_tu_");
    cacheNameBuilder.append(ca::str::format_std("{}", classNameHashSiteIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: getClass().getName().hashCode() fused\n");
    }
    decl.append("static mj2x_regc_class_name_hash_site ");
    decl.append(cacheName);
    decl.append(" = {{{NULL, 0}, {NULL, 0}}};\n");
    declarations_.push_back(decl.build());
    return cacheName;
}

std::string RegCCachePool::renderDeclarations() const
{
    CSourceStream out;
    for (const auto& declaration : declarations_) {
        out.append(declaration);
    }
    if (!declarations_.empty()) out.append("\n");
    return out.build().to_std_string();
}

// ============================================================
// 字符串字面量 at-rest 加密（--strings-encrypt）
// ============================================================

namespace {

/// xorshift32 一步推进（与 core/native_lib_crypto.hpp::xorshift32Step 同式，
/// 亦与 mj2x_regc_runtime 侧解密实现一致）。
inline std::uint32_t regcStringXorshift32(std::uint32_t state)
{
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/// 每字面量密钥派生：内容 fnv1a64 折叠到 u32，再混入 seedBase（RegC 链路取
/// RegIR 变换 transformSeed，CLI 侧已按 --client-id 派生）与其高位；0 回退
/// 固定种子（与 xorStreamBytes 的 key==0 约定一致）。确定性、可复现构建。
std::uint32_t deriveLiteralSeed(std::uint64_t contentHash, std::uint64_t seedBase)
{
    const std::uint64_t mixed = contentHash ^ (seedBase * 0x9E3779B97F4A7C15ULL);
    std::uint32_t       seed =
        static_cast<std::uint32_t>(mixed ^ (mixed >> 32)) ^ static_cast<std::uint32_t>(seedBase);
    if (seed == 0u) seed = 0x9E3779B9u;
    return seed;
}

}   // namespace

void RegCCachePool::setStringsEncrypt(bool enabled, std::uint64_t seedBase)
{
    stringsEncrypt_         = enabled;
    stringsEncryptSeedBase_ = seedBase;
}

std::string RegCCachePool::encryptedLiteralBuffer(const std::string& content)
{
    auto it = encryptedLiteralNames_.find(content);
    if (it != encryptedLiteralNames_.end()) return it->second;

    // 缓冲名按内容哈希派生（与登记序无关，多调用点/多方法自然去重共享）。
    const std::uint64_t contentHash = codegen::fnv1a64(content);
    EncryptedLiteral    entry;
    entry.name = "_mj2x_str_" + codegen::hex64(contentHash);
    entry.seed = deriveLiteralSeed(contentHash, stringsEncryptSeedBase_);

    // 内容 + 结尾 NUL 一起进密钥流：解密后即合法 C 串（NewStringUTF 直接消费）。
    entry.cipher.resize(content.size() + 1);
    if (!content.empty()) {
        std::memcpy(entry.cipher.data(), content.data(), content.size());
    }
    entry.cipher[content.size()] = 0u;

    std::uint32_t state = entry.seed;
    for (auto& byte : entry.cipher) {
        state = regcStringXorshift32(state);
        byte  = static_cast<std::uint8_t>(byte ^ static_cast<std::uint8_t>(state & 0xFFu));
    }

    encryptedLiteralNames_.emplace(content, entry.name);
    encryptedLiterals_.push_back(std::move(entry));
    return encryptedLiteralNames_[content];
}

std::string RegCCachePool::renderEncryptedLiterals() const
{
    if (encryptedLiterals_.empty()) return {};
    // 密文数组以有符号 char 十进制发射（值域 [-128,127]），避免符号警告；
    // 单数组多行发射。换行交界必须先补 "," 再起新行——initializer list 里
    // "45\n    -12" 缺逗号会被 C 折叠为常量表达式 45-12，静默改写数组内容。
    CSourceStream out;
    // 生成 TU 内注释保持 ASCII（MSVC codepage 安全，对齐 mj2x_regc_runtime.c 约定）。
    out.append("/* mj2x strings-encrypt: literal ciphertext buffers, batch-decrypted in JNI_OnLoad */\n");
    for (const auto& entry : encryptedLiterals_) {
        out.append("static char ");
        out.append(entry.name);
        out.append("[] = {");
        std::string line;
        bool        lineFirst = true;
        for (size_t i = 0; i < entry.cipher.size(); ++i) {
            if (!lineFirst) line += ", ";
            lineFirst = false;
            line += std::to_string(static_cast<int>(static_cast<signed char>(entry.cipher[i])));
            if (line.size() > 72) {
                out.append(line);
                out.append(",\n    ");
                line.clear();
                lineFirst = true;
            }
        }
        out.append(line);
        out.append("}; /* seed=");
        out.append(ca::str::format_std("0x{:08x}", entry.seed));
        out.append(" bytes=");
        out.append(ca::str::format_std("{}", entry.cipher.size()));
        out.append(" */\n");
    }
    return out.build().to_std_string();
}

std::string RegCCachePool::renderEncryptedLiteralDecryptCalls() const
{
    if (encryptedLiterals_.empty()) return {};
    // 显式字节长度（多文件模式 entry.c 仅见 extern 不完整数组，不能用 sizeof）。
    std::string out =
        "/* mj2x strings-encrypt: batch literal decrypt (ciphertext at-rest), once in JNI_OnLoad */\n";
    for (const auto& entry : encryptedLiterals_) {
        out += "_mj2x_regc_decrypt_bytes((uint8_t*)" + entry.name + ", " +
               ca::str::format_std("{}u", entry.cipher.size()) + ", " +
               ca::str::format_std("0x{:08x}u", entry.seed) + ");\n";
    }
    return out;
}

RegCCachePool::RegCCachePool(RegCCommentLevel commentLevel)
    : commentLevel_(commentLevel)
{}

bool RegCCachePool::empty() const
{
    return declarations_.empty();
}

}   // namespace mj2x

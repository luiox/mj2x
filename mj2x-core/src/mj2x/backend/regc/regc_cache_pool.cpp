/// @file regc_cache_pool.cpp
/// @brief RegCCachePool 实现：生成 C 翻译单元的共享 cache 声明池。
///
/// @details
/// RegCEmitter 在无池模式（cachePool == nullptr）下发射函数局部 cache 声明；
/// RegCBackend 提供一个池让整个生成 C 文件的相同 owner/name/desc 或类描述符
/// 共享同一个 static cache 对象。本文件只实现池本身，与发射路径解耦。
#include "regc_emitter.hpp"

#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

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
    cacheNameBuilder.append(std::to_string(staticMethodCacheIndex_++));
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
    cacheNameBuilder.append(std::to_string(instanceMethodCacheIndex_++));
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
    cacheNameBuilder.append(std::to_string(resolvedCallSiteIndex_++));
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
    cacheNameBuilder.append(std::to_string(staticFieldCacheIndex_++));
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
    cacheNameBuilder.append(std::to_string(instanceFieldCacheIndex_++));
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
    cacheNameBuilder.append(std::to_string(classCacheIndex_++));
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

std::string RegCCachePool::renderDeclarations() const
{
    CSourceStream out;
    for (const auto& declaration : declarations_) {
        out.append(declaration);
    }
    if (!declarations_.empty()) out.append("\n");
    return out.build().to_std_string();
}

RegCCachePool::RegCCachePool(RegCCommentLevel commentLevel)
    : commentLevel_(commentLevel)
{}

bool RegCCachePool::empty() const
{
    return declarations_.empty();
}

}   // namespace mj2x
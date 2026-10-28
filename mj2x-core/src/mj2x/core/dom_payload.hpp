#pragma once

/// @file        dom_payload.hpp
/// @brief       DOM CP → 树契约同型载荷的共享解析面（code_converter /
///              invokedynamic preprocessor 共用，批 4b 收口单源）。
///
/// 批 6e 门面收口：Any 载荷一律 mj2x 自有类型——Class/MethodType 走
/// mj2x::Type、MethodHandle 走 mj2x::Handle、字符串视图必须 intern 进
/// 作业 arena（DOM CP 存储生命周期独立，裸视图会悬垂）。
/// ConstantDynamic 不支持（fail loud）。

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "libca/core/any.hpp"
#include "libca/core/tag_cast.hpp"

#include "mj2x/core/jvm_type.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mj2x {

/// @brief Utf8 条目内容视图；索引无效即抛（畸形类文件不静默）。
inline ca::str::Utf8StringRef domUtf8(const jbc::core::ClassFile& dom, ca::u32 idx)
{
    if (!dom.cp.valid(idx)) {
        throw std::invalid_argument("dom_payload: bad CP utf8 index " + std::to_string(idx));
    }
    return dom.cp.utf8Ref(idx);
}

/// @brief CP 来源内容 intern：必须走 intern_raw（modified UTF-8 含孤代理对/
/// C0 80 一类序列不是合法标准 UTF-8，intern 校验失败返回空视图——TextUnicode
/// 代理对常量整条变空首曝）。CP 内容按 JVMS 恒为 modified UTF-8，与 jbc
/// addUtf8 的 intern_raw 口径一致。
inline ca::str::Utf8StringRef internCpUtf8(ca::str::Utf8StringArena& arena,
                                           const ca::str::Utf8StringRef& ref)
{
    return arena.intern_raw(ref.data(), ref.byte_length());
}

/// @brief CP MethodHandle → 旧栈 Handle（引用视图 intern 进 arena）。
inline mj2x::Handle resolveDomHandle(jbc::core::ClassFile& dom, ca::u32 cpIdx,
                                                ca::str::Utf8StringArena& arena)
{
    const auto& entry = dom.cp.at(cpIdx);
    if (entry.tag != jbc::core::CpTag::MethodHandle) {
        throw std::invalid_argument("dom_payload: CP " + std::to_string(cpIdx)
                                    + " is not a MethodHandle");
    }
    const auto& ref = dom.cp.at(entry.b);      // Methodref / InterfaceMethodref
    const auto& nat = dom.cp.at(ref.b);
    return mj2x::Handle(entry.a, internCpUtf8(arena, domUtf8(dom, dom.cp.at(ref.a).a)),
                                   internCpUtf8(arena, domUtf8(dom, nat.a)),
                                   internCpUtf8(arena, domUtf8(dom, nat.b)),
                                   ref.tag == jbc::core::CpTag::InterfaceMethodref);
}

/// @brief CP 常量 → 树契约同型 Any 载荷（bsmArgs / ldc 载荷共用）。
///
/// 载荷类型对齐旧栈 ClassReader::readConst：Integer→i32、Float→float、
/// Long→i64、Double→double、String→Utf8StringRef、Class/MethodType→旧栈
/// Type、MethodHandle→旧栈 Handle。ConstantDynamic 不支持（fail loud）。
inline ca::core::Any resolveDomConstPayload(jbc::core::ClassFile& dom, ca::u32 cpIdx,
                                            ca::str::Utf8StringArena& arena)
{
    const auto& entry = dom.cp.at(cpIdx);
    switch (entry.tag) {
    case jbc::core::CpTag::Integer: return ca::core::Any(entry.ival);
    case jbc::core::CpTag::Float: {
        float f = 0.0f;
        static_assert(sizeof(f) == sizeof(entry.ival));
        std::memcpy(&f, &entry.ival, sizeof(f));
        return ca::core::Any(f);
    }
    case jbc::core::CpTag::Long: return ca::core::Any(entry.lval);
    case jbc::core::CpTag::Double: {
        double d = 0.0;
        static_assert(sizeof(d) == sizeof(entry.lval));
        std::memcpy(&d, &entry.lval, sizeof(d));
        return ca::core::Any(d);
    }
    // Any 载荷里的字符串视图一律先 intern（DOM CP 存储生命周期独立）。
    case jbc::core::CpTag::String: return ca::core::Any(internCpUtf8(arena, domUtf8(dom, entry.a)));
    case jbc::core::CpTag::Class:
        return ca::core::Any(
            mj2x::Type::getObjectType(internCpUtf8(arena, domUtf8(dom, entry.a))));
    case jbc::core::CpTag::MethodType:
        return ca::core::Any(
            mj2x::Type::getMethodType(internCpUtf8(arena, domUtf8(dom, entry.a))));
    case jbc::core::CpTag::MethodHandle: return ca::core::Any(resolveDomHandle(dom, cpIdx, arena));
    case jbc::core::CpTag::Utf8:
        // 旧栈 writer 对 bootstrap arg 的 std::string 载荷 intern 成 CONSTANT_Utf8。
        return ca::core::Any(internCpUtf8(arena, domUtf8(dom, cpIdx)));
    default:
        throw std::invalid_argument("dom_payload: unsupported CP tag "
                                    + std::to_string(static_cast<int>(entry.tag))
                                    + " in constant payload");
    }
}

/// @brief 类级 BootstrapMethods 属性（INVOKEDYNAMIC 的 bsm 解析用；lazy 解码）。
inline const jbc::core::BootstrapMethodsAttr* domBootstrapMethods(jbc::core::ClassFile& dom)
{
    for (auto& slot : dom.attrs) {
        if (!dom.cp.utf8Equals(slot.nameCp, "BootstrapMethods")) continue;
        if (slot.peek() != nullptr) {
            return ca::core::tag_cast::cast<jbc::core::BootstrapMethodsAttr>(slot.peek());
        }
        auto decoded = dom.decodeAttr(slot);
        if (decoded.is_err()) {
            throw std::invalid_argument("dom_payload: BootstrapMethods decode failed");
        }
        return ca::core::tag_cast::cast<jbc::core::BootstrapMethodsAttr>(
            std::move(decoded).unwrap());
    }
    return nullptr;
}

/// @brief 单个 indy 站点的解码载荷（name/desc/bsm/bsmArgs，树契约同型）。
/// Handle 无默认构造（旧栈契约），随构造注入。
struct DomIndySite
{
    std::string                name;
    std::string                desc;
    mj2x::Handle    bsm;
    std::vector<ca::core::Any> bsmArgs;

    DomIndySite(std::string n, std::string d, mj2x::Handle h)
        : name(std::move(n)), desc(std::move(d)), bsm(std::move(h))
    {
    }
};

/// @brief 解码 INVOKEDYNAMIC 站点载荷（cp 索引 + 类级 BootstrapMethods）。
inline DomIndySite decodeDomIndySite(jbc::core::ClassFile& dom, ca::u32 indyCp,
                                     const jbc::core::BootstrapMethodsAttr& bsmAttr,
                                     ca::str::Utf8StringArena& arena)
{
    const auto& entry = dom.cp.at(indyCp);
    if (entry.tag != jbc::core::CpTag::InvokeDynamic) {
        throw std::invalid_argument("dom_payload: CP " + std::to_string(indyCp)
                                    + " is not InvokeDynamic");
    }
    if (entry.a >= bsmAttr.entries.size()) {
        throw std::invalid_argument("dom_payload: INDY bootstrap method " + std::to_string(entry.a)
                                    + " not resolvable");
    }
    const auto& be  = bsmAttr.entries[entry.a];
    const auto& nat = dom.cp.at(entry.b);
    DomIndySite site(std::string(domUtf8(dom, nat.a)), std::string(domUtf8(dom, nat.b)),
                     resolveDomHandle(dom, be.methodRef, arena));
    for (ca::u32 argCp : be.args) {
        site.bsmArgs.push_back(resolveDomConstPayload(dom, argCp, arena));
    }
    return site;
}

}   // namespace mj2x

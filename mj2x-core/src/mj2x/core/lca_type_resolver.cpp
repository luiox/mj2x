#include "mj2x/core/lca_type_resolver.hpp"

#include "mj2x/core/hierarchy_supply.hpp"
#include "mj2x/core/jvm_opcodes.hpp"

namespace mj2x {

void LcaTypeResolver::buildFromIndex(const CompilerIndex& index)
{
    for (const auto& [name, cls] : index.getClasses()) {
        if (cls.dom == nullptr) continue;
        const auto& dom = *cls.dom;

        mjbase::hierarchy::ClassSummary s;
        s.self = mjbase::classKeyOf(pool_, dom.thisName());
        if (const std::string_view super = dom.superName(); !super.empty()) {
            s.super = mjbase::classKeyOf(pool_, super);
        }
        for (const ca::u16 itf : dom.interfaces) {
            s.interfaces.push_back(mjbase::classKeyOf(pool_, dom.className(itf)));
        }
        const auto added = hierarchy_.addClass(s);
        if (added.is_err()) {
            // DuplicateClass 不可能（index 按名唯一）；BadKey/Cycle 跳过该类，
            // 其涉及类型的合并落回硬编码表（其余类照常精确）。
            continue;
        }
    }
}

ca::usize LcaTypeResolver::extendFromEnvironment(std::string_view classpath, bool includeJdk,
                                                 std::string& note)
{
    ca::usize added = 0;
    if (!classpath.empty()) {
        added += hierarchy_supply::supplyClasspathSkeletons(hierarchy_, pool_, classpath, note);
    }
    if (includeJdk) {
        added += hierarchy_supply::supplyJdkSkeletons(hierarchy_, pool_, note);
    }
    // resolver 惰性创建（见头注：fallback 缓存不得吃图后扩的陈旧结果）。
    return added;
}

ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError> LcaTypeResolver::merge(
    jbc::analysis::VType a, jbc::analysis::VType b)
{
    using jbc::analysis::TypeMergeError;
    using jbc::analysis::VKind;
    // 结构性归并（与 StrictTypeResolverAdapter 同口径）：Null 作万能引用底。
    if (a.k == VKind::Null && b.k == VKind::Ref) return ca::core::Ok(b);
    if (b.k == VKind::Null && a.k == VKind::Ref) return ca::core::Ok(a);
    if (a.k != VKind::Ref || b.k != VKind::Ref) {
        return ca::core::Err(TypeMergeError::Unresolvable);
    }
    if (a.name.empty() || b.name.empty()) {
        return ca::core::Err(TypeMergeError::Unresolvable);
    }
    if (a.name == b.name) return ca::core::Ok(a);

    // 谱系内对 → 真 LCA。resolver 的 fallback（Object）只可能对「图内两链
    // 无交」触发——那与硬编码表兜底的语义一致，直接采信。resolver 惰性
    // 创建：merge 首调时图已定形（buildFromIndex/extendFromEnvironment 均完成）。
    if (resolver_ == nullptr) {
        resolver_ = std::make_unique<mjbase::spi::CommonSuperClassResolver>(hierarchy_);
    }
    const auto known = [this](std::string_view n) {
        return hierarchy_.findClass(n).valid();
    };
    if (known(a.name) && known(b.name)) {
        if (const std::string_view super = resolver_->getCommonSuperClass(a.name, b.name);
            !super.empty()) {
            return ca::core::Ok(jbc::analysis::VType::ref(super));
        }
    }
    // 任一侧未登记（JDK 类，M1 图只含输入 jar）：硬编码表回退，与旧行为一致。
    return fallback_.merge(a, b);
}

}   // namespace mj2x

#pragma once

#include <memory>

#include "jbc/analysis/domain.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/spi/common_super_resolver.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/frame_recompute.hpp"

namespace mj2x {

/// @brief LCA 帧类型解析器（帧换装，2026-10-29）。
///
/// @details
/// 旧口径 PortedCommonSuperResolver = 硬编码 JDK 父类链 + Object 兜底，对输入
/// jar 内的自定义谱系一律 Object——合并点下游对合并值直接 invokevirtual 时
/// （帧类型须是调用目标 owner 的子类型）VerifyError；Strict 不可算则整类
/// 写不出。本解析器把 mjbase 的类层次图（ClassHierarchy super 链最近公共
/// 祖先 + CommonSuperClassResolver 查询缓存）挂进 jbc 帧引擎：
///
/// - 两类型都在图内 → 真 LCA（精确到谱系，覆盖混淆器造出的任意自定义谱系）；
/// - 任一侧未登记 → 回退 PortedCommonSuperResolver 硬编码表（与旧行为逐位
///   一致）。M2 供给面（classpath jar + JDK jmods 扫描，带磁盘缓存）把 JDK
///   类型对也纳入图内，兜底面收窄到真正未知的类型。
///
/// 语义说明：mjbase 的 LCA 是 super-chain-only（接口维度不参与，与旧
/// writer 口径一致）；数组/未登记名走兜底链。成员不给帧用，这里不填。
class LcaTypeResolver final : public jbc::analysis::ITypeResolver
{
public:
    LcaTypeResolver()
        : hierarchy_(pool_)
    {}

    /// @brief 用 CompilerIndex 的全部类建图（输入 jar 谱系面）。
    ///
    /// @details 逐类从 dom 读 this/super/interfaces 手工构 ClassSummary
    ///（层次 LCA 只需要 super 链骨架，不扫成员）。畸形谱系（父链成环，
    /// 混淆器产物可能故意如此）addClass 报 Cycle 时跳过该类——其对涉及
    /// 类型的查询落回硬编码表，其余类照常精确。
    void buildFromIndex(const CompilerIndex& index);

    /// @brief 扩充谱系（M2 供给面）：classpath 依赖 jar + JDK jmods/rt.jar
    ///        扫描（JDK 侧带磁盘缓存）。
    ///
    /// @details 必须在首次 merge 前调用：CommonSuperClassResolver 对「当时
    /// 未登记」的对缓存 fallback 结果，图后扩会让缓存吃到陈旧 Object——
    /// resolver 因此惰性创建（merge 首调时图已定形）。
    /// @return 登记增量（诊断日志用）。
    ca::usize extendFromEnvironment(std::string_view classpath, bool includeJdk,
                                    std::string& note);

    ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError> merge(
        jbc::analysis::VType a, jbc::analysis::VType b) override;

    /// 已登记类数（测试/诊断用）。
    ca::usize registeredCount() const noexcept { return hierarchy_.classCount(); }

private:
    /// 成员序 = 析构序的逆：resolver 借用 hierarchy，hierarchy 借用 pool。
    mjbase::KeyPool                                        pool_;
    mjbase::hierarchy::ClassHierarchy                      hierarchy_;
    std::unique_ptr<mjbase::spi::CommonSuperClassResolver> resolver_;
    PortedCommonSuperResolver                              fallback_;
};

}   // namespace mj2x

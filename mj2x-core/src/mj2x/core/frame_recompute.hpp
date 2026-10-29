#pragma once

#include <string>
#include <string_view>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "libca/core/result.hpp"

namespace jbc::ir {
class ClassSession;
}

namespace mj2x {

class LcaTypeResolver;

/// @brief 旧栈 DefaultCommonSuperClassProvider 的硬编码父类链移植（批 6）。
///
/// 旧 writer 的 COMPUTE_FRAMES 公共父类口径 = 这张表 + Object 兜底（mj2x
/// 从未注入 typeResolver）。帧引擎 Strict 档经本 resolver 复现同口径：
/// 表外类型一律 Object（旧 writer 行为，语料已验证可行）。共享消费方：
/// output 输出面帧重算、native_loader/cloader 生成类的 maxes 计算。
class PortedCommonSuperResolver final : public jbc::analysis::ITypeResolver
{
public:
    ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError> merge(
        jbc::analysis::VType a, jbc::analysis::VType b) override;

    static std::string_view directSuperOf(std::string_view type);
    static bool             isAssignableTo(std::string_view target, std::string_view source);
    static std::string_view refForName(std::string_view type);
};

/// @brief 会话类输出面帧重算：对齐旧 writer COMPUTE_FRAMES——全方法重算
///        maxStack/maxLocals（lower 落位）+ StackMapTable（v50+，槽换装）。
///        帧引擎 Strict 档不可算即抛（旧 writer 同口径：不可验证的类写不出
///        来）。生成类构建器（cloader/native_loader）用同一入口拿 maxes。
///        @p lca 非空时用谱系 LCA 解析器（帧换装：输入 jar 内类型对精确求
///        公共父类），缺省维持硬编码表口径。
void recomputeSessionFrames(const std::string& name, jbc::ir::ClassSession& session,
                            LcaTypeResolver* lca = nullptr);

}   // namespace mj2x

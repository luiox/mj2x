#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"

namespace mj2x::hierarchy_supply {

/// @brief 单个类的谱系骨架（LCA 建图最小面：super 链 + 接口名）。
struct ClassSkeleton
{
    std::string              name;
    std::string              superName;   ///< 空 = 无父类（java/lang/Object 本体）
    std::vector<std::string> interfaces;
};

/// @brief 把骨架注册进层次图；已登记名跳过，畸形（成环）跳过。
/// @return 实际新增数。
ca::usize registerSkeleton(mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool,
                           const ClassSkeleton& skeleton);

/// @brief 扫描 .jar / .jmod 容器内全部 .class 的谱系骨架。
///
/// @details jmod 是 4 字节 "JM" 前缀 + zip；自动识别跳过。解析失败条目
///（module-info、隐藏类等）静默跳过——谱系图容许悬挂键。
/// @return 扫描到的类数；容器打不开返回 -1（note 附原因）。
ca::i64 scanContainerSkeletons(const std::string& path, std::vector<ClassSkeleton>& out,
                               std::string& note);

/// @brief classpath 供给：os 路径分隔符拆分；.jar 扫描，目录按 *.class 递归。
/// @return 登记增量；@p note 追加供给面描述。
ca::usize supplyClasspathSkeletons(mjbase::hierarchy::ClassHierarchy& hierarchy,
                                   mjbase::KeyPool& pool, std::string_view classpath,
                                   std::string& note);

/// @brief JDK 供给：jdk_finder 定位 JDK → jmods/*.jmod（JDK9+）或 rt.jar（JDK8）
///        扫描建图。带磁盘缓存（<temp>/mj2x_lca_cache/<release 哈希>.txt；
///        java_info 不可得时退化为每次实扫）。
/// @return 登记增量；@p note 追加供给面描述（含 JDK 路径/缓存命中态）。
ca::usize supplyJdkSkeletons(mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool,
                             std::string& note);

/// 缓存序列化往返（测试用）：文本行 `name\tsuper\titf1\titf2`。
std::string serializeSkeletons(const std::vector<ClassSkeleton>& skeletons);
std::vector<ClassSkeleton> deserializeSkeletons(std::string_view text);

}   // namespace mj2x::hierarchy_supply

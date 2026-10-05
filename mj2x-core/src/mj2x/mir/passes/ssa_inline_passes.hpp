/// @file ssa_inline_passes.hpp
/// @brief SSA 层方法内联 pass（RegC 性能主线最后一块）。
///
/// @details
/// 在 SSA build 之后、RegIR lowering 之前运行：把满足安全条件的调用点替换为
/// 被调方法的 SSA 子图（深拷贝 + 参数绑定 + 返回汇合），消除 JNI 往返与
/// 调用方-被调方边界。语义等价（fail-closed：条件不满足就跳过调用点）。
#pragma once

#include "mj2x/mir/ssa/ssa_function.hpp"

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mj2x {

/// @brief 内联选项（性能优化，默认开启；大小/深度限制防爆炸）。
struct SsaInlineOptions
{
    bool    enable               = true;
    /// 被调方法指令数上限（超过不内联）。
    ca::u32 maxCalleeInstructions = 48;
    /// 嵌套内联深度上限（A→B→C 链）。
    ca::u32 maxInlineDepth        = 2;
    /// 调用点排除谓词（返回 true = 该调用点不内联）。RegC 用它把已验证
    /// javac 合成 values() 留给调用点站点 intrinsic（getstatic + 站点缓存
    /// clone，2 次 JNI 边界；内联展开体多一次 checkcast→IsInstanceOf）；
    /// nullptr = 不排除（默认，保持既有形状）。
    std::function<bool(const SSAMethodRef&)> excludeMethod;
};

/// @brief 内联统计（测试与诊断用）。
struct SsaInlineStats
{
    ca::usize inlinedCalls          = 0;
    ca::usize skippedUnresolvable   = 0;
    ca::usize skippedByException    = 0;
    ca::usize skippedBySize         = 0;
    ca::usize skippedByRecursion    = 0;
    ca::usize skippedByDepth        = 0;
    ca::usize skippedNotInlinable   = 0;
    ca::usize skippedExcluded       = 0;
    /// 不可解析调用点样例（#1193 可见化）：按首次出现去重，超过上限截断。
    /// 形如 "owner.name:desc"（同名重载按 desc 区分）。
    std::vector<std::string> unresolvableSamples;
};

/// unresolvableSamples 单函数内保留的样例上限（去重后仍超限时只增计数不增样例）。
inline constexpr ca::usize kUnresolvableSampleCap = 8;

/// @brief 按方法引用构建被调方法 SSA 的回调。
///
/// @details
/// 由调用方（RegCBackend）提供：从 CompilerIndex 取 MethodNode，走
/// CodeConverter → SSABuilder 构建。返回 nullptr 表示目标不可解析/不可内联。
/// 返回的 SSA 仅用于克隆（pass 会深拷贝并把 Utf8StringRef 重新入调用方
/// arena），回调返回后可立即析构。
using SsaInlineResolver =
    std::function<std::unique_ptr<SSAFunction>(const SSAMethodRef& method)>;

/// @brief 对函数做方法内联（可多次迭代：克隆进来的调用点下一轮继续）。
///
/// @details
/// `arena` 用于把被调方法的 Utf8StringRef（owner/name/desc/typeDesc 等）
/// 重新入调用方 arena（被调方法的 SSA 在回调返回后析构，ref 不可浅拷贝）。
/// 安全条件（任一不满足即跳过该调用点，fail-closed）：
/// - 目标可解析（resolver 返回非空）
/// - 被调方法无异常区（exceptionRegions 为空）
/// - 指令数 ≤ maxCalleeInstructions
/// - 非递归（调用链上不含目标方法自身）
/// - 嵌套深度 ≤ maxInlineDepth
/// - 未命中 options.excludeMethod 排除谓词（可选）
/// 克隆时参数绑定调用点实参；所有返回点改跳新汇合块（block arg 收集
/// 返回值），原 Call 指令 result 的所有引用重定向到汇合块 arg。
/// 调用块位于异常受保护区时（#1188），克隆体与汇合块登记为该区域的受保护
/// 块并补异常边（handlerArgs 沿用调用块现有边——调用点之前的帧状态），
/// 发射端对新块的逐调用 pending 异常检查因此保持开启，try 语义不因内联丢失。
SsaInlineStats applySsaInlining(SSAFunction& function, const SsaInlineOptions& options,
                                const SsaInlineResolver& resolver,
                                ca::str::Utf8StringArena& arena);
}   // namespace mj2x

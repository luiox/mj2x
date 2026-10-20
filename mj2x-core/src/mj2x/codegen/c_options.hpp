/// @file c_options.hpp
/// @brief 生成开关系统：CodeOptions 命名开关表 + 注释级别 + 作用域覆盖。
///
/// @details
/// 见 doc_ai/regc_template_engine.md §3.2。发射层的"要不要生成某段代码"决策
/// 全部收敛到 CodeOptions 的强类型字段：发射点用 `opts.field` 查询，组合进
/// `CSourceStream::iff` 的条件。来源可配置（backend 配置 / CLI），作用域覆盖
/// 用 RAII 的 ScopeOverride 表达。注释级别枚举从 regc_emitter.hpp 迁入本文件
/// （codegen 层通用概念），regc_emitter.hpp 用 using 兼容既有引用。
#pragma once

#include <cstdint>

#include "mj2x/codegen/runtime_protection.hpp"

namespace mj2x::codegen {

/// @brief 生成注释的分级（与日志分级一致：默认 Summary，最详细级逐指令出注释）。
///
/// @details
/// Summary 是默认产出（函数头/块/缓存槽溯源）；Detailed 在 Summary 基础上对每条
/// RegIR 指令与终结器输出一行典型指令文本；None 用于最小输出基线。
enum class RegCCommentLevel : uint8_t
{
    None = 0,     ///< 不生成任何注释。
    Summary = 1,  ///< 函数头 + 基本块 + 缓存槽溯源注释（默认）。
    Detailed = 2, ///< Summary + 每条指令/终结器的溯源注释。
};

/// @brief native-to-native 直接 C 调用的档位。
///
/// @details
/// C 直调不产生 JVM 栈帧，`Throwable.getStackTrace` / `StackWalker` / 反射等
/// 栈观察语义会失真（issue #814）。档位按**栈失真范围**划分，而不是按"开不开"：
///
/// | 档 | 直调范围 | 栈失真范围 |
/// |---|---|---|
/// | `Off` | 仅确定性分派的虚调用（final 方法/owner、编译输入无覆写） | 无（这类调用 resolved 路径本来就直调） |
/// | `IntraClass` | + 同类内的调用 | 类内，栈上少若干同类帧 |
/// | `All` | + 所有确定性分派的已转换目标 | 跨类，栈上少整个受保护调用链 |
enum class DirectCallTier : uint8_t
{
    Off = 0,        ///< 默认。保持 JVM 栈帧语义完整。
    IntraClass = 1, ///< 同类内直调，栈失真限制在类内。
    All = 2,        ///< 全部确定性分派直调（原 `--regc-direct-calls` 布尔语义）。
};

/// @brief 判定某个调用点在给定档位下可否直调。
///
/// @details
/// RegC 与 LLVM 共用同一判定——两端口径不一致正是本次要消掉的东西。
///
/// @param tier                生效档位
/// @param sameClass           目标 owner 与当前方法 owner 相同
/// @param deterministicVirtual 确定性分派的虚调用（final 方法/owner、无覆写）
inline bool allowsDirectCall(DirectCallTier tier, bool sameClass, bool deterministicVirtual)
{
    // 确定性虚调用在任何档位都直调：resolved 路径本来就直调 fn，运行时
    // IsInstanceOf 是纯冗余，直调不引入新的语义差异。
    if (deterministicVirtual) return true;
    switch (tier) {
    case DirectCallTier::Off: return false;
    case DirectCallTier::IntraClass: return sameClass;
    case DirectCallTier::All: return true;
    }
    return false;
}

/// @brief 生成开关表：贯穿所有发射点的"是否生成"决策集中于此。
///
/// @details
/// 每个发射点用 `opts.<field>` 查询开关，作为 iff/block 条件语句的布尔源；
/// 不发明新语法。字段与 RegCEmitter 构造参数一一对应（Step 4 收敛后构造参数
/// 替换为 `const CodeOptions&`），backend 配置 / CLI 统一入口。
struct CodeOptions
{
    /// @brief 注释级别（对应原 RegCEmitter 构造的 commentLevel）。
    RegCCommentLevel commentLevel = RegCCommentLevel::Summary;

    /// @brief 实例 void 调用保留 varargs 帮手路径以输出调用日志。
    /// @details 开启时缓存调用仍走帮手（日志面）；关闭时一律内联发射。
    bool traceCallLog = false;

    /// @brief 动态实例调用启用 resolved native dispatch。
    /// @details 依赖 finalize 发射的单一 TU 函数表；多文件模式必须关闭。
    bool resolvedDispatch = false;

    /// @brief native-to-native 直接 C 调用档位（默认 Off）。
    /// @details 见 DirectCallTier。Off 之外每档都以 JVM 栈观察语义失真换 JNI
    /// 边界开销（issue #814 regc 2.6 ReTrace）。
    DirectCallTier directCallTier = DirectCallTier::Off;

    /// @brief 反侦测检查点注入（方法入口 mj2x_rt_checkpoint_run；默认关闭）。
    /// @details 依赖 mj2x-rt 的 rt_protect 模块（rt.resources 内嵌）。
    RuntimeProtectionOptions runtimeProtection;

    /// @brief RAII 作用域覆盖：构造保存旧值，析构恢复。
    /// @details 函数级/调用点级临时改开关用；支持嵌套（内层先析构先恢复）。
    ///          `CodeOptions::ScopeOverride guard(opts.traceCallLog, true);`
    template <typename T>
    class ScopeOverride
    {
    public:
        ScopeOverride(T& field, const T& value) : field_(field), saved_(field)
        {
            field_ = value;
        }

        ~ScopeOverride() { field_ = saved_; }

        ScopeOverride(const ScopeOverride&) = delete;
        ScopeOverride& operator=(const ScopeOverride&) = delete;

    private:
        T& field_;
        T  saved_;
    };
};

}   // namespace mj2x::codegen

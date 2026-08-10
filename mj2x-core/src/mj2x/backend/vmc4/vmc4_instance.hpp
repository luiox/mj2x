/// @file vmc4_instance.hpp
/// @brief VmInstance：一个 VM 实例的完整描述（seed 驱动生成）。
///
/// @details
/// VmInstance 是 VMC4 的中心抽象——它描述一个 VM 实例的全部可变维度：
///   - opcode 编号映射表（RegInstructionKind/VmOpcode → 实例 opcode 字节）
///   - handler 顺序（dispatch 表/handler 函数的排列）
///   - dispatcher 算法（5 种之一）
///   - operand 编码细节（MVP 固定变长）
///   - 驱动上述维度的 seed
///
/// MVP（Phase 0+1）的可变性维度：
///   - dispatcherKind：5 种 dispatcher 真实落地（Phase 1）
///   - opcodeTable / handlerOrder：**固定恒等映射**（Phase 2 引入随机化）
///   - seed：记录但不驱动随机化（Phase 2 起驱动）
///
/// Phase 2/3 会扩展：opcodeTable 随机化、ALT 别名、super-operator、bogus handler、
/// per-method/per-class 粒度。这些已记入 doc_ai/vmc4_multi_vm_design.md §4.5/§9。
///
/// ### 可复现构建不变量
///
/// 相同 seed → 相同 VmInstance。所有随机维度必须从 seed 确定性派生，禁止引入
/// 未播种的随机源（如系统 rand()）。MVP 阶段随机化未启用，但 seed 字段已保留，
/// 保证 Phase 2 启用时接口不变。
#pragma once

#include "mj2x/backend/vmc4/vm_opcode.hpp"

#include <cstdint>
#include <string>

namespace mj2x {

/// @brief VMC4 配置选项（由 CLI/注解传入 Vmc4Backend，再传给 VmInstance 构造）。
///
/// @details
/// MVP 只暴露 dispatcher 选择。Phase 2 会加 seed 控制和粒度（per-method/per-class）。
struct Vmc4Options
{
    /// dispatcher 形态。默认 SwitchTable（最广平台兼容）。
    VmDispatcherKind dispatcherKind = VmDispatcherKind::SwitchTable;

    /// seed（MVP 记录但不驱动随机化；Phase 2 起驱动 opcode 映射/handler 顺序）。
    /// 默认 0 = 固定实例。非 0 时仍保证可复现（相同 seed 相同实例）。
    uint64_t seed = 0;
};

/// @brief 一个 VM 实例的描述对象。
///
/// @details
/// 由 Vmc4Options 构造。解释器发射器（VmInterpreterEmitter）和编码器（VmEncoder）
/// 都消费 VmInstance 来决定生成形态。MVP 的 VmInstance 只在 dispatcherKind 上有差异，
/// 但抽象结构已为 Phase 2 的随机化维度预留字段。
class VmInstance
{
public:
    /// @brief 从配置构造 VM 实例。
    ///
    /// @details
    /// 确定性：相同 options 产生相同实例。MVP 阶段只是把 dispatcherKind/seed 存下来，
    /// Phase 2 会在此处用 seed 派生 opcodeTable/handlerOrder。
    explicit VmInstance(const Vmc4Options& options);

    /// dispatcher 形态（解释器发射据此选择生成路径）。
    VmDispatcherKind dispatcherKind() const { return dispatcherKind_; }

    /// 驱动实例的 seed（可复现构建与日志）。
    uint64_t seed() const { return seed_; }

    /// @brief 实例的简短标识（用于生成的函数名后缀，避免多实例命名冲突）。
    ///
    /// @details
    /// MVP 固定为 dispatcher 名 + seed hex，如 "switch_table_0000000000000000"。
    /// Phase 2 起 per-method 实例会附加方法哈希。
    std::string instanceTag() const;

    /// @brief MVP 是否支持该 opcode（handler 已实现）。
    ///
    /// @details
    /// Phase 0 支持 Copy/Unary/Binary/Compare/Convert + Jump/Branch/Return。
    /// 其余 opcode 的 handler 留待 Phase 2+。VmEncoder 在遇到不支持的 family 时
    /// 应先 fail-closed，但此函数供解释器发射器决定生成哪些 handler。
    static bool isOpcodeSupported(VmOpcode op);

private:
    VmDispatcherKind dispatcherKind_;
    uint64_t         seed_;
};

}   // namespace mj2x

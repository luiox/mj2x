/// @file vm_interpreter_emit.hpp
/// @brief VMC4 解释器源码发射器：把 VmProgram + VmInstance 发射成可编译的 C 解释器。
///
/// @details
/// Vmc4Backend 在 compileMethod 阶段调用 VmEncoder 得到 VmProgram，在 finalize 阶段
/// 调用本发射器把 VmProgram 转成 C 源码。生成的 C 包含：
///   - VMC4 运行时类型定义（mj2x_vm_slot 联合体、VmExecStatus、解释器上下文）
///   - 常量数组（从 VmProgram.constantPool 发射）
///   - bytecode 数组（从 VmProgram.bytecodes 发射）
///   - blockTargets 数组（从 VmProgram.blockTargets 发射）
///   - 每个 opcode 的 handler 逻辑（内联进 dispatcher，MVP 不拆独立函数以减小体积）
///   - dispatcher 主循环（按 VmInstance.dispatcherKind 选择 5 种形态之一）
///   - 解释器入口函数 mj2x_vm_exec_<tag>(...)（JNI wrapper 转调它）
///
/// ### 为什么 handler 内联进 dispatcher 而非独立函数
///
/// MVP 阶段（Phase 0+1）用"解释器模板 + 数据表"形态（见设计文档 §4.6）。
/// 把每个 handler 内联进 dispatcher 的 case/分支里，生成的是一个大的执行函数，
/// 体积可控、可读性差（这正是 VM 混淆的目标）。Phase 2 引入 FuncPtrTable
/// dispatcher 时会把 handler 拆成独立函数指针。
#pragma once

#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"

#include <string>

namespace mj2x {

/// @brief 解释器源码发射器。
class VmInterpreterEmitter
{
public:
    /// @brief 发射一个方法的解释器源码片段。
    ///
    /// @param program 编码产物（bytecodes + constantPool + blockTargets + numRegs）。
    /// @param instance VM 实例（决定 dispatcher 形态和命名后缀）。
    /// @param methodTag 方法级唯一标识（用于数组名/函数名去重，通常是 fnv1a64 hex）。
    /// @return 生成的 C 源码（解释器函数定义 + 该方法的常量/bytecode/blockTargets 数组）。
    static std::string emit(const VmProgram& program, const VmInstance& instance,
                            const std::string& methodTag);

    /// @brief 发射 VMC4 运行时公共定义（每个 TU 只需一份，放文件头）。
    ///
    /// @details
    /// 包含 mj2x_vm_slot 联合体、VmExecStatus 别名、除零/移位 helper 的 static inline
    /// 定义。VmInterpreterEmitter::emit 依赖这些类型，故 finalize 须先 emitRuntimePrelude
    /// 再 emit 各方法的解释器。
    static std::string emitRuntimePrelude();
};

}   // namespace mj2x

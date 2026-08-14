/// @file vmc4_backend.hpp
/// @brief VMC4 虚拟机后端：把选定方法虚拟化成自定义 opcode + C 解释器。
///
/// @details
/// Vmc4Backend 实现 mj2x::Backend 契约，镜像 RegCBackend 的三件套
/// （compileMethod / injectIntoClass / finalize），但在 RegIR 之后分叉：
///   - RegC：RegIR → RegCEmitter → 无栈 C helper
///   - VMC4：RegIR → VmEncoder → opcode 字节流 → VmInterpreterEmitter → C 解释器
///
/// 前端链路（L1 → SSA → RegIR → DCE → transform）与 RegC 完全共享。
///
/// ## 输出形状
///
/// 每个成功的方法生成：
///   - VM 解释器函数 mj2x_vm_exec_<tag>(slots, retval)
///   - 常量/bytecode/blockTargets 数组
///   - JNI wrapper（标 ACC_NATIVE + RegisterNatives 绑定，复用 regc 机制）
///
/// finalize 时组装：
///   - VMC4 runtime prelude（mj2x_vm_slot 联合体 + div/rem helper）
///   - 所有方法的解释器 + 数组
///   - RegisterNatives 表（每类 register fn + JNI_OnLoad）
///
/// ## 失败策略
///
/// fail-closed：SSA/RegIR/编码任一阶段失败（含 MVP 不支持的 family），
/// 保留字节码、不标 native、留可观测日志。继承 RegC 哲学（regc_codegen.md §1）。
///
/// ## 运行时边界
///
/// MVP 只支持纯计算 + 控制流（Copy/Unary/Binary/Compare/Convert/Jump/Branch/Return）。
/// 涉及对象/数组/字段/方法调用/intrinsic 的方法整体 fail-closed，保留字节码。
#pragma once

#include "mj2x/backend/backend.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/codegen/runtime_protection.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief VMC4 虚拟机后端。
class Vmc4Backend : public Backend
{
public:
    Vmc4Backend();
    ~Vmc4Backend() override;

    std::string_view name() const override { return "vmc4"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks,
                                CompilerIndex* index = nullptr) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::C99, sourceCode_, "vmc4.c"};
    }

    /// @brief 设置 VM 配置（dispatcher 形态、seed）。
    ///
    /// @details
    /// 必须在 compileMethod 之前调用：选项透传给每个方法的 VmInstance 构造。
    void setOptions(const Vmc4Options& options) { options_ = options; }

    /// @brief 设置用户自定义 intrinsic（issue #818）。
    /// @details 静态调用点匹配用户条目时降级为直接调用用户 symbol；
    /// 必须在 compileMethod 之前调用。
    void setUserIntrinsics(const UserIntrinsicConfig& config) override;

    /// @brief Set a `.c` output path written during finalize.
    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }

    /// @brief 全部实例的 expected trace surface 描述（Phase 4 红队契约）。
    ///
    /// @details
    /// 汇总 compileMethod 期间缓存的所有 VmInstance 的 describe() 文本，供 CLI
    /// 写成交付样本旁的 trace-surface 文件。同 seed + 同输入 → 同内容。
    std::string traceSurface() const;

    /// @brief 发射单个方法的 JNI wrapper（成员函数，compileMethod 调用）。
    static std::string emitNativeWrapper(const std::string& className,
                                         const std::string& methodName,
                                         const std::string& methodDesc, bool isStatic,
                                         PrimitiveKind retKind,
                                         const std::vector<PrimitiveKind>& args,
                                         const VmProgram& program,
                                         const std::string& cFuncName,
                                         const std::string& methodTag,
                                         const codegen::RuntimeProtectionOptions&
                                             runtimeProtection = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Vmc4Options options_;
    std::string cOutputPath_;
    std::string sourceCode_;
};

}   // namespace mj2x

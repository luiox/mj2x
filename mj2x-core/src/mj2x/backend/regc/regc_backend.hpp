/// @defgroup regc_native_backend RegC Native Backend
///
/// 无栈 RegC 行的 native facade。连接共享 mj2x backend 接口到较新的
/// SSA → RegIR → RegC 管线，使 RegC 可以开始替换旧的 operand-stack 形状
/// StackC 表面。
///
/// ## 管线位置
///
/// ```text
/// MethodNode → CodeConverter L1 → SSA → RegIR → RegC helper → JNI wrapper
/// ```
///
/// RegCBackend 不直接翻译字节码。它从共享 Pipeline 接收 L1 block，
/// 构建 SSA 和 RegIR，委托 RegCEmitter 生成无栈 helper 函数，
/// 并附加 JNI 导出 wrapper。
///
/// ## 输出形状
///
/// 每个成功的方法生成两层：
/// - 静态 `regc_*` helper（使用虚拟寄存器 local，如 `r0`）
/// - 静态 `__mj2x_c_<hash>` JNI 形状 wrapper（将 JNI 参数转为 helper 参数）
///
/// RegC 用 **RegisterNatives 动态绑定**（不再是 `Java_<mangled>` 符号导出）：
/// 每个方法编译成 TU 内部的 `static` C 函数（不透明名 `__mj2x_c_<fnv1a64>`），
/// 通过 finalize 阶段发射的 RegisterNatives 表显式绑定到 Java 类。每个受保护类的
/// `<clinit>` 调 `mj2x/cloader/Loader.registerNativesForClass(classId, thisClass)`
/// 触发该类惰性注册（classloader 安全）。这彻底消除了符号名/导出的脆弱性，
/// 尤其是内部类名里的 `$` 不再需要 JNI 转义即可正确绑定。
///
/// ## 失败策略
///
/// Lowering 和 emission 是 fail-closed。如果 SSA、RegIR 或 RegC 无法处理方法，
/// 返回 `BackendResult::Kind::NONE` 且不标记 Java 方法为 native。
///
/// ## 运行时边界
///
/// 纯计算和控制流可通过生成的 JNI wrapper 执行；
/// 对象/字段/数组/monitor/类型检查操作降低为 `_mj2x_regc_*` helper 调用。

/// @file regc_backend.hpp
/// @brief Native backend facade for the Register-C pipeline.
///
/// @details
/// RegCBackend adapts the stackless RegC emitter to the shared mj2x Backend
/// interface.  It lowers one method through SSA and RegIR, emits the register
/// helper function, then wraps that helper in a RegisterNatives-bound native
/// function (opaque `static ... JNICALL __mj2x_c_<hash>`) and marks the
/// original Java method as native.  The current helper ABI is emitted
/// inline into the generated translation unit so RegC samples are executable
/// while the longer-term R3 runtime symbol/context boundary is still being
/// hardened.
#pragma once

#include "mj2x/backend/backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief Backend that emits JNI-native C using the SSA -> RegIR -> RegC path.
///
/// @details
/// The backend owns no bytecode conversion logic itself.  The shared Pipeline
/// still supplies L1 MIR blocks; RegCBackend builds SSA and RegIR from those
/// blocks, delegates stackless C generation to RegCEmitter, and appends a JNI
/// wrapper with javah-compatible symbol naming.  Methods that cannot be lowered
/// fail closed and are not injected as native methods; this prevents partially
/// translated bytecode from being replaced by a broken native stub.
class RegCBackend : public Backend
{
public:
    RegCBackend();
    ~RegCBackend() override;

    std::string_view name() const override { return "regc_native"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    /// @brief Set a `.c` output path written during finalize.
    ///
    /// @details
    /// Pipeline and E2E tests use this to compile the generated translation unit
    /// into a native library.  When empty, finalize still records
    /// `generatedSource()` but does not write a file.
    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }

    /// @brief 设置生成注释的详细级别（见 reg_codegen_syntax.md §4.2）。
    ///
    /// @details
    /// 必须在 compileMethod 之前调用：级别透传给 RegCCachePool 与 RegCEmitter，
    /// 影响函数头/块/cache 溯源注释（Summary）与逐指令 IR 注释（Detailed）。
    /// 默认 Summary。
    void setCommentLevel(RegCCommentLevel level);

    /// @brief Full generated C translation unit after finalize.
    ///
    /// @details
    /// The source includes helper declarations/definitions, one RegC helper per
    /// successfully lowered method, and the JNI wrappers used to bind those
    /// helpers back to the protected Java class.
    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::C99, sourceCode_, "native.c"};
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string cOutputPath_;
    std::string sourceCode_;
};

}   // namespace mj2x

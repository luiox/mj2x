/// @file backend.hpp
/// @brief 抽象后端接口：L1 MIR -> native 翻译的统一契约。
///
/// @details
/// Pipeline 通过这个接口驱动每个后端（StackC / RegC / 未来新后端）。后端只负责
/// 把 L1 MIR blocks 翻译成 native 源、把方法标记为 native、并在 finalize 时把
/// 累积的源写出来。class 重写（如 clinit 注入）优先由 postprocessor 负责，
/// 不再由后端承担（见 NativeLoaderPostprocessor）。
#pragma once

#include "backend_result.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/user_intrinsics.hpp"
#include "mj2x/frontend/block.hpp"
#include "libca/log/log_macros.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mj2x {

/// @brief finalize 期诊断聚合（T1：三后端失败/降级行为口径统一）。
///
/// @details
/// finalize 期的事实多是「降级而非失败」：RegC 内联因 callee 不可解析放弃、
/// VMC4 配置冲突被归一化——此前只落各自 tag 的 WARN 日志，管线/CLI 无编程
/// 消费面，口径散在三个后端。finalize 把告警同时收进本结构（日志照发），
/// warnings > 0 即本次产物存在降级；samples 携带逐条一句话描述（封顶由各
/// 后端自定）。
struct BackendDiagnostics
{
    std::size_t              warnings = 0;
    std::vector<std::string> samples;
};


/// @brief Native 源文件的语言方言。
///
/// 当前两个后端都只生成 C99；C++17 和 LLVM IR 由后续 emitter 通过同一契约接入。
enum class NativeSourceLanguage
{
    C99,
    Cpp17,
    LlvmIr,
};

/// @brief finalize 后由 native backend 公开的源产物。
///
/// @details content 的所有权仍属于 backend，调用方不得在 backend 析构后保留该视图。
struct NativeSourceArtifact
{
    NativeSourceLanguage language;
    std::string_view     content;
    std::string_view     suggestedFileName;
};

/// @brief 抽象后端：把选定方法的 L1 MIR 翻译成 native 源。
///
/// @details
/// 四个核心方法的调用时机（见 Pipeline::run）：
///  - beforeCompile：全部方法编译开始前调一次，此时全类目录已加载、
///    preprocessor 已跑完、任何 MethodNode 指令尚未被转换清空。
///  - compileMethod：每个 markedForCompile 方法调一次，产出 BackendResult。
///  - injectIntoClass：compileMethod 之后对同一方法调一次，改写 class（标 ACC_NATIVE、
///    清指令）。失败 lowering（result.kind 非成功产物形态）时不应改写。
///  - finalize：所有方法编译完后调一次，输出累积的 native 源。
class Backend
{
public:
    virtual ~Backend()                    = default;

    /// 后端名（用于日志，如 "c_source" / "regc_native"）。
    virtual std::string_view name() const = 0;

    /// @brief 全部方法编译开始前的整输入钩子（issue #1193）。
    ///
    /// @details
    /// 唯一能看见全部方法体原始指令的时机：此后 compileMethod/injectIntoClass
    /// 逐方法清空指令，跨方法体访问（如内联被调解析）即受转换顺序制约。
    /// 需要条目序无关视角的后端在此做整 jar 预解析。默认无操作。
    virtual void beforeCompile(CompilerIndex& /*index*/) {}

    /// @brief 翻译一个方法的 L1 blocks 为 native 源。
    /// @return 成功时 result.kind 为成功产物形态（C 系后端 C_SOURCE / LLVM 后端
///         LLVM_IR）且 text 含生成源；失败时 fail-closed（kind=NONE）。
    /// @param index 可选：整个编译输入的类索引（调用目标元数据/已转换集合）。
    ///              纯后端的既有测试不传时为 nullptr，行为与以前一致。
    virtual BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                        const std::vector<Block*>& blocks,
                                        CompilerIndex* index = nullptr) = 0;

    /// @brief 把编译结果写回 class（标记 native、清字节码指令）。
    /// @details result.kind 非本后端成功产物形态时应直接返回，保留原字节码。
    virtual void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                 const BackendResult& result) = 0;

    /// 所有方法编译完后调用，输出累积的 native 源到文件或 generatedSource()。
    virtual void finalize(const CompilerIndex& index) = 0;

    /// @brief finalize 期诊断（降级告警计数 + 采样）。
    /// @details finalize 入口清零、期间经 recordWarning 累积；管线在全部后端
    ///          finalize 后聚合输出，编程消费面（corpus 门禁等）经此读取。
    const BackendDiagnostics& diagnostics() const { return diagnostics_; }

    /// @brief 返回 finalize 后生成的完整 native 源码。
    /// @details 返回的引用由 backend 持有，在 backend 生命周期内有效。
    virtual const std::string& generatedSource() const = 0;

    /// @brief 返回 finalize 后生成的 native 源产物元数据与内容。
    virtual NativeSourceArtifact generatedSourceArtifact() const = 0;

    /// @brief 设置用户自定义 intrinsic（issue #818）。
    /// @details 默认无操作；StackC / RegC / VMC4 override 把用户条目接入各自
    /// 的调用发射路径（RegC registry 合并、StackC emitInvoke 匹配、VMC4
    /// encodeCall 自定义 helper）。在 compileMethod 前调用。
    virtual void setUserIntrinsics(const UserIntrinsicConfig& /*config*/) {}

protected:
    /// @brief 记录一条 finalize 期降级告警：日志照发 + 进诊断聚合。
    void recordWarning(const char* tag, const std::string& message)
    {
        ++diagnostics_.warnings;
        diagnostics_.samples.push_back(message);
        CA_LOGT_WARN(tag, "{}\n", message.c_str());
    }

    /// @brief finalize 入口调用：清零上一轮诊断（后端可跨 run 复用）。
    void resetDiagnostics()
    {
        diagnostics_ = BackendDiagnostics{};
    }

    /// @brief 收编已另行落日志的降级事实（不重复打日志）：1 条告警 + 采样入聚合。
    void adoptDiagnostics(std::vector<std::string> samples)
    {
        if (samples.empty()) return;
        ++diagnostics_.warnings;
        diagnostics_.samples = std::move(samples);
    }

private:
    BackendDiagnostics diagnostics_;
};

}   // namespace mj2x

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
#include "mj2x/frontend/block.hpp"

#include <string>
#include <string_view>

namespace mj2x {

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
/// 三个核心方法的调用时机（见 Pipeline::run）：
///  - compileMethod：每个 markedForCompile 方法调一次，产出 BackendResult。
///  - injectIntoClass：compileMethod 之后对同一方法调一次，改写 class（标 ACC_NATIVE、
///    清指令）。失败 lowering（result.kind 非 C_SOURCE）时不应改写。
///  - finalize：所有方法编译完后调一次，输出累积的 native 源。
class Backend
{
public:
    virtual ~Backend()                    = default;

    /// 后端名（用于日志，如 "c_source" / "regc_native"）。
    virtual std::string_view name() const = 0;

    /// @brief 翻译一个方法的 L1 blocks 为 native 源。
    /// @return 成功时 result.kind=C_SOURCE 且 text 含生成源；失败时 fail-closed。
    virtual BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                        const std::vector<Block*>& blocks) = 0;

    /// @brief 把编译结果写回 class（标记 native、清字节码指令）。
    /// @details result.kind 非 C_SOURCE 时应直接返回，保留原字节码。
    virtual void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                 const BackendResult& result) = 0;

    /// 所有方法编译完后调用，输出累积的 native 源到文件或 generatedSource()。
    virtual void finalize(const CompilerIndex& index) = 0;

    /// @brief 返回 finalize 后生成的完整 native 源码。
    /// @details 返回的引用由 backend 持有，在 backend 生命周期内有效。
    virtual const std::string& generatedSource() const = 0;

    /// @brief 返回 finalize 后生成的 native 源产物元数据与内容。
    virtual NativeSourceArtifact generatedSourceArtifact() const = 0;
};

}   // namespace mj2x

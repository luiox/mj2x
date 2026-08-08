/// @file regc_emitter.hpp
/// @brief Stackless C emitter for Register IR.
///
/// @details
/// RegC is the stackless native backend line.  The emitter consumes RegIR and
/// writes one C helper function that uses virtual-register locals, labels,
/// edge-copy temporaries, and `_mj2x_regc_*` helper calls for JVM operations.
/// It does not own JNI wrapper emission or class injection; those are handled by
/// RegCBackend.  This split keeps the core trace surface focused on register
/// code while the backend facade supplies JVM binding glue.
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include "libca/str/utf8_string.hpp"
#include "mj2x/codegen/c_source.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mj2x {

/// @brief 生成注释的分级（与日志分级一致：默认 Summary，最详细级逐指令出注释）。
///
/// @details
/// 见 doc_ai/reg_codegen_syntax.md §4.2。Summary 是默认产出（函数头/块/缓存槽溯源）；
/// Detailed 在 Summary 基础上对每条 RegIR 指令与终结器输出一行典型指令文本（复用
/// reg_dump 的行渲染），供逐字节排查生成代码与 IR 的一致性。None 用于最小输出基线。
enum class RegCCommentLevel : uint8_t
{
    None = 0,     ///< 不生成任何注释。
    Summary = 1,  ///< 函数头 + 基本块 + 缓存槽溯源注释（默认）。
    Detailed = 2, ///< Summary + 每条指令/终结器的溯源注释。
};

/// @brief Result of one RegC emission attempt.
///
/// @details
/// `ok` is false when the RegIR shape is malformed or an operation is outside
/// the current helper surface.  `functionName` is populated early so callers can
/// still report deterministic diagnostics, while `source` is valid only when
/// `ok` is true.
struct RegCEmitResult
{
    bool        ok = false;
    std::string functionName;
    std::string source;
    std::string reason;
};

/// @brief Shared generated-C translation-unit cache declaration pool.
///
/// @details
/// RegCEmitter can still emit function-local cache declarations for standalone
/// tests.  RegCBackend supplies one pool for the whole generated C file so equal
/// owner/name/desc or class descriptors across methods share a single static
/// cache object.
class RegCCachePool
{
public:
    explicit RegCCachePool(RegCCommentLevel commentLevel = RegCCommentLevel::Summary);

    ca::str::Utf8String staticMethodCache(const ca::str::Utf8StringRef& owner,
                                          const ca::str::Utf8StringRef& name,
                                          const ca::str::Utf8StringRef& desc);
    ca::str::Utf8String instanceMethodCache(const ca::str::Utf8StringRef& owner,
                                            const ca::str::Utf8StringRef& name,
                                            const ca::str::Utf8StringRef& desc);
    ca::str::Utf8String staticFieldCache(const ca::str::Utf8StringRef& owner,
                                         const ca::str::Utf8StringRef& name,
                                         const ca::str::Utf8StringRef& desc);
    ca::str::Utf8String instanceFieldCache(const ca::str::Utf8StringRef& owner,
                                           const ca::str::Utf8StringRef& name,
                                           const ca::str::Utf8StringRef& desc);
    ca::str::Utf8String classCache(const ca::str::Utf8StringRef& desc);

    std::string renderDeclarations() const;
    bool        empty() const;

    /// @brief 在首次 add 前覆盖注释级别（影响后续声明的 cache 溯源注释）。
    /// @details RegCBackend 透传统一级别时调用；与 RegCEmitter 的级别保持一致，
    ///          保证翻译单元内 cache 声明注释与函数体注释同档。
    void setCommentLevel(RegCCommentLevel level) { commentLevel_ = level; }

private:
    std::vector<ca::str::Utf8String>     declarations_;
    std::map<std::string, ca::str::Utf8String> staticMethodCacheNames_;
    std::map<std::string, ca::str::Utf8String> instanceMethodCacheNames_;
    std::map<std::string, ca::str::Utf8String> staticFieldCacheNames_;
    std::map<std::string, ca::str::Utf8String> instanceFieldCacheNames_;
    std::map<std::string, ca::str::Utf8String> classCacheNames_;
    int                                staticMethodCacheIndex_   = 0;
    int                                instanceMethodCacheIndex_ = 0;
    int                                staticFieldCacheIndex_    = 0;
    int                                instanceFieldCacheIndex_  = 0;
    int                                classCacheIndex_          = 0;
    RegCCommentLevel                   commentLevel_              = RegCCommentLevel::Summary;
};

/// @brief Emits stackless helper C from Register IR.
///
/// @details
/// The emitter supports arithmetic, conversions, branches, switches, returns,
/// phi edge copies, helper-backed heap/reference/call operations, `ATHROW`, and
/// exception-table dispatch metadata carried by RegIR.  It fails closed with a
/// reason string if an unsupported intrinsic or invalid RegIR invariant reaches
/// this layer.
class RegCEmitter
{
public:
    explicit RegCEmitter(RegCCachePool* cachePool = nullptr,
                         RegCCommentLevel commentLevel = RegCCommentLevel::Summary);

    /// @brief Emit a C helper function for one RegFunction.
    ///
    /// @details
    /// The generated helper is intentionally not a JNI export.  It receives
    /// RegIR parameters as C parameters and calls `_mj2x_regc_*` helpers for JVM
    /// operations that require runtime or JNI state.  The returned source must
    /// pass `validateRegCNoStackAnchors` before `ok` is set.
    RegCEmitResult emitFunction(const RegFunction& fn) const;

private:
    RegCCachePool*   cachePool_;
    RegCCommentLevel commentLevel_ = RegCCommentLevel::Summary;
};

/// @brief Emit declarations shared by all RegC function bodies in one C file.
void emitRegCHelperPrelude(ca::str::Utf8StringBuilder& out);

/// @brief Validate that generated RegC text does not expose stack-machine anchors.
///
/// @details
/// This is a structural string gate for the generated C surface.  RegIR itself
/// has no stack-slot operand kind; this helper ensures the C emitter does not
/// accidentally reintroduce names that c2j-style tooling can match.
bool validateRegCNoStackAnchors(const std::string& source, std::string& reason);

}   // namespace mj2x

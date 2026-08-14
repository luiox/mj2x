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
#include "mj2x/codegen/c_options.hpp"
#include "mj2x/codegen/c_source.hpp"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

class CompilerIndex;
class RegCIntrinsicRegistry;

/// @brief 生成注释级别（定义见 codegen/c_options.hpp，本 using 保持既有引用兼容）。
using RegCCommentLevel = codegen::RegCCommentLevel;

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
    ca::str::Utf8String resolvedCallSite(const ca::str::Utf8StringRef& owner,
                                         const ca::str::Utf8StringRef& name,
                                         const ca::str::Utf8StringRef& desc);

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
    std::map<std::string, ca::str::Utf8String> resolvedCallSiteNames_;
    int                                staticMethodCacheIndex_   = 0;
    int                                instanceMethodCacheIndex_ = 0;
    int                                staticFieldCacheIndex_    = 0;
    int                                instanceFieldCacheIndex_  = 0;
    int                                classCacheIndex_          = 0;
    int                                resolvedCallSiteIndex_    = 0;
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
    /// @param cachePool 文件级缓存槽池（nullptr 时方法级 cache 声明）。
    /// @param options   生成开关表（注释级别/跟踪/分派形态等，见 codegen::CodeOptions）。
    /// @param fileIndySites 可选：非空时 emitFunction 把本方法的 invokedynamic 站点
    ///                      追加进文件级表（finalize 据此生成 bootstrap 函数）。
    /// @param indySiteBase  文件级表的全局站点偏移（本方法首个站点的全局索引）。
    /// @param classIndex    可选：调用目标元数据（final 判定）；nullptr 关闭直接调用。
    /// @param convertedFunctions 可选：已转换方法表（owner\nname\ndesc -> regc_* 函数名）。
    /// @param indyFunctions      可选：含 invokedynamic 站点的方法键集合（直接调用排除）。
    /// @param intrinsicRegistry  可选：用户自定义 intrinsic registry（issue #818）；
    ///                           nullptr 时用 builtins()。
    explicit RegCEmitter(RegCCachePool* cachePool = nullptr,
                         const codegen::CodeOptions& options = {},
                         std::vector<RegFunction::InvokeDynamicSite>* fileIndySites = nullptr,
                         size_t indySiteBase = 0,
                         CompilerIndex* classIndex = nullptr,
                         const std::unordered_map<std::string, std::string>* convertedFunctions =
                             nullptr,
                         const std::unordered_set<std::string>* indyFunctions = nullptr,
                         const RegCIntrinsicRegistry* intrinsicRegistry = nullptr);

    /// @brief Emit a C helper function for one RegFunction.
    ///
    /// @details
    /// The generated helper is intentionally not a JNI export.  It receives
    /// RegIR parameters as C parameters and calls `_mj2x_regc_*` helpers for JVM
    /// operations that require runtime or JNI state.  The returned source must
    /// pass `validateRegCNoStackAnchors` before `ok` is set.
    RegCEmitResult emitFunction(const RegFunction& fn);

    /// @brief 计算 RegFunction 生成的 C 函数体名（与 emitFunction 产出一致）。
    ///
    /// @details
    /// RegCBackend 用它提前登记 convertedFunctions（供实例自递归直接调用），
    /// 保证 emit 过程中同一 key 已指向最终函数名。
    static std::string functionNameFor(const SSAFunctionSymbol& symbol);

private:
    RegCCachePool*   cachePool_;
    codegen::CodeOptions options_;
    std::vector<RegFunction::InvokeDynamicSite>* fileIndySites_ = nullptr;
    size_t indySiteBase_ = 0;
    CompilerIndex* classIndex_ = nullptr;
    const std::unordered_map<std::string, std::string>* convertedFunctions_ = nullptr;
    const std::unordered_set<std::string>* indyFunctions_ = nullptr;
    const RegCIntrinsicRegistry* intrinsicRegistry_ = nullptr;
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

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

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace mj2x {

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
    std::string staticMethodCache(const std::string& owner, const std::string& name,
                                  const std::string& desc);
    std::string instanceMethodCache(const std::string& owner, const std::string& name,
                                    const std::string& desc);
    std::string staticFieldCache(const std::string& owner, const std::string& name,
                                 const std::string& desc);
    std::string instanceFieldCache(const std::string& owner, const std::string& name,
                                   const std::string& desc);
    std::string classCache(const std::string& desc);

    std::string renderDeclarations() const;
    bool        empty() const;

private:
    std::vector<std::string>           declarations_;
    std::map<std::string, std::string> staticMethodCacheNames_;
    std::map<std::string, std::string> instanceMethodCacheNames_;
    std::map<std::string, std::string> staticFieldCacheNames_;
    std::map<std::string, std::string> instanceFieldCacheNames_;
    std::map<std::string, std::string> classCacheNames_;
    int                                staticMethodCacheIndex_   = 0;
    int                                instanceMethodCacheIndex_ = 0;
    int                                staticFieldCacheIndex_    = 0;
    int                                instanceFieldCacheIndex_  = 0;
    int                                classCacheIndex_          = 0;
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
    explicit RegCEmitter(RegCCachePool* cachePool = nullptr);

    /// @brief Emit a C helper function for one RegFunction.
    ///
    /// @details
    /// The generated helper is intentionally not a JNI export.  It receives
    /// RegIR parameters as C parameters and calls `_mj2x_regc_*` helpers for JVM
    /// operations that require runtime or JNI state.  The returned source must
    /// pass `validateRegCNoStackAnchors` before `ok` is set.
    RegCEmitResult emitFunction(const RegFunction& fn) const;

private:
    RegCCachePool* cachePool_;
};

/// @brief Emit declarations shared by all RegC function bodies in one C file.
void emitRegCHelperPrelude(std::ostream& out);

/// @brief Validate that generated RegC text does not expose stack-machine anchors.
///
/// @details
/// This is a structural string gate for the generated C surface.  RegIR itself
/// has no stack-slot operand kind; this helper ensures the C emitter does not
/// accidentally reintroduce names that c2j-style tooling can match.
bool validateRegCNoStackAnchors(const std::string& source, std::string& reason);

}   // namespace mj2x

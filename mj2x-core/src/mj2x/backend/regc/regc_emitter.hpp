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

#include <cstdint>
#include <map>
#include <set>
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
    /// javac 合成 values()/valueOf 调用点站点缓存（owner 去重；发射端已验证
    /// 方法体为 javac 合成形态）。
    ca::str::Utf8String enumValuesSite(const ca::str::Utf8StringRef& owner);
    ca::str::Utf8String enumValueOfSite(const ca::str::Utf8StringRef& owner);
    /// getClass().getName().hashCode() 融合调用点站点缓存（每调用点一个）。
    ca::str::Utf8String classNameHashSite();

    /// @brief 开启字符串字面量 at-rest 加密（--strings-encrypt，对标 jvmtp
    ///        obfuscation.strings / JavaShroud 字符串加密）。
    /// @details 开启后 operandExpr 的 ldc_string 调用点不再内联明文
    ///          cStringLiteral，改为引用密文缓冲变量（内容 + 结尾 NUL 一起
    ///          经 xorshift32 密钥流逐字节加密）；缓冲定义经
    ///          renderEncryptedLiterals() 统一渲染，解密调用经
    ///          renderEncryptedLiteralDecryptCalls() 插入 JNI_OnLoad 批量
    ///          执行（JVM 保证单线程且先于任何 native 方法，规避 XOR 双解密
    ///          污染与就绪位图竞态）。首切片边界：仅覆盖 LDC 字面量面与
    ///          indy bsm 参数字符串；JavaShroud 的 callsite 形状多样化（每
    ///          解码点不同 push 形状/indy/payload 载体）是后续切片。
    /// @param seedBase 密钥基 seed：非 0 时混入每字面量密钥（RegC 链路取
    ///                 RegIR 变换 transformSeed——CLI 侧已做 clientId 派生）；
    ///                 0 时仅由字面量内容派生（确定性、可复现构建）。
    void setStringsEncrypt(bool enabled, std::uint64_t seedBase = 0);

    /// @brief 字面量加密是否已启用。
    bool stringsEncryptEnabled() const { return stringsEncrypt_; }

    /// @brief 登记一个字面量密文缓冲并返回缓冲变量名（内容去重，多调用点共享）。
    /// @details 仅在 stringsEncryptEnabled() 时由发射路径调用；密文按
    ///          xorshift32 密钥流逐字节生成（与 mj2x_regc_runtime 的
    ///          _mj2x_regc_decrypt_bytes 逐位互逆）。缓冲生命周期覆盖整个
    ///          执行期（JNI_OnLoad 解密一次后常驻）。
    std::string encryptedLiteralBuffer(const std::string& content);

    /// @brief 渲染 TU 头部密文缓冲定义块（finalize 期调用一次；空返回 ""）。
    std::string renderEncryptedLiterals() const;

    /// @brief 渲染 JNI_OnLoad 内的逐缓冲批量解密调用行（finalize 期调用一次）。
    std::string renderEncryptedLiteralDecryptCalls() const;

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
    std::map<std::string, ca::str::Utf8String> enumValuesSiteNames_;
    std::map<std::string, ca::str::Utf8String> enumValueOfSiteNames_;
    int                                classNameHashSiteIndex_ = 0;
    int                                staticMethodCacheIndex_   = 0;
    int                                instanceMethodCacheIndex_ = 0;
    int                                staticFieldCacheIndex_    = 0;
    int                                instanceFieldCacheIndex_  = 0;
    int                                classCacheIndex_          = 0;
    int                                resolvedCallSiteIndex_    = 0;
    int                                enumValuesSiteIndex_      = 0;
    int                                enumValueOfSiteIndex_     = 0;
    RegCCommentLevel                   commentLevel_              = RegCCommentLevel::Summary;

    /// 字面量加密状态（--strings-encrypt）：登记序即渲染序；密文缓冲为
    /// name + 密文字节（含结尾 NUL）+ 每字面量 seed（解密调用行内联发射）。
    struct EncryptedLiteral
    {
        std::string               name;    ///< 缓冲变量名（_mj2x_str_<hex16>）
        std::vector<std::uint8_t> cipher;  ///< 密文字节（含结尾 NUL）
        std::uint32_t             seed = 0; ///< 该字面量的 xorshift32 种子
    };
    bool                               stringsEncrypt_         = false;
    std::uint64_t                      stringsEncryptSeedBase_ = 0;
    std::map<std::string, std::string> encryptedLiteralNames_;     ///< content -> 缓冲名
    std::vector<EncryptedLiteral>      encryptedLiterals_;
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
    /// @param enumSyntheticMethods 可选：后端预扫描验证过的 javac 合成枚举
    ///                             values()/valueOf() 方法键集合；nullptr 时
    ///                             发射器对这类调用点回退现场验证（单测路径）。
    explicit RegCEmitter(RegCCachePool* cachePool = nullptr,
                         const codegen::CodeOptions& options = {},
                         std::vector<RegFunction::InvokeDynamicSite>* fileIndySites = nullptr,
                         size_t indySiteBase = 0,
                         CompilerIndex* classIndex = nullptr,
                         const std::unordered_map<std::string, std::string>* convertedFunctions =
                             nullptr,
                         const std::unordered_set<std::string>* indyFunctions = nullptr,
                         const RegCIntrinsicRegistry* intrinsicRegistry = nullptr,
                         const std::set<std::string>* enumSyntheticMethods = nullptr);

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
    const std::set<std::string>* enumSyntheticMethods_ = nullptr;
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

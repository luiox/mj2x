/// @file regc_emitter_internal.hpp
/// @brief RegC 发射器内部共享声明：功能区拆分后的跨 TU 接口。
///
/// @details
/// 仅供 backend/regc/ 下 regc_*.cpp 内部包含（不对外）。承载发射状态结构
/// （RegCNativeObjectState / RegCFunctionEmitState / RegCStringBuilderPlan）
/// 与按功能区拆出的 helper 声明。声明随拆分批次增量加入：某函数搬入新文件
/// 的 commit 才把它的声明加进本头，避免与 regc_emitter.cpp 内仍保留的定义
/// 在同 TU 冲突。拆分保持纯搬移：生成代码零差异。
#pragma once

#include "regc_emitter.hpp"

#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/core/compiler_index.hpp"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

// 与既有 cpp 一致：CSourceStream 从 codegen 起别名，函数签名保持一行。
using codegen::CSourceStream;

/// @brief RegId 对应的 native 对象状态（装箱值/Random/StringBuilder 等 intrinsic 载体）。
struct RegCNativeObjectState
{
    RegCIntrinsicNativeObjectKind kind = RegCIntrinsicNativeObjectKind::None;
    std::string                   storageName;
};

/// @brief 单个 RegFunction 发射过程的状态：native 对象表、缓存名表、临时索引。
///
/// @details
/// 全部引用类型均为持有对象（map 存值），发射 helper 间按引用共享；函数内
/// 发射顺序依赖索引递增（stringBuilderTempIndex/edgeTempIndex 等），拆分后
/// 语义保持与拆分前一致。
struct RegCFunctionEmitState
{
    std::map<uint32_t, RegCNativeObjectState> nativeObjects;
    std::string                               functionName;
    SSAFunctionSymbol                         symbol;
    RegCCachePool*                            cachePool = nullptr;
    RegCCommentLevel                          commentLevel = RegCCommentLevel::Summary;
    bool                                      traceCalls   = false;
    /// native-to-native 直接调用开关（CodeOptions.directCalls；默认关闭保持
    /// JVM 栈帧语义，见 c_options.hpp）。
    bool                                      directCalls  = false;
    /// native-to-native 直接调用上下文（compileMethod 传入，nullptr 关闭）。
    CompilerIndex*                            classIndex          = nullptr;
    const std::unordered_map<std::string, std::string>* convertedFunctions = nullptr;
    const std::unordered_set<std::string>*    indyFunctions       = nullptr;
    /// intrinsic 匹配 registry（issue #818）：nullptr 时用 builtins()。
    /// RegCBackend::setUserIntrinsics 构造带用户条目的 registry 实例后设置。
    const RegCIntrinsicRegistry*              intrinsicRegistry   = nullptr;
    /// 动态实例调用是否启用 resolved native dispatch（多文件模式关闭：
    /// 函数表只在单文件 TU 内可见）。
    bool                                      resolvedDispatch    = false;
    /// 本方法是否发射 PushLocalFrame 帧（IR 级判定，见 regc_emitter.cpp
    /// fnProducesLocalRefsIR）。resolved 调用点据此省略每次 GetObjectClass 后
    /// 的 DeleteLocalRef——帧内 local ref 由 PopLocalFrame 批量释放。
    bool                                      hasLocalFrame       = false;
    std::vector<ca::str::Utf8String>         staticMethodCacheDeclarations;
    std::vector<ca::str::Utf8String>       staticFieldCacheDeclarations;
    std::vector<ca::str::Utf8String>       classCacheDeclarations;
    std::map<std::string, ca::str::Utf8String> staticMethodCacheNames;
    std::map<std::string, ca::str::Utf8String> staticFieldCacheNames;
    std::map<std::string, ca::str::Utf8String> classCacheNames;
    int                                       staticMethodCacheIndex = 0;
    int                                       staticFieldCacheIndex  = 0;
    int                                       classCacheIndex        = 0;
    int                                       stringBuilderTempIndex = 0;
    std::set<uint32_t>                        nativeStringBuilderRegisters;
    std::vector<uint32_t>                     nativeStringBuilderOwners;
    std::map<uint32_t, uint32_t>              nativeStringBuilderOwnerByRegister;
    std::set<uint32_t>                        nativeWrapperRegisters;
    /// Edge-copy temporary types actually emitted, indexed by edgeTempIndex.
    /// Captured during emit so the declaration block declares exactly the temporaries
    /// that are used (a block with multiple mayThrow instructions re-emits its
    /// exception edge copies once per throw site, so the count can exceed the static
    /// CFG edge count).
    std::vector<SSAType>                      edgeTempTypes;
};

/// @brief analyzeStringBuilders 输出：native StringBuilder 寄存器集合与归属关系。
struct RegCStringBuilderPlan
{
    std::set<uint32_t>                                nativeRegisters;
    std::vector<uint32_t>                             ownerRegisters;
    std::map<uint32_t, uint32_t>                      ownerByRegister;
    std::map<uint32_t, RegCIntrinsicNativeObjectKind> kindByRegister;
};

/// @name regc_expr.cpp：类型/表达式/命名 helper
/// @{

const char* cType(SSAType type);
const char* unsignedCType(SSAType type);
bool        isIntegerLike(SSAType type);
std::string sanitizeIdent(const std::string& text);
std::string functionName(const SSAFunctionSymbol& symbol);
std::string regName(RegId id);
const char* defaultValue(SSAType type);
const char* helperSuffix(SSAType type);

/// 解析方法描述符 "(...)" 中的参数类型标签（Z B C S I J F D / L / [）。
std::vector<char> descriptorParamTags(const ca::str::Utf8StringRef& desc);
/// 描述符返回类型标签（')' 之后的首字符），无效描述符返回 '\0'。
char descriptorReturnTag(const ca::str::Utf8StringRef& desc);
/// JNI CallXxxMethodA 的类型族名，如 'I' -> "Int"。
const char* jniCallFamily(char tag);
/// jvalue 联合体成员名，如 'I' -> "i"。
const char* jvalueSlotName(char tag);
/// 值写入 jvalue 槽前的显式强转，如 'I' -> "(jint)"。
const char* jniValueCast(char tag);
/// JNI 调用结果到寄存器 C 类型的强转；引用结果转 mj2x_ref。
ca::str::Utf8String jniResultCast(char returnTag, SSAType destType);

ca::str::Utf8String floatingOperandExpr(double value, SSAType type);
ca::str::Utf8String operandExpr(const RegOperand& operand);
const char*         binaryOp(SSABinaryOp op);
const char*         compareOp(SSAComparePredicate predicate);

const char* fieldHelperSuffix(const ca::str::Utf8StringRef& desc, SSAType fallback);
const char* arrayHelperSuffix(SSAType type);
bool        isReferenceArrayDesc(const ca::str::Utf8StringRef& desc);
std::string arrayComponentDesc(const ca::str::Utf8StringRef& desc);
std::vector<std::string> multiArrayComponentDescs(const ca::str::Utf8StringRef& desc,
                                                  size_t                      dimCount);

const char* nativeObjectType(RegCIntrinsicNativeObjectKind kind);
bool        isMutableStringKind(RegCIntrinsicNativeObjectKind kind);
const char* mutableStringDisplayName(RegCIntrinsicNativeObjectKind kind);
ca::str::Utf8String stringBuilderOwnerName(RegId id);
ca::str::Utf8String stringBuilderMaterializedName(RegId id);
ca::str::Utf8String stringBuilderHandleExpr(const RegOperand&             operand,
                                            RegCIntrinsicNativeObjectKind kind);
ca::str::Utf8String stringBuilderStateExpr(const RegOperand&             operand,
                                           RegCIntrinsicNativeObjectKind kind);

/// 带参数个数的 varargs 帮手实参列表（"N, (T)(arg), ..."）。
ca::str::Utf8String callArgs(const std::vector<RegOperand>& args);
ca::str::Utf8String callArgs(const std::vector<RegOperand>&           args,
                             const std::vector<ca::str::Utf8String>& expressions);
/// 直接调用实参列表（"(T)(arg), ..."，无参数个数前缀）。
ca::str::Utf8String directCallArgs(const std::vector<RegOperand>& args);
ca::str::Utf8String directCallArgs(const std::vector<RegOperand>&           args,
                                   const std::vector<ca::str::Utf8String>& expressions);
/// @}

/// @name regc_calls.cpp：方法调用发射与缓存添加
/// @{

/// 查寄存器对应的 native 包装对象（StringBuilder/StringBuffer），非寄存器或未登记返回
/// nullptr。
const RegCNativeObjectState* nativeObjectFor(const RegCFunctionEmitState& state,
                                             const RegOperand&            operand);
bool isStaticCall(const RegInstruction& ins);
bool isSelfStaticCall(const RegCFunctionEmitState& state, const RegInstruction& ins);
std::string cacheKey(const ca::str::Utf8StringRef& first, const ca::str::Utf8StringRef& second,
                     const ca::str::Utf8StringRef& third);
ca::str::Utf8String addStaticMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins);
ca::str::Utf8String addInstanceMethodCache(RegCFunctionEmitState& state,
                                           const RegInstruction&  ins);
ca::str::Utf8String addStaticFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins);
ca::str::Utf8String addInstanceFieldCache(RegCFunctionEmitState& state,
                                          const RegInstruction&  ins);
ca::str::Utf8String addClassCache(RegCFunctionEmitState&      state,
                                  const ca::str::Utf8StringRef& desc);
bool isDeterministicDispatch(const RegInstruction& ins, CompilerIndex* classIndex);

/// 发射 native-to-native 直接调用（跳过 JNI 方法分派），未命中时返回 false。
bool tryEmitDirectCall(CSourceStream&                                     out,
                       const RegInstruction&                              ins,
                       RegCFunctionEmitState&                             state,
                       CompilerIndex*                                      classIndex,
                       const std::unordered_map<std::string, std::string>* convertedFunctions,
                       const std::unordered_set<std::string>*             indyFunctions);
/// 发射缓存化的 JNI 调用点（jvalue 数组 + CallXxxMethodA）。
/// ownerMethodOnly：仅按声明 owner 解析 methodID（GetMethodID 一次 + 缓存，
/// 之后每调用 0 JNI 解析；CallXxxMethodA 的 JNI 虚分派保证语义正确）。
void emitCachedCall(CSourceStream& out, const RegInstruction& ins, bool staticCall,
                    const ca::str::Utf8StringRef&            cacheName,
                    const std::vector<ca::str::Utf8String>& expressions,
                    bool ownerMethodOnly = false);
/// 发射「分配 + 构造」融合调用（NewObjectA：1 个 JNI 替代 AllocObject + 解析）。
void emitAllocateWithInit(CSourceStream& out, RegCFunctionEmitState& state,
                          const RegInstruction& alloc, const RegInstruction& initCall,
                          const ca::str::Utf8StringRef& allocCacheName,
                          const std::vector<ca::str::Utf8String>& expressions);
/// Allocate 与紧邻 <init> 调用可否融合为 NewObjectA（语义条件判定）。
bool canFuseAllocateInit(const RegCFunctionEmitState& state, const RegInstruction& alloc,
                         const RegInstruction& call);
/// 发射动态实例调用的 resolved native dispatch。
void emitResolvedInstanceCall(CSourceStream& out, const RegInstruction& ins,
                              const ca::str::Utf8StringRef&            siteName,
                              const std::vector<ca::str::Utf8String>& expressions,
                              bool omitDeleteLocalRef = false);
/// @}

/// @name regc_intrinsics.cpp：native 对象分析与 intrinsic 调用发射
/// @{

/// 发射一次状态检查（_mj2x_regc_report_status），用于 StringBuilder/StringBuffer 操作。
void emitRegCStatusCheck(CSourceStream& out, RegCFunctionEmitState& state,
                         const ca::str::Utf8StringRef& statusExpr,
                         const std::string&            operation);
/// native 包装对象（StringBuilder/StringBuffer）的局部变量名。
ca::str::Utf8String nativeObjectStorageName(RegId id);
/// 若操作数为待 materialize 的 StringBuilder/StringBuffer，发射 materialize 代码并改写表达式。
bool emitMaterializedStringBuilder(CSourceStream& out, RegCFunctionEmitState& state,
                                   const RegOperand& operand, ca::str::Utf8String& expression);
/// 拒绝 native 包装对象经指定表面（field store/array store/call 参数）逃逸。
bool rejectNativeObjectEscape(const RegCFunctionEmitState& state, const RegOperand& operand,
                              const std::string& surface, std::string& reason);
bool rejectNativeObjectEscapes(const RegCFunctionEmitState&   state,
                               const std::vector<RegOperand>& operands,
                               const std::string&            surface, std::string& reason);
/// 分析函数内登记的 native 包装寄存器集合（含参数传入的包装对象）。
std::set<uint32_t> analyzeNativeWrappers(const RegFunction& fn);
/// 分析 StringBuilder/StringBuffer 的分配/连接/逃逸，产出 materialize 计划。
/// registry 用于识别 StringBuilder 方法 intrinsic（nullptr 时用 builtins()）。
RegCStringBuilderPlan analyzeStringBuilders(const RegFunction& fn,
                                            const RegCIntrinsicRegistry* registry = nullptr);
/// 尝试发射 intrinsic 调用（StringBuilder/StringBuffer/字符串运行时方法）。
RegCIntrinsicDecision emitIntrinsicCall(CSourceStream& out, const RegInstruction& ins,
                                        RegCFunctionEmitState& state);
/// @}

/// @name regc_flow.cpp：指令流发射与寄存器收集
/// @{

/// 登记一个寄存器值到类型表；同寄存器类型冲突返回 false。
bool collectRegister(std::map<uint32_t, SSAType>& regs, RegValue value);
/// 收集函数参数/块内指令/异常边产生的全部寄存器类型。
bool collectRegisters(const RegFunction& fn, std::map<uint32_t, SSAType>& regs,
                      std::string& reason);
/// 取保护指定 block 的全部异常区域（按声明顺序）。
std::vector<const RegExceptionRegion*> activeExceptionRegions(const RegFunction& fn,
                                                              const RegBlock*    block);
/// 发射函数级 helper 前导声明（cached/instance 调用等）。
void emitHelperPrelude(ca::str::Utf8StringBuilder& out);
/// 判断已发射函数体是否会产生 JNI local ref（gate PushLocalFrame/PopLocalFrame）。
bool bodyProducesLocalRefs(const ca::str::Utf8StringRef& body);
/// 发射异常区域分发表（_mj2x_regc_exception_check + 区域边界拷贝）。
bool emitExceptionDispatch(CSourceStream& out,
                           const std::vector<const RegExceptionRegion*>& regions,
                           const RegBlock* block, SSAType returnType,
                           RegCFunctionEmitState& state, int& dispatchIndex,
                           int& edgeTempIndex, std::string& reason);
/// 发射一条指令（表达式/JNI 调用/字段/数组/分配/native 包装等）。
/// block 为指令所在块：receiver 精确类型静态绑定（分配点传播）需要回溯
/// 前驱边拷贝查询 SSA def。
bool emitInstruction(CSourceStream& out, const RegInstruction& ins,
                     RegCFunctionEmitState& state, const RegFunction& fn, const RegBlock* block,
                     size_t indySiteBase, std::string& reason);
/// 发射块终结符（跳转/分支/switch/返回/异常），含跨边引用拷贝。
bool emitTerminator(CSourceStream& out, const RegTerminator& terminator,
                    const RegBlock* block, SSAType returnType,
                    const std::vector<const RegExceptionRegion*>& activeRegions,
                    RegCFunctionEmitState& state, int& dispatchIndex, int& edgeTempIndex,
                    std::string& reason);
/// @}

/// Emit one invokedynamic site call.  Defined in regc_emitter.cpp.
bool emitInvokeDynamic(codegen::CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       std::string& reason);

}   // namespace mj2x
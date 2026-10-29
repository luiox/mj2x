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
#include "mj2x/frontend/type_utils.hpp"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

/// @brief 函数级 intrinsic 判定表（定义见下方 class 模板区；emitState 持指针）。
class RegCIntrinsicDecisionTable;

// 与既有 cpp 一致：CSourceStream 从 codegen 起别名，函数签名保持一行。
/// @brief REF 寄存器并查集：把"同一个对象的多个寄存器"归并到一个根。
///
/// @details
/// StringBuilder/StringBuffer 的 native lowering 与 POD struct 降级共用——两份
/// 副本必然漂移，故放在这里。逃逸分析的第一步都是"这个寄存器指的是哪个对象"。
class RegIdUnionFind
{
public:
    void add(uint32_t id) { parent_.emplace(id, id); }

    uint32_t find(uint32_t id)
    {
        add(id);
        uint32_t parent = parent_[id];
        if (parent == id) return id;
        parent_[id] = find(parent);
        return parent_[id];
    }

    void unite(uint32_t lhs, uint32_t rhs)
    {
        const uint32_t lhsRoot = find(lhs);
        const uint32_t rhsRoot = find(rhs);
        if (lhsRoot != rhsRoot) parent_[rhsRoot] = lhsRoot;
    }

    const std::map<uint32_t, uint32_t>& entries() const { return parent_; }

private:
    std::map<uint32_t, uint32_t> parent_;
};

using codegen::CSourceStream;

/// 方法唯一键（owner\nname\ndesc）：convertedFunctions / indyFunctions /
/// enumSyntheticMethods 的跨 TU 统一构建点（原 regc_backend.cpp TU 内静态
/// helper 下沉，评审 nit N6）。键格式逐字节冻结——查表写入端
/// （compileMethod 预登记/自登记）与读取端（直调/resolved 表还原按 '\n'
/// 切三段）都依赖三段式布局，任何转义/分隔符语义变化都是产物级漂移。
inline std::string methodKey(const std::string& owner, const std::string& name,
                             const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

/// @brief RegId 对应的 native 对象状态（装箱值/Random/StringBuilder 等 intrinsic 载体）。
struct RegCNativeObjectState
{
    RegCIntrinsicNativeObjectKind kind = RegCIntrinsicNativeObjectKind::None;
    std::string                   storageName;
    /// 仅 PodStruct 用：结构体的 C 类型名。其余 native 态对象（Random / 包装类
    /// 等）的类型由 kind 唯一决定，不需要这一项。
    std::string                   typeName;
};

/// @brief 调用目标间接化的站点条目（对标 jvmtp `obfuscation.calls.*`）。
///
/// @details
/// 站表由生成工程 emit：调用点只带 `(表, 组内索引)`，目标身份字符串
/// （owner/name/desc）从调用点移出，静态分析须先解析索引才能还原调用边。
struct RegCIndirectCallSite
{
    std::string owner;
    std::string name;
    std::string desc;
    int32_t     invokeKind = 0;   ///< 0 = virtual/interface，1 = static，2 = special
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
    /// native-to-native 直接调用档位（CodeOptions.directCallTier；默认 Off 保持
    /// JVM 栈帧语义，见 c_options.hpp 的 DirectCallTier）。
    codegen::DirectCallTier                   directCallTier = codegen::DirectCallTier::Off;
    /// computed jumps：switch 目标间接化开关（默认关）。
    bool                                      computedJumps  = false;
    /// 本函数已用过的 computed-jumps 站点计数（表名唯一化）。
    size_t                                    computedJumpSites = 0;
    /// native-to-native 直接调用上下文（compileMethod 传入，nullptr 关闭）。
    CompilerIndex*                            classIndex          = nullptr;
    const std::unordered_map<std::string, std::string>* convertedFunctions = nullptr;
    /// 函数名 → 由被调方签名生成的规范前向声明文本（直调声明的唯一权威，
    /// 见 directCallDeclFor）。nullptr 时回退按调用点推导。
    const std::unordered_map<std::string, std::string>* convertedFunctionDecls = nullptr;
    const std::unordered_set<std::string>*    indyFunctions       = nullptr;
    /// intrinsic 匹配 registry（issue #818）：nullptr 时用 builtins()。
    /// RegCBackend::setUserIntrinsics 构造带用户条目的 registry 实例后设置。
    const RegCIntrinsicRegistry*              intrinsicRegistry   = nullptr;
    /// 函数级 intrinsic 判定表（T1）：emitFunction 入口以与 intrinsicRegistry
    /// 同源的 registry 实例预计算；发射期全部 intrinsic 判定取此表，
    /// intrinsicRegistry 仅作声明发射等表外用途。nullptr（单测直连发射器
    /// 未设表）时 emitIntrinsicCall 回退现场 matchCall。
    const RegCIntrinsicDecisionTable*         decisions           = nullptr;
    /// 后端预扫描验证过的 javac 合成枚举 values()/valueOf() 方法键集合
    /// （owner\nname\ndesc）。方法注入 native 后指令被清空，发射期无法再
    /// 现场验证方法体；nullptr（单测直调发射器）时回退现场验证。
    const std::set<std::string>*              enumSyntheticMethods = nullptr;
    /// 文件作用域前向声明注册表（backend TU 级）：方法间直调的声明登记于此，
    /// 渲染到翻译单元头部（多文件模式经 splitRewriteStatic 提取进 api.h）。
    /// C11 6.7.1p6 禁止块作用域函数声明带 static 存储类（GCC 报
    /// invalid storage class，MSVC 静默放行）；nullptr（单测直连发射器）时
    /// 回退块内声明。
    codegen::CForwardDeclarations*            forwardDecls        = nullptr;
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
    /// POD struct 降级：可降级实例的并查集根 -> 类名。
    std::map<uint32_t, std::string>          podStructRoots;
    std::vector<uint32_t>                     nativeStringBuilderOwners;
    std::map<uint32_t, uint32_t>              nativeStringBuilderOwnerByRegister;
    std::set<uint32_t>                        nativeWrapperRegisters;
    /// Edge-copy temporary types actually emitted, indexed by edgeTempIndex.
    /// Captured during emit so the declaration block declares exactly the temporaries
    /// that are used (a block with multiple mayThrow instructions re-emits its
    /// exception edge copies once per throw site, so the count can exceed the static
    /// CFG edge count).
    std::vector<SSAType>                      edgeTempTypes;
    /// 调用目标间接化站点表：按返回形态（dispatcher 后缀）分组，每组一张表、
    /// 组内独立索引。按指令遍历序登记——与 pass 遍历序一致，产物可复现。
    std::map<std::string, std::vector<RegCIndirectCallSite>> indirectSiteTables;
    std::map<std::string, std::string>                       indirectSiteTableNames;
    int                                                     indirectSiteTableIndex = 0;
};

/// @brief analyzePodStructs 输出：可降级为 C struct 的寄存器根集合。
///
/// @details
/// `structRoots` 是不逃逸的 POD 实例根（并查集根 id）；`ownerByRoot` 给出每个
/// 根的类名（用于取 struct 类型名）。`escapedRoots` 保留供诊断——降级覆盖面窄
/// 时需要能说出"为什么这个实例没降级"。
struct RegCPodStructPlan
{
    std::set<uint32_t>              structRoots;
    std::set<uint32_t>              escapedRoots;
    std::map<uint32_t, std::string> ownerByRoot;
    /// 并查集里属于 POD 根的**全部寄存器** -> 类名。结构体变量必须在函数作用域
    /// 预声明（边拷贝发生在块作用域，块内声明跨块即不可见），故需要完整集合
    /// 而不只是根。
    std::map<uint32_t, std::string> ownerByRegister;
    /// 真正 `new` 出来的寄存器（别名的背后实例）。只有它们需要 backing struct
    /// 变量；其余寄存器是指针别名。
    std::set<uint32_t>              allocationRegisters;
};

/// @brief analyzeStringBuilders 输出：native StringBuilder 寄存器集合与归属关系。
struct RegCStringBuilderPlan
{
    std::set<uint32_t>                                nativeRegisters;
    std::vector<uint32_t>                             ownerRegisters;
    std::map<uint32_t, uint32_t>                      ownerByRegister;
    std::map<uint32_t, RegCIntrinsicNativeObjectKind> kindByRegister;
};

/// @name 字符串字面量 at-rest 加密上下文（--strings-encrypt，regc_expr.cpp 持有）
///
/// operandExpr 是无状态自由函数、调用点遍布各拆分文件；ldc_string 分支需要
/// 登记密文缓冲的池。发射期间由 RegCEmitter::emitFunction 经 RAII 安装当前池
/// （单线程发射，无并发），函数外为空——字面量保持明文发射（独立发射器/单测
/// 路径默认不受影响）。
/// @{

/// 取当前加密登记池（nullptr = 未启用，ldc_string 走明文发射）。
RegCCachePool* currentStringEncryptPool();
/// 安装/清除当前加密登记池（emitFunction 入口/出口调用）。
void setCurrentStringEncryptPool(RegCCachePool* pool);

/// RAII 安装守卫：构造安装，析构恢复为空。
class RegCStringEncryptScope
{
public:
    explicit RegCStringEncryptScope(RegCCachePool* pool) { setCurrentStringEncryptPool(pool); }
    ~RegCStringEncryptScope() { setCurrentStringEncryptPool(nullptr); }
    RegCStringEncryptScope(const RegCStringEncryptScope&)            = delete;
    RegCStringEncryptScope& operator=(const RegCStringEncryptScope&) = delete;
};

/// @}

/// @name regc_expr.cpp：类型/表达式/命名 helper
/// @{

const char* cType(SSAType type);

/// @brief 方法间直调的前向声明登记（三处直调发射共用，见 regc_calls/regc_flow）。
/// @details 声明文本与 emitFunction 参数发射一致（receiver = void* r0，后续按
///          实参 SSAType）。state.forwardDecls 非空时登记到 TU 头部注册表：
///          块作用域函数声明带 static 违反 C11 6.7.1p6（GCC 报 invalid storage
///          class，MSVC 静默放行），声明必须落在文件作用域；注册表为空时回退
///          块内声明（单测直连发射器路径，仅 MSVC 兼容）。
/// @brief 由返回类型 + 参数类型表生成规范前向声明文本。
///
/// @details
/// 声明必须与被调方的函数定义逐字一致（同一 `cType` 映射、同一参数序）。不能用
/// 调用点的操作数类型推导：JVM 窄类型（bool/byte/char/short）在调用点可能被
/// 提升为 I32，同一函数在两个调用点上算出的声明文本不同，去重按文本做就会登记
/// 两条（或与定义本身冲突），C 编译器报 conflicting types。
inline std::string directCallDeclForTypes(const std::string& fnName, SSAType returnType,
                                          const std::vector<SSAType>& parameterTypes)
{
    std::string decl = "static ";
    decl += cType(returnType);
    decl += " ";
    decl += fnName;
    decl += "(";
    for (size_t i = 0; i < parameterTypes.size(); ++i) {
        if (i) decl += ", ";
        decl += cType(parameterTypes[i]);
    }
    decl += ");";
    return decl;
}

/// @brief 由**被调方** RegFunction 生成规范前向声明文本。
/// @details 见 directCallDeclForTypes。
inline std::string directCallDeclFor(const std::string& fnName, const RegFunction& fn)
{
    std::vector<SSAType> parameterTypes;
    parameterTypes.reserve(fn.parameters.size());
    for (const auto& parameter : fn.parameters) parameterTypes.push_back(parameter.type);
    return directCallDeclForTypes(fnName, fn.symbol.returnType, parameterTypes);
}

inline void declareDirectCallTarget(RegCFunctionEmitState& state, CSourceStream& out,
                                    const RegInstruction& ins, const std::string& fnName)
{
    // 有被调方签名时优先用它（与定义逐字一致）；否则回退按调用点推导（类级
    // 预登记阶段尚无签名，维持原行为）。
    if (state.convertedFunctionDecls != nullptr) {
        const auto declIt = state.convertedFunctionDecls->find(fnName);
        if (declIt != state.convertedFunctionDecls->end()) {
            if (state.forwardDecls != nullptr) {
                state.forwardDecls->declare(declIt->second);
                return;
            }
            out.appendLineStart(declIt->second).newLine();
            return;
        }
    }
    std::string decl;
    decl += "static ";
    decl += ins.hasDest ? cType(ins.dest.type) : "void";
    decl += " ";
    decl += fnName;
    decl += "(";
    for (size_t i = 0; i < ins.args.size(); ++i) {
        if (i) decl += ", ";
        decl += cType(ins.args[i].type);
        decl += " r";
        decl += std::to_string(i);
    }
    decl += ");";
    if (state.forwardDecls != nullptr) {
        state.forwardDecls->declare(decl);
        return;
    }
    out.appendLineStart(decl).newLine();
}

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

/// @brief 由**方法描述符**生成同样的声明（被调方尚未 lowering 时用）。
///
/// @details
/// 类级预登记阶段只有 MethodNode，没有 RegFunction；但直调声明必须在任何调用
/// 方发射之前就备好（调用方可能先于被调方编译），故走描述符算出同一份签名。
/// 参数序与 ssa_builder 一致：非静态方法首位是 receiver（REF）。
inline std::string directCallDeclForDescriptor(const std::string& fnName, const std::string& desc,
                                              bool isStatic)
{
    const std::vector<char> tags = descriptorParamTags(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    std::vector<SSAType>    parameterTypes;
    if (!isStatic) parameterTypes.push_back(SSAType::REF);
    for (char tag : tags) {
        const std::string slot(1, tag);
        parameterTypes.push_back(descriptorSlotToSSA(slot));
    }
    const char  returnTag = descriptorReturnTag(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    const SSAType returnType =
        returnTag == '\0' ? SSAType::VOID : descriptorSlotToSSA(std::string(1, returnTag));
    return directCallDeclForTypes(fnName, returnType, parameterTypes);
}
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
/// 以下 5 个 add*Cache 返回值均为视图：池路径返回池内 intern 的 cacheName 视图
///（H2 返回端，命中零 clone）；无池路径（单测直连发射器）返回 emitState 拥有的
/// map 值视图（std::map 节点地址稳定）。视图寿命覆盖单次发射/整 TU 生成。
ca::str::Utf8StringRef addStaticMethodCache(RegCFunctionEmitState& state,
                                            const RegInstruction&  ins);
ca::str::Utf8StringRef addInstanceMethodCache(RegCFunctionEmitState& state,
                                              const RegInstruction&  ins);
ca::str::Utf8StringRef addStaticFieldCache(RegCFunctionEmitState& state,
                                           const RegInstruction&  ins);
ca::str::Utf8StringRef addInstanceFieldCache(RegCFunctionEmitState& state,
                                             const RegInstruction&  ins);
ca::str::Utf8StringRef addClassCache(RegCFunctionEmitState&       state,
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
void emitCachedCall(CSourceStream& out, RegCFunctionEmitState& state, const RegInstruction& ins,
                    bool staticCall,
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
/// @brief 函数级 intrinsic 判定表（T1 判定热点 + registry 视图统一）。
///
/// @details
/// emitFunction 入口对全部 Call 指令以唯一 registry 实例算一次 matchCall，
/// StringBuilder/wrapper 分析（各两遍）、local-ref 帧判定、异常检查稀疏化
/// 与 emitIntrinsicCall 共享同一份决策。此前各消费点自行重复判定（每 Call
/// 至多 6 次 fromInstruction + 179 条目线性扫），且 analyze/稀疏化侧硬编码
/// builtins() 而 emit 侧用用户 registry——用户条目遮蔽内建签名时两侧判定
/// 漂移（wrapper 分析按 WrapperValueOf 建 native 态、emit 按 UserIntrinsic
/// 出 jobject 的错配；纯计算豁免 ExceptionCheck 套在可抛用户函数上）。
/// 表键取指令地址：RegFunction 块/指令为 unique_ptr 所有权，单次发射内
/// 地址稳定。
class RegCIntrinsicDecisionTable
{
public:
    RegCIntrinsicDecisionTable(const RegFunction&             fn,
                               const RegCIntrinsicRegistry& registry)
    {
        ca::usize callCount = 0;
        for (const auto& block : fn.blocks) callCount += block->instructions.size();
        byCall_.reserve(callCount);
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block->instructions) {
                if (ins.kind != RegInstructionKind::Call) continue;
                byCall_.emplace(&ins, registry.matchCall(ins));
            }
        }
    }

    /// @brief Call 指令的预计算判定。
    /// @details 构造覆盖函数内全部 Call；非 Call 指令或跨函数查询返回缺省
    /// Fallback（消费方语义即"按通用 JNI 派发处理"，与 miss 时保守行为一致）。
    const RegCIntrinsicDecision& at(const RegInstruction& ins) const
    {
        const auto it = byCall_.find(&ins);
        return it != byCall_.end() ? it->second : fallback_;
    }

private:
    std::unordered_map<const RegInstruction*, RegCIntrinsicDecision> byCall_;
    /// at() 对表外指令的保守应答（通用派发语义）。
    RegCIntrinsicDecision fallback_;
};

/// 分析函数内登记的 native 包装寄存器集合（含参数传入的包装对象）。
/// 判定一律取决策表（与 emit 同一 registry 视图）。
std::set<uint32_t> analyzeNativeWrappers(const RegFunction& fn,
                                         const RegCIntrinsicDecisionTable& decisions);
/// 分析 StringBuilder/StringBuffer 的分配/连接/逃逸，产出 materialize 计划。
/// 判定一律取决策表（与 emit 同一 registry 视图）。
RegCStringBuilderPlan analyzeStringBuilders(const RegFunction& fn,
                                            const RegCIntrinsicDecisionTable& decisions);

/// @brief 判定某类的实例字段是否全为原生类型（POD 类）。
/// @details 同时要求无接口、直接继承 Object——继承/接口会让字段布局不等于本类
///          字段表，struct 降级不再等价。库类一律否（拿不到字段全貌）。
bool isPodClassOwner(CompilerIndex* classIndex, const ca::str::Utf8StringRef& owner);
/// @brief POD 类生成的 C struct 类型名（pod_<sanitized owner>）。
std::string podStructTypeName(const ca::str::Utf8StringRef& owner);
/// @brief 字段名 → C struct 成员名（sanitize，合成字段的 `$` 也要处理）。
std::string podStructMemberName(std::string_view name);
/// @brief POD struct 实例的**指针别名**变量名。
///
/// @details
/// 别名必须是指针：同一个对象在 RegIR 里可以有多个寄存器（phi / 边拷贝），
/// 值语义的 struct 拷贝会让它们变成互不影响的独立副本，与 Java 的引用语义
/// 不等价（实测产物算错结果）。指针拷贝才是别名。
std::string podStructPtrName(RegId id);
/// @brief 分配点的 backing struct 变量名（指针别名指向它）。
std::string podStructValueName(RegId id);
/// @brief struct 的一个成员声明（名称 + C 类型文本）。
struct RegCPodStructMember
{
    std::string name;
    std::string cType;
};
/// @brief 某 POD 类的实例字段成员表（按类字段表顺序，只含原生字段）。
std::vector<RegCPodStructMember> podStructMembers(CompilerIndex* classIndex,
                                                 const std::string& owner);
/// @brief 取操作数对应的 POD struct 存储名（非 POD struct 返回 nullptr）。
const std::string* podStructStorageFor(const RegCFunctionEmitState& state,
                                      const RegOperand& operand);
/// @brief 发射 computed jumps 的分派前奏：查表解析出块索引。
///
/// @details
/// 成功时 `indexExpression` 为块索引变量名，调用方改按 0..N-1 的索引 switch。
/// 返回 false（无 TU 头部注册表，key 表无处安放）时 `indexExpression` 保持空，
/// 调用方**退回按原始 case key 分派**——本项是混淆增益，不该让方法编译不了。
bool emitComputedJumpDispatch(CSourceStream& out, RegCFunctionEmitState& state,
                              const RegSwitchTerminator& t, std::string& indexExpression,
                              std::string& reason);
/// @brief POD struct 的 CFG 边拷贝（按值拷贝结构体，非 POD 时不做任何事）。
void podStructEdgeCopy(CSourceStream& out, RegCFunctionEmitState& state, const RegCopy& copy);
/// @brief POD struct 实例字段读（命中返回 true 表示已发射）。
bool emitPodStructFieldLoad(CSourceStream& out, RegCFunctionEmitState& state,
                            const RegInstruction& ins);
/// @brief POD struct 实例字段写（命中返回 true 表示已发射）。
bool emitPodStructFieldStore(CSourceStream& out, RegCFunctionEmitState& state,
                             const RegInstruction& ins, std::string& reason);
/// @brief 逐函数判定哪些 POD 实例可以不逃逸地降级为 C struct。
/// @details 逃逸判定保守：白名单（实例字段读写 + <init> receiver）之外一律逃逸。
///          漏判逃逸会导致数据静默错乱，宁可覆盖面小。
RegCPodStructPlan analyzePodStructs(const RegFunction& fn, CompilerIndex* classIndex);
/// 尝试发射 intrinsic 调用（StringBuilder/StringBuffer/字符串运行时方法）。
RegCIntrinsicDecision emitIntrinsicCall(CSourceStream& out, const RegInstruction& ins,
                                        RegCFunctionEmitState& state);
/// 编译输入内 owner 的超类链抵达 java/lang/Enum（枚举子类判定）。
bool isEnumSubtypeOwner(CompilerIndex* classIndex, std::string_view owner);
/// javac 合成 values() 形态验证（getstatic $VALUES + clone + checkcast + areturn；
/// 批 6d：DOM raw 指令匹配，cls/m = 目标类与方法快照）。
bool isJavacEnumValuesBody(const CompilerClass& cls, const CompilerMethod& method,
                           const std::string& owner, const std::string& arrayDesc);
/// javac 合成 valueOf(String) 形态验证（ldc class + Enum.valueOf + checkcast + areturn）。
bool isJavacEnumValueOfBody(const CompilerClass& cls, const CompilerMethod& method,
                            const std::string& owner);
/// @}

/// @name regc_backend.cpp：枚举合成方法 SSA 形状验证（#887 Enums 承接卡）
/// @{

/// javac 合成 values() 的 SSA 形状验证：getstatic owner.$VALUES → invokevirtual
/// clone（owner 为数组描述符或 java/lang/Object）→ checkcast 数组描述符，同块
/// 3 指令（按块搜索，容忍空 entry 块）。与字节码侧 isJavacEnumValuesBody 同一
/// 口径的 fail-closed SSA 版：方法自身指令清空后字节码侧不可达，只能在其 SSA
/// 转换当口验证。仅 values——valueOf 无 SSA 形状验证：SSA 侧形状门弱于字节码
/// 门（可见证据严格子集，opaque tag 不可靠），旧 valueOf 谓词已删（#1192 评审
/// N4 承接，方案 B），合成验证唯一来源是字节码侧 isJavacEnumValueOfBody
/// 预扫描/按类补扫。
bool isJavacEnumValuesSsa(const SSAFunction& fn);
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
/// 发射函数级 helper 前导声明（cached/instance 调用等）。registry 非空时（用户
/// 自定义 intrinsic registry）改从该实例发射——其 emitDeclarations 覆盖 builtins
/// 固定声明并追加用户 symbol 的 extern 声明。
void emitHelperPrelude(ca::str::Utf8StringBuilder& out,
                       const RegCIntrinsicRegistry* intrinsicRegistry = nullptr);
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
/// fn/block 供直派面 null receiver 守卫做块保护分流（issue #958）。
bool emitInvokeDynamic(codegen::CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       const RegFunction& fn, const RegBlock* block, std::string& reason);

/// 发射 invokedynamic 站点直派桩（设计文档 §6 P2）：按站点派发形态分派——
/// F1（DirectFieldAccess+载荷）发字段访问直派、F2（RecordObjectMethods+访问器载荷）
/// 发访问器直调，形态为单分支守卫（READY 才走快路，回退分支即既有 helper 路径）；
/// 其余形态原样落现状 helper 路径（发射文本与现状逐字节一致）。定义在
/// regc_emitter.cpp。
bool emitIndyDirectCall(codegen::CSourceStream& out, const RegInstruction& ins,
                        const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                        const RegFunction& fn, const RegBlock* block, std::string& reason);

}   // namespace mj2x
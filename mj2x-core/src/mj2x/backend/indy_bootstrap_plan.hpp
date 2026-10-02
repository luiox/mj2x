/// @file indy_bootstrap_plan.hpp
/// @brief invokedynamic bootstrap 解析单源（#1027 B3）：三后端（StackC/RegC/
///        LLVM）各自的 bsm 形参游走、Handle tag→Lookup.find* 表、bsm 常量
///        分类、装箱类名映射收敛到本模块。后端只做各自的发射文本，不做判定。
///
/// @details
/// 三份游走的公共结构（切换前 StackC stackc_mir_invoke.cpp / RegC
/// regc_backend.cpp / LLVM llvm_ir_helpers.cpp 逐分支一致）：
///   形参 0/1/2 = Lookup / 站点名 / 站点 MethodType；
///   尾随 [Ljava/lang/Object; 形参 = 剩余 bsmArgs 打包 varargs；
///   形参多于剩余 bsmArgs → NULL 补位；
///   其余形参 = 消费一个 bsmArgs 静态参数。
/// 本模块只产出"哪个槽消费什么"的解析结构（与发射文本解耦）；各槽的物化
/// 方式仍是后端策略（StackC 按形参原语宽度、RegC/LLVM 按 Any 动态类型——
/// 差异是发射面契约，不属于解析）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "libca/core/any.hpp"

namespace mj2x {

/// bsm 形参槽的种类（游走结论载荷）。
enum class IndyBsmSlotKind
{
    Lookup,     ///< 形参 0：MethodHandles.Lookup（后端各自变量名）。
    SiteName,   ///< 形参 1：站点名。
    SiteType,   ///< 形参 2：站点 MethodType。
    StaticArg,  ///< 静态参数槽：消费 bsmArgs[bsmArgIndex]。
    Varargs,    ///< 尾随 Object[]：打包 bsmArgs[bsmArgIndex .. +varargCount)。
    Null,       ///< 形参存在但 bsmArgs 已耗尽 → NULL 补位。
};

struct IndyBsmSlot
{
    IndyBsmSlotKind kind       = IndyBsmSlotKind::Null;
    size_t          bsmArgIndex = 0;   ///< StaticArg：参数下标；Varargs：起始下标。
    size_t          varargCount = 0;   ///< 仅 Varargs：打包参数个数。
    int             formalSort  = 0;   ///< 仅 StaticArg：形参 Type::Sort 值（StackC
                                       ///  按形参宽度装箱用；其余后端可忽略）。
};

/// @brief bsm 形参游走单源（bsmDesc = bootstrap 方法描述符，bsmArgCount =
///        site.bsmArgs.size()）。返回值即逐槽解析结论，三后端一致。
std::vector<IndyBsmSlot> planIndyBsmSlots(const std::string& bsmDesc, size_t bsmArgCount);

/// Handle tag 实参形状（Lookup.find* 调用的尾参布局）。
enum class IndyHandleArgShape
{
    NameFieldType,   ///< findGetter/findStaticGetter/findSetter/findStaticSetter。
    NameType,        ///< findStatic / findVirtual（默认）。
    NameTypeOwner,   ///< findSpecial：尾参 owner class。
    TypeOnly,        ///< findConstructor：不传站点名。
};

struct IndyHandleFind
{
    const char*        findName;
    const char*        findDesc;   ///< Lookup.find* 方法描述符。
    IndyHandleArgShape argShape;
    bool               emitName;   ///< findConstructor 不发站点名字符串。
};

/// @brief Handle tag → Lookup.find* 形态（#1027 B3 单源：StackC/RegC 两份
///        同式 switch 收敛；LLVM 走运行时 helper 不需要本表）。
///        未知 tag → findVirtual 默认形态（与切换前 default 分支一致）。
IndyHandleFind indyHandleFindFor(int handleTag);

/// bsm 常量分类（emitAnyAsObject / emitIndyBsmArg / indyBsmArgExpr 三份
/// if 链的公共判定序：String → Utf8StringRef → Type(METHOD?MethodType:Class)
/// → Handle → i32 → i64 → float → double → 未知 Null）。
enum class IndyBsmArgKind
{
    String,
    MethodType,
    Class,
    Handle,
    Int,
    Long,
    Float,
    Double,
    Null,
};

IndyBsmArgKind classifyIndyBsmArg(const ca::core::Any& value);

/// @brief JVM 描述符首字符 → 装箱类内部名（原语 Class 物化共用）。
///        Z/B/C/S/I/J/F/D → 对应包装类；其余（L/[，含空串）→ java/lang/Object
///        （与 RegC 原 indyBoxedClassName 逐值一致）。
const char* indyBoxedClassNameForDescHead(char descHead);

}   // namespace mj2x

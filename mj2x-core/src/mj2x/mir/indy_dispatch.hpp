/// @file indy_dispatch.hpp
/// @brief invokedynamic 站点静态派发形态载荷（MIR 可见的判定结果载体）。
///
/// @details
/// 判定单源在管线层（InvokeDynamicPreprocessor::classifyIndySite /
/// classifyIndySiteForm，判定树见 doc_ai/indy_native_dispatch_design.md §2）；
/// 本头文件只承载判定结论的值载荷：SSAFunction::InvokeDynamicSite 与
/// RegFunction::InvokeDynamicSite 持有 form + 载荷，值拷贝透传到后端。
/// 发射端按 form 查表消费，不做任何判定逻辑（后端零判定）。
#pragma once

#include <optional>
#include <string>

namespace mj2x {

/// invokedynamic 站点静态派发形态（判定树顶层结论，见设计文档 §2）。
enum class IndyDispatchForm
{
    /// 回退阶段3 运行时 call-site helper（现状路径；reason 区分具体原因）。
    RuntimeHelper,
    /// 已被本 preprocessor 阶段1/2 静态展开为普通字节码（concat / LMF 合成类）。
    StaticExpanded,
    /// LMF 字段句柄 impl 的精确一致子集 → 直接字段访问直派（RegC 发射）。
    DirectFieldAccess,
    /// java/lang/runtime/ObjectMethods.bootstrap（record）→ 静态可命名 target 直派。
    RecordObjectMethods,
};

/// form==DirectFieldAccess 时的载荷：字段读/写的精确目标（判定即承诺，无任何适配）。
struct IndyDirectField
{
    bool        isStatic = false;   ///< true = GETSTATIC/PUTSTATIC。
    bool        isWrite  = false;   ///< true = PUTSTATIC/PUTFIELD（写字段）。
    std::string owner;              ///< 声明类内部名。
    std::string name;               ///< 字段名。
    std::string fieldDesc;          ///< 字段描述符（发射端按其选 JNI 变体）。
};

/// form==RecordObjectMethods 时的载荷：record 类 + 组件名/描述符集 + 可直派访问器。
struct IndyRecordMethods
{
    /// 可直派访问器（判定内唯一匹配 indy 名且调用布局逐位一致的 bsmArgs[3..] 句柄）。
    struct Accessor
    {
        int         tag;    ///< jbytecode Handle tag（H_INVOKESTATIC / H_INVOKEVIRTUAL / H_INVOKEINTERFACE）。
        std::string owner;  ///< 声明类内部名。
        std::string name;   ///< 访问器名。
        std::string desc;   ///< 访问器描述符。
    };

    std::string recordClass;   ///< bsmArgs[0] 的 record 类内部名。
    std::string names;         ///< bsmArgs[1] 的组件名集（';' 分隔）。
    std::string methodsDesc;   ///< bsmArgs[2] 的方法描述符集。
    /// nullopt = 无可直派访问器（javac equals/hashCode/toString 等 ObjectMethods
    /// 内建语义形态），发射端走既有 helper 路径（与现状逐字节一致）。
    std::optional<Accessor> accessor;
};

/// 站点级判定结论（classifyIndySiteForm 返回值；SSA 站点登记时解包进 InvokeDynamicSite）。
struct IndyDispatchPayload
{
    IndyDispatchForm                form = IndyDispatchForm::RuntimeHelper;
    std::optional<IndyDirectField>   directField;
    std::optional<IndyRecordMethods> recordMethods;
};

}   // namespace mj2x

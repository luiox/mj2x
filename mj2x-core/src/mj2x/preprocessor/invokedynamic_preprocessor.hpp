// mj2x InvokeDynamicPreprocessor — invokedynamic 站点处理。
//
// StackC 在 MIR 层保留 bootstrap 元数据，并在 native 侧通过 bootstrap +
// MethodHandle.invokeWithArguments 执行。RegC 是无栈 IR，不走 runtime bridge，
// 而是在此 preprocessor 把可静态展开的 indy 改写成等价字节码（R6.1）。
//
// 现阶段（R6.1 阶段1/2/2b/2c/2d/2e/2f）已展开：
//   - java/lang/invoke/StringConcatFactory 的 makeConcatWithConstants / makeConcat
//     —— 改写成 new StringBuilder + append 链 + toString（阶段1）；
//   - java/lang/invoke/LambdaMetafactory 的 metafactory 与 altMetafactory —— 为每个站点
//     合成实现 functional interface 的类：无状态 + 捕获 lambda（阶段2/2b），实例
//     （H_INVOKEVIRTUAL/H_INVOKEINTERFACE/H_INVOKESPECIAL）与构造器
//     （H_NEWINVOKESPECIAL）方法引用（阶段2c），擦除泛型的装箱/桥接形态
//     （阶段2d：CHECKCAST 收窄 / 拆装箱 / POP 丢弃返回值），altMetafactory 的
//     serializable / marker interface / bridge 方法（阶段2e），以及基本类型加宽桥接
//     （阶段2f：i2l/i2d/l2d/f2d 等）。
// 展开后走 RegC 通用字节码路径转 native，绕开 RegC 缺 owner-class 的障碍。
// 仍 fail-closed 诊断的形态：基本类型收窄、装箱+加宽复合适配、自定义 bootstrap
//   （交由后续阶段处理）。
//
// indy native dispatch 判定（sprint 波次一原型，设计见 doc_ai/indy_native_dispatch_design.md）：
//   classifyIndySite 对每个站点按 classfile 静态可见内容判定派发形态（T0 静态展开 /
//   F1 字段直派 / F2 record 直派 / T2 运行时 helper），process 重载旁路收集判定报告。
//   判定只打标记，不改写任何字节码——对现有行为零回归。
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include "mj2x/mir/indy_dispatch.hpp"

#include <jbytecode/jbytecode.hpp>

#include <libca/core/any.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mj2x {

// IndyDispatchForm 及直派载荷结构自波次二起提升到 MIR 可见位置
// （mj2x/mir/indy_dispatch.hpp）：判定仍在管线层，载荷经 SSA/Reg 站点值拷贝
// 透传到后端；本头文件经包含保持既有引用（mj2x::IndyDispatchForm）不变。

/// 单个 indy 站点的判定结果（值语义，不持有节点指针——展开会销毁节点）。
struct IndyDispatchDecision
{
    IndyDispatchForm form = IndyDispatchForm::RuntimeHelper;
    /// 判定理由（稳定字面量，单测断言与编译期日志用）。
    const char* reason = "";
    /// 站点定位：所属类内部名 / 方法名 / 方法内第几个 indy（0 起，按指令序）。
    std::string className;
    std::string methodName;
    ca::i32     ordinal = 0;
    /// indy name（SAM 方法名）与调用点描述符。
    std::string indyName;
    std::string indyDesc;
    /// 直派载荷（与 InvokeDynamicSite 同源；载荷 engage 才非空——发射端据此直派）。
    std::optional<IndyDirectField>   directField;
    std::optional<IndyRecordMethods> recordMethods;
};

/// 一次 process 的判定报告（旁路产物，不影响字节码输出）。
struct IndyDispatchReport
{
    std::vector<IndyDispatchDecision> sites;

    /// 统计某形态的站点数。
    ca::usize count(IndyDispatchForm form) const;
};

class InvokeDynamicPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override;

    /// 批 4 唯一留树载体的 pass（1500 行树改写逻辑批 4b 收尾切 DOM）；
    /// pipeline 据此把它排在树载体段先跑。
    bool editsDom() const noexcept override { return false; }

    /// 同一流程 + 旁路收集判定报告（report 可为 null，等价 process(index)）。
    void process(CompilerIndex& index, IndyDispatchReport* report);
};

/// 判定单个 indy 站点的静态派发形态（设计文档 §2 判定树的可执行版本）。
///
/// @details
/// 纯函数：只读 indy 节点与 index（用编译输入类集合区分自定义 bootstrap 的 D1/D2 分支），
/// 不改写任何状态。B1（LMF 可展开子集）与 expandLambdaSite 共用同一前置谓词，
/// 保证「判定结果 = 展开行为」零漂移。
IndyDispatchDecision classifyIndySite(CompilerIndex& index,
                                      const jbytecode::tree::InvokeDynamicInsnNode* indy);

/// 站点级判定（SSA 登记站点时填充直派载荷的入口；设计文档 §6 P1）。
///
/// @details
/// 与 classifyIndySite 共用同一判定主体（判定单源）：输入是站点自持数据
/// （indy 名/描述符/bsm/bsmArgs——MIRInvokeDynamic 透传到 SSA 站点的全部信息），
/// 不需要 CompilerIndex。区别只有 D 分支理由细分（D1/D2 需要 index 查编译输入，
/// 只影响报告 reason 不影响 form）。返回的载荷经 SSAFunction::indySites 值拷贝
/// 透传到 RegFunction，发射端只消费不做判定。
IndyDispatchPayload classifyIndySiteForm(ca::str::Utf8StringArena& arena,
                                         const std::string&             indyName,
                                         const std::string&             indyDesc,
                                         const std::optional<jbytecode::core::Handle>& bsm,
                                         const std::vector<ca::core::Any>&             bsmArgs);

}   // namespace mj2x

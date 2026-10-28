#pragma once

/// @file        dom_inject.hpp
/// @brief       批 5：backend 注入（injectIntoClass）的 DOM 改写门面。
///
/// 四后端注入的改写权威从树切到 libjbc DOM（与 pre/postprocessor 的
/// openDomEdit/commitDomEdit 同一会话纪律：字节往返提交，真实编解码器
/// 单一通道）。树投影由管线在 compileMethods 后统一 syncDomToTree 刷新
/// （postprocessor 基线 + output 投影），backend 内不再触树。
///
/// 树世界「instructions.clear() + maxStack/maxLocals=0」的 DOM 对应物 =
/// 摘除 Code 属性槽（JVMS：native 方法不带 Code 属性）；clinit 前缀注入
/// 对应 promote + insertBefore(first)（同锚点保序）或 addMethod + append。

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/dom_edit.hpp"
#include "mj2x/core/jvm_opcodes.hpp"

namespace mj2x {
namespace dom_inject {

/// @brief name+desc（类内唯一）锚定 MemberInfo 下标；未命中返回 methods.size()。
inline ca::usize findMethodInDom(const jbc::core::ClassFile& dom, const std::string& name,
                                 const std::string& desc)
{
    for (ca::usize i = 0; i < dom.methods.size(); ++i) {
        if (dom.cp.utf8Equals(dom.methods[i].nameCp, name)
            && dom.cp.utf8Equals(dom.methods[i].descCp, desc)) {
            return i;
        }
    }
    return dom.methods.size();
}

/// @brief 标记方法为 native 并摘除 Code 槽（「清指令」的 DOM 对应物）。
///
/// 幂等：已无 Code 槽（如同类第二后端重复注入）时只重标 access。
/// cls.modified 置位语义与树版一致（output 侧 COMPUTE/重写判定）。
inline void markMethodNative(CompilerClass& cls, const std::string& className,
                             const std::string& name, const std::string& desc)
{
    auto session = openDomEdit(cls, className);
    auto& dom    = session->backing();
    const auto mi = findMethodInDom(dom, name, desc);
    if (mi == dom.methods.size()) {
        throw std::runtime_error("dom_inject: method not in DOM: " + className + "." + name);
    }
    auto& m = dom.methods[mi];
    m.access |= Opcodes::ACC_NATIVE;
    for (ca::usize i = 0; i < m.attrs.size(); ++i) {
        if (dom.cp.utf8Equals(m.attrs[i].nameCp, "Code")) {
            m.attrs.erase(m.attrs.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    cls.modified = true;
    commitDomEdit(cls, className, session);
}

/// @brief clinit 头部插入 registerNativesForClass(classId, thisClass)（regc/vmc4）。
///
/// 无 clinit 则按种子建（ACC_STATIC + RETURN 骨架）。前缀三条对 first 同锚点
/// insertBefore 正序插入（同锚保序，批 4b 实证 14 的对偶：insertAtStart 才
/// 需要逆序）。
inline void prependClinitRegisterCall(CompilerClass& cls, const std::string& className,
                                      ca::i32 classId, const char* bridgeClass,
                                      const char* bridgeName, const char* bridgeDesc)
{
    constexpr ca::u16 kClinitMaxStack = 2;   // ldc int + ldc class 各占 1 slot

    auto session  = openDomEdit(cls, className);
    auto& dom     = session->backing();
    auto  mi      = findMethodInDom(dom, "<clinit>", "()V");
    if (mi == dom.methods.size()) {
        auto added = session->addMethod(jbc::ir::MethodSeed{
            "<clinit>", "()V", static_cast<ca::u32>(Opcodes::ACC_STATIC), true,
            kClinitMaxStack, 0});
        if (added.is_err()) {
            throw std::runtime_error("dom_inject: addMethod <clinit> failed for " + className);
        }
        mi = dom.methods.size() - 1;
    }

    auto promoted = jbc::ir::promoteMethod(*session, mi);
    if (promoted.is_err()) {
        throw std::runtime_error("dom_inject: promote <clinit> failed for " + className);
    }
    auto unit = std::move(promoted).unwrap();

    // 空体（新建种子或既有无体 clinit）先补 RETURN 作锚；insertBefore(RETURN)
    // 正序插前缀三条，最终序 = [ldc id, ldc class, invokestatic, RETURN...]。
    if (unit->dom.size() == 0) {
        jbc::ir::CodeEditor editor(*unit);
        auto added = editor.append(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        if (added.is_err()) {
            throw std::runtime_error("dom_inject: append RETURN failed for " + className);
        }
    }
    {
        jbc::ir::CodeEditor editor(*unit);
        const auto          anchor = unit->dom.first();
        std::vector<jbc::ir::Insn> prefix;
        prefix.push_back(jbc::ir::CodeEditor::loadConst(session->internInteger(classId)));
        // loadConst（索引 > 0xFF 自动升 LDC_W）：cpInsn 硬编码 LDC 在大常量池类
        // 索引超 u8，导出 bad instructions（IntrinsicSurface/StringsAndBoxing 首曝）。
        prefix.push_back(
            jbc::ir::CodeEditor::loadConst(session->internClass(dom.thisName())));
        prefix.push_back(jbc::ir::CodeEditor::cpInsn(
            jbc::core::Op::INVOKESTATIC,
            session->internMethodRef(bridgeClass, bridgeName, bridgeDesc)));
        for (auto& in : prefix) {
            (void)editor.insertBefore(anchor, std::move(in));
        }
    }
    if (unit->maxStack < kClinitMaxStack) unit->maxStack = kClinitMaxStack;

    auto lowered = jbc::ir::lowerMethod(*session, mi);
    if (lowered.is_err()) {
        throw std::runtime_error(std::string("dom_inject: lower <clinit> failed for ")
                                 + className + ": " + jbc::ir::to_string(std::move(lowered).unwrap_err()));
    }
    cls.modified = true;
    commitDomEdit(cls, className, session);
}

/// @brief 全量重建方法体（stackc 的 clinit 重写 / interface-default proxy 改写）。
///
/// 无该方法则按种子建；有则 promote 后逐条 erase（含异常表清空——树版
/// clearMethodDebugAndHandlers 的 DOM 对应物）再经 emit 追加新体。emit 在
/// 会话内执行：CP 操作数（bridge 调用等）经 session intern 后 append，保证
/// 索引归属本次提交。accessAndMask 非 0 时 access &= mask（clinit/proxy
/// 场景清 ACC_NATIVE）。
inline void replaceMethodBody(
    CompilerClass& cls, const std::string& className, const std::string& name,
    const std::string& desc,
    const std::function<void(jbc::ir::ClassSession&, jbc::ir::CodeEditor&)>& emit,
    ca::u16 maxStack, ca::u16 maxLocals, ca::u32 accessAndMask = 0)
{
    auto session = openDomEdit(cls, className);
    auto& dom    = session->backing();
    auto  mi     = findMethodInDom(dom, name, desc);
    if (mi == dom.methods.size()) {
        auto added = session->addMethod(jbc::ir::MethodSeed{
            name, desc, static_cast<ca::u32>(Opcodes::ACC_STATIC), true, maxStack, maxLocals});
        if (added.is_err()) {
            throw std::runtime_error("dom_inject: addMethod " + name + " failed for " + className);
        }
        mi = dom.methods.size() - 1;
    }
    else if (accessAndMask != 0) {
        dom.methods[mi].access &= static_cast<ca::u16>(accessAndMask);
    }

    auto promoted = jbc::ir::promoteMethod(*session, mi);
    if (promoted.is_err()) {
        throw std::runtime_error("dom_inject: promote " + name + " failed for " + className);
    }
    auto unit = std::move(promoted).unwrap();

    // 全量重建：逐条 erase 首条直至清空（句柄域稳定）；异常表/调试属性引用
    // 旧布局，随体丢弃。erase 不清除标签槽（导入期 ControlFlow 标签留槽失锚），
    // 残留孤儿标签会让后续 lower 按「目标未锚定」拒绝——IntrinsicSurface 首
    // 曝（clinit 含 LVT 搬运被弃，原体带分支标签，重建后 lower bad
    // instructions）——擦除前收集全部引用标签，清空后统一销毁。
    {
        std::vector<jbc::ir::LabelId> doomed;
        for (auto id2 : unit->dom.order()) {
            const auto* din = unit->dom.peek(id2);
            if (din == nullptr) continue;
            if (din->target != jbc::ir::kNoLabel) doomed.push_back(din->target);
            for (auto t : din->targets) doomed.push_back(t);
        }
        for (const auto& e : unit->exc) {
            doomed.push_back(e.start);
            doomed.push_back(e.end);
            doomed.push_back(e.handler);
        }
        jbc::ir::CodeEditor editor(*unit);
        while (unit->dom.size() > 0) {
            auto erased = editor.erase(unit->dom.first());
            if (erased.is_err()) {
                throw std::runtime_error("dom_inject: erase body failed for " + className + "."
                                         + name);
            }
        }
        unit->exc.clear();
        for (auto lbl : doomed) (void)unit->dom.destroyLabel(lbl);
        // 嵌套调试属性（LNT/LVT/SMT）引用旧布局：随体丢弃——lower 的嵌套
        // 属性重定位按旧锚执行，体已全换则无锚可依。
        for (auto& slot : session->backing().methods[mi].attrs) {
            if (!session->backing().cp.utf8Equals(slot.nameCp, "Code")) continue;
            auto* body = slot.mutate();
            if (body != nullptr) static_cast<jbc::core::CodeAttr&>(*body).attrs.clear();
            break;
        }
    }
    {
        jbc::ir::CodeEditor editor(*unit);
        emit(*session, editor);
    }
    unit->maxStack  = maxStack;
    unit->maxLocals = maxLocals;

    if (unit->dom.size() > 0) {
        auto lowered = jbc::ir::lowerMethod(*session, mi);
        if (lowered.is_err()) {
            throw std::runtime_error(std::string("dom_inject: lower ") + name + " failed for "
                                     + className + ": "
                                     + jbc::ir::to_string(std::move(lowered).unwrap_err()));
        }
    }
    cls.modified = true;
    commitDomEdit(cls, className, session);
}

}   // namespace dom_inject
}   // namespace mj2x

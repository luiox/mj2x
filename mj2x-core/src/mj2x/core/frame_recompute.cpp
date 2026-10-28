#include "mj2x/core/frame_recompute.hpp"

#include <stdexcept>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mj2x/core/jvm_opcodes.hpp"

namespace mj2x {

using jbc::analysis::FrameAt;
using jbc::analysis::FrameEngine;
using jbc::analysis::FramePolicy;
using jbc::analysis::MethodSig;
using jbc::analysis::SmtBuildError;
using jbc::analysis::VKind;
using jbc::analysis::VType;

namespace {

constexpr std::string_view kCodeAttrName = "Code";
constexpr std::string_view kSmtAttrName  = "StackMapTable";

}   // namespace

ca::core::Result<VType, jbc::analysis::TypeMergeError> PortedCommonSuperResolver::merge(
    VType a, VType b)
{
    using jbc::analysis::TypeMergeError;
    if (a.k != VKind::Ref || b.k != VKind::Ref) {
        return ca::core::Err(TypeMergeError::Unresolvable);
    }
    if (a.name == b.name) return ca::core::Ok(a);
    if (isAssignableTo(a.name, b.name)) return ca::core::Ok(a);
    if (isAssignableTo(b.name, a.name)) return ca::core::Ok(b);
    for (std::string_view parent = directSuperOf(a.name); !parent.empty();
         parent                  = directSuperOf(parent)) {
        if (isAssignableTo(parent, b.name)) {
            return ca::core::Ok(VType::ref(refForName(parent)));
        }
    }
    return ca::core::Ok(VType::ref("java/lang/Object"));
}

std::string_view PortedCommonSuperResolver::directSuperOf(std::string_view type)
{
    if (type == "java/lang/Object") return {};
    if (type == "java/lang/Number") return "java/lang/Object";
    if (type == "java/lang/Byte" || type == "java/lang/Short"
        || type == "java/lang/Integer" || type == "java/lang/Long"
        || type == "java/lang/Float" || type == "java/lang/Double") {
        return "java/lang/Number";
    }
    if (type == "java/lang/Boolean" || type == "java/lang/Character"
        || type == "java/lang/String") {
        return "java/lang/Object";
    }
    if (type == "java/lang/Throwable") return "java/lang/Object";
    if (type == "java/lang/Exception" || type == "java/lang/Error") {
        return "java/lang/Throwable";
    }
    if (type == "java/lang/RuntimeException") return "java/lang/Exception";
    if (type == "java/lang/IndexOutOfBoundsException") {
        return "java/lang/RuntimeException";
    }
    if (type == "java/lang/AssertionError") return "java/lang/Error";
    return "java/lang/Object";
}

bool PortedCommonSuperResolver::isAssignableTo(std::string_view target,
                                               std::string_view source)
{
    for (std::string_view t = source; !t.empty(); t = directSuperOf(t)) {
        if (t == target) return true;
    }
    return false;
}

std::string_view PortedCommonSuperResolver::refForName(std::string_view type)
{
    if (type == "java/lang/Number" || type == "java/lang/Throwable"
        || type == "java/lang/Exception" || type == "java/lang/Error"
        || type == "java/lang/RuntimeException") {
        return type;
    }
    return "java/lang/Object";
}

void recomputeSessionFrames(const std::string& name, jbc::ir::ClassSession& session)
{
    auto&                     dom      = session.backing();
    PortedCommonSuperResolver resolver;
    FrameEngine               engine(dom.cp, resolver, FramePolicy::Strict);

    for (ca::usize i = 0; i < dom.methods.size(); ++i) {
        auto promoted = jbc::ir::promoteMethod(session, i);
        if (promoted.is_err()) {
            continue;   // 无 Code 槽 = abstract/native：无帧可算
        }
        auto unit = std::move(promoted).unwrap();

        auto&       m      = dom.methods[i];
        const bool  isStat = (m.access & Opcodes::ACC_STATIC) != 0;
        MethodSig   sig;
        sig.isStatic  = isStat;
        sig.desc      = dom.cp.utf8Of(m.descCp);
        sig.name      = dom.cp.utf8Of(m.nameCp);
        sig.thisClass = isStat ? std::string_view{} : dom.thisName();

        std::vector<FrameAt> frames;
        auto                 summary = engine.compute(*unit, sig, frames);
        if (!summary.ok) {
            throw std::runtime_error("frame_recompute: compute failed for " + name + "."
                                     + std::string(sig.name) + ": "
                                     + jbc::analysis::to_string(summary.fail));
        }
        jbc::analysis::applyComputedMaxes(*unit, summary, sig);

        auto lowered = jbc::ir::lowerMethod(session, i);
        if (lowered.is_err()) {
            throw std::runtime_error("frame_recompute: maxes lower failed for " + name
                                     + "." + std::string(sig.name) + ": "
                                     + jbc::ir::to_string(std::move(lowered).unwrap_err()));
        }

        if (dom.major < 50) continue;   // SMT 自 v50 才有定义；旧版本类不动

        auto smt = jbc::analysis::buildStackMapTable(session, *unit, frames);
        if (smt.is_err()) {
            const auto e = std::move(smt).unwrap_err();
            if (e == SmtBuildError::ClassTooOld) continue;
            throw std::runtime_error("frame_recompute: smt build failed for " + name + "."
                                     + std::string(sig.name) + ": "
                                     + jbc::analysis::to_string(e));
        }
        auto result = std::move(smt).unwrap();

        // SMT 槽换装：Code 槽已被 promote/lower 解码，mutate() 置脏后改嵌套
        // 属性表——needed=false 不留空表（对齐旧 writer），已有槽删除。
        ca::usize codeIdx = m.attrs.size();
        for (ca::usize k = 0; k < m.attrs.size(); ++k) {
            if (dom.cp.utf8Equals(m.attrs[k].nameCp, kCodeAttrName)) {
                codeIdx = k;
                break;
            }
        }
        if (codeIdx == m.attrs.size()) continue;
        auto* codeBody = m.attrs[codeIdx].mutate();
        if (codeBody == nullptr) continue;
        auto& nested = static_cast<jbc::core::CodeAttr&>(*codeBody).attrs;

        ca::usize smtIdx = nested.size();
        for (ca::usize k = 0; k < nested.size(); ++k) {
            if (dom.cp.utf8Equals(nested[k].nameCp, kSmtAttrName)) {
                smtIdx = k;
                break;
            }
        }
        if (result.needed) {
            const ca::u32 nameCp = smtIdx < nested.size()
                                       ? nested[smtIdx].nameCp
                                       : dom.cp.addUtf8(kSmtAttrName);
            auto slot = jbc::core::AttrSlot::makeNew(nameCp, std::move(result.attr));
            if (smtIdx < nested.size()) {
                nested[smtIdx] = std::move(slot);
            }
            else {
                nested.push_back(std::move(slot));
            }
        }
        else if (smtIdx < nested.size()) {
            nested.erase(nested.begin() + smtIdx);
        }
    }
}

}   // namespace mj2x

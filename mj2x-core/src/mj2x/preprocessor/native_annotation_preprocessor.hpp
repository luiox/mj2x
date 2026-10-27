#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <string>
#include <utility>
#include <vector>
#include "libca/log/log_macros.hpp"

#include "mj2x/core/dom_edit.hpp"

namespace mj2x {

class NativeAnnotationPreprocessor : public Preprocessor
{
public:
    explicit NativeAnnotationPreprocessor(std::string descriptor)
        : descriptor_(std::move(descriptor))
    {}

    void process(CompilerIndex& index) override
    {
        int marked = 0;
        for (auto& [name, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            // 类级 glob 过滤器剔除的类不标记（--native-include/--native-exclude）。
            if (cls.filteredOut) continue;
            // 接口整体跳过：接口方法禁止 ACC_NATIVE（JLS 9.4/JVMS 4.6），
            // 标记接口的 default/static/私有方法会产出 ClassFormatError。
            // 批 4：读侧落 DOM（access 位是 ClassFile 头字段）。
            if (cls.dom && (cls.dom->access & Opcodes::ACC_INTERFACE))
                continue;
            for (auto& method : cls.methods) {
                auto&       dm = domMethodFor(cls, method, name);
                if (dm.access &
                    (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE))
                    continue;

                auto mname = std::string(cls.dom->nameOf(dm));
                if (mname == "<init>" || mname == "<clinit>") continue;

                if (!hasAnnotation(*cls.dom, dm)) continue;

                method.markedForCompile = true;
                marked++;
            }
        }
        CA_LOGT_INFO("preprocess", "Marked {} method(s) with {}\n", marked, descriptor_.c_str());
    }

private:
    /// @brief 方法上是否挂有目标描述符注解（批 4：DOM 属性解码面）。
    ///
    /// Runtime{Visible,Invisible}Annotations 共用 AnnotationsAttr 模型
    /// （属性名只差可见性），遍历槽逐个解码比对 type 描述符。
    bool hasAnnotation(jbc::core::ClassFile& dom, jbc::core::MemberInfo& m) const
    {
        return hasAnnotation(dom, m, "RuntimeVisibleAnnotations") ||
               hasAnnotation(dom, m, "RuntimeInvisibleAnnotations");
    }

    bool hasAnnotation(jbc::core::ClassFile& dom, jbc::core::MemberInfo& m,
                       std::string_view attrName) const
    {
        for (auto& slot : m.attrs) {
            if (dom.cp.utf8Of(slot.nameCp) != attrName) continue;
            auto decoded = dom.decodeAttr(slot);
            if (decoded.is_err()) {
                throw std::runtime_error("native_annotation: annotation attr decode failed");
            }
            auto* body = std::move(decoded).unwrap();
            if (body->kind() != jbc::core::AttrKind::Annotations) continue;
            auto* annos = static_cast<jbc::core::AnnotationsAttr*>(body);
            for (const auto& anno : annos->annos) {
                if (dom.cp.utf8Of(anno.typeCp) == descriptor_) return true;
            }
        }
        return false;
    }

    std::string descriptor_;
};

}   // namespace mj2x

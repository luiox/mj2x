#pragma once

#include "mj2x/core/dom_edit.hpp"
#include "mj2x/preprocessor/preprocessor.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x {

class MarkAllMethodsPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override
    {
        for (auto& [name, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            // 类级 glob 过滤器剔除的类不标记（--native-include/--native-exclude）。
            if (cls.filteredOut) continue;
            // 接口整体跳过：JLS 9.4/JVMS 4.6 禁止接口方法带 ACC_NATIVE，default/
            // static/私有接口方法（有 Code、非 abstract）一旦标记会产出
            // ClassFormatError 的接口类文件。接口的具体方法保持字节码运行。
            // 批 4：读侧落 DOM（access 位是 ClassFile 头字段）。
            if (cls.dom && (cls.dom->access & Opcodes::ACC_INTERFACE)) continue;
            for (auto& method : cls.methods) {
                // Skip abstract and native methods
                const auto& dm = domMethodFor(cls, method.name, method.desc);
                if (dm.access &
                    (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE))
                    continue;
                // 跳过 <init>/<clinit>：JVM 校验对构造器/类初始化器的 ACC_NATIVE 修饰
                // 有限制（部分实现拒收 native <init>）。构造器/类初始化逻辑由
                // InitClinitMoverPreprocessor 提前搬入 __regc_* 静态方法（见
                // regc_conversion_decisions.md §7.1），此处骨架保留原 Java 字节码。
                auto mname = std::string(cls.dom->nameOf(dm));
                if (mname == "<init>" || mname == "<clinit>") continue;
                method.markedForCompile = true;
            }
        }
        CA_LOGT_INFO("preprocess", "All methods marked for compilation\n");
    }
};

}   // namespace mj2x

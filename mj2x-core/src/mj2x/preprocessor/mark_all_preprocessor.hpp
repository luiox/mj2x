#pragma once

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
            for (auto& method : cls.methods) {
                // Skip abstract and native methods
                auto* mn = method.methodNode;
                if (mn->access &
                    (jbytecode::core::Opcodes::ACC_ABSTRACT | jbytecode::core::Opcodes::ACC_NATIVE))
                    continue;
                // 跳过 <init>/<clinit>：JVM 校验对构造器/类初始化器的 ACC_NATIVE 修饰
                // 有限制（部分实现拒收 native <init>）。构造器/类初始化逻辑由
                // InitClinitMoverPreprocessor 提前搬入 __regc_* 静态方法（见
                // regc_conversion_decisions.md §7.1），此处骨架保留原 Java 字节码。
                auto mname = mn->name.to_std_string();
                if (mname == "<init>" || mname == "<clinit>") continue;
                method.markedForCompile = true;
            }
        }
        CA_LOGT_INFO("preprocess", "All methods marked for compilation\n");
    }
};

}   // namespace mj2x

#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

namespace mj2x {

class MarkAllMethodsPreprocessor : public Preprocessor {
public:
    void process(CompilerIndex& index) override {
        for (auto& [name, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            for (auto& method : cls.methods) {
                // Skip abstract and native methods
                auto* mn = method.methodNode;
                if (mn->access & (jbytecode::core::Opcodes::ACC_ABSTRACT |
                                  jbytecode::core::Opcodes::ACC_NATIVE))
                    continue;
                // 跳过 <init>/<clinit>：JVM 校验对构造器/类初始化器的 ACC_NATIVE 修饰
                // 有限制（部分实现拒收 native <init>），且 StackC 当前不对构造器做
                // 完整支持。这些方法保留原 Java 字节码，由 JVM 直接执行。
                auto mname = mn->name.to_std_string();
                if (mname == "<init>" || mname == "<clinit>")
                    continue;
                method.markedForCompile = true;
            }
        }
        printf("[MarkAll] All methods marked for compilation\n");
    }
};

} // namespace mj2x


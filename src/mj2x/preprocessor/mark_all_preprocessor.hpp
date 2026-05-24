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
                if (mn->access & (jbytecode::Opcodes::ACC_ABSTRACT |
                                  jbytecode::Opcodes::ACC_NATIVE))
                    continue;
                method.markedForCompile = true;
            }
        }
        printf("[MarkAll] All methods marked for compilation\n");
    }
};

} // namespace mj2x

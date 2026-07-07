#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mj2x {

class NativeAnnotationPreprocessor : public Preprocessor {
public:
    explicit NativeAnnotationPreprocessor(std::string descriptor)
        : descriptor_(std::move(descriptor))
    {}

    void process(CompilerIndex& index) override {
        int marked = 0;
        for (auto& [name, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            for (auto& method : cls.methods) {
                auto* mn = method.methodNode;
                if (mn->access & (jbytecode::core::Opcodes::ACC_ABSTRACT |
                                  jbytecode::core::Opcodes::ACC_NATIVE))
                    continue;

                auto mname = mn->name.to_std_string();
                if (mname == "<init>" || mname == "<clinit>")
                    continue;

                if (!hasAnnotation(*mn))
                    continue;

                method.markedForCompile = true;
                marked++;
            }
        }
        printf("[NativeAnnotation] Marked %d method(s) with %s\n",
               marked, descriptor_.c_str());
    }

private:
    bool hasAnnotation(const jbytecode::tree::MethodNode& method) const {
        return hasAnnotation(method.visibleAnnotations) ||
               hasAnnotation(method.invisibleAnnotations);
    }

    bool hasAnnotation(
        const std::vector<jbytecode::tree::AnnotationNode*>& annotations) const {
        for (auto* annotation : annotations) {
            if (annotation != nullptr &&
                annotation->desc.to_std_string() == descriptor_) {
                return true;
            }
        }
        return false;
    }

    std::string descriptor_;
};

} // namespace mj2x

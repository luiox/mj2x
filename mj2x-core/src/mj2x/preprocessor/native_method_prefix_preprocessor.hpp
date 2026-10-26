#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <string>
#include <utility>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace mj2x {

class NativeMethodPrefixPreprocessor : public Preprocessor
{
public:
    explicit NativeMethodPrefixPreprocessor(std::vector<std::string> prefixes)
        : prefixes_(std::move(prefixes))
    {}

    void process(CompilerIndex& index) override
    {
        int marked = 0;
        for (auto& [className, cls] : index.getClasses()) {
            if (cls.isLibrary) continue;
            // 类级 glob 过滤器剔除的类不标记（--native-include/--native-exclude）。
            if (cls.filteredOut) continue;
            // 接口整体跳过：接口方法禁止 ACC_NATIVE（JLS 9.4/JVMS 4.6），
            // 标记接口的 default/static/私有方法会产出 ClassFormatError。
            if (cls.classNode &&
                (cls.classNode->access & jbytecode::core::Opcodes::ACC_INTERFACE))
                continue;
            for (auto& method : cls.methods) {
                auto* mn = method.methodNode;
                if (mn->access &
                    (jbytecode::core::Opcodes::ACC_ABSTRACT | jbytecode::core::Opcodes::ACC_NATIVE))
                    continue;

                const auto methodName = mn->name.to_std_string();
                if (methodName == "<init>" || methodName == "<clinit>") continue;

                const std::string key = className + "." + methodName;
                if (!matches(key)) continue;

                method.markedForCompile = true;
                marked++;
            }
        }
        CA_LOGT_INFO("preprocess", "Marked {} method(s)\n", marked);
    }

private:
    bool matches(const std::string& key) const
    {
        for (const auto& prefix : prefixes_) {
            if (key.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    std::vector<std::string> prefixes_;
};

}   // namespace mj2x

// mj2x InvokeDynamicPreprocessor — 诊断 invokedynamic 站点。
//
// StackC 在 MIR 层保留 bootstrap 元数据，并在 native 侧通过 bootstrap +
// MethodHandle.invokeWithArguments 执行。RegC 仍需要后续字节码/IR 展开，所以本
// preprocessor 暂不改写指令，只负责给出显式诊断，避免静默忽略。
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

class InvokeDynamicPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override;
};

}   // namespace mj2x

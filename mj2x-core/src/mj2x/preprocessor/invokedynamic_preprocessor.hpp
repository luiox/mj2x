// mj2x InvokeDynamicPreprocessor — invokedynamic 站点处理。
//
// StackC 在 MIR 层保留 bootstrap 元数据，并在 native 侧通过 bootstrap +
// MethodHandle.invokeWithArguments 执行。RegC 是无栈 IR，不走 runtime bridge，
// 而是在此 preprocessor 把可静态展开的 indy 改写成等价字节码（R6.1）。
//
// 现阶段（R6.1 阶段1）：展开 java/lang/invoke/StringConcatFactory 的
// makeConcatWithConstants / makeConcat —— 改写成 new StringBuilder + append 链 +
// toString，RegC 已有 StringBuilder intrinsic 支持，无需任何 native 新代码，也绕开
// RegC 缺 owner-class 的障碍。其余 indy（lambda 等）保持 fail-closed 诊断，交由
// 后续阶段（lambda expansion / runtime bridge）处理。
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

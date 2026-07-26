// mj2x InvokeDynamicPreprocessor — invokedynamic 站点处理。
//
// StackC 在 MIR 层保留 bootstrap 元数据，并在 native 侧通过 bootstrap +
// MethodHandle.invokeWithArguments 执行。RegC 是无栈 IR，不走 runtime bridge，
// 而是在此 preprocessor 把可静态展开的 indy 改写成等价字节码（R6.1）。
//
// 现阶段（R6.1 阶段1/2/2b/2c/2d）已展开：
//   - java/lang/invoke/StringConcatFactory 的 makeConcatWithConstants / makeConcat
//     —— 改写成 new StringBuilder + append 链 + toString（阶段1）；
//   - java/lang/invoke/LambdaMetafactory.metafactory —— 为每个站点合成实现
//     functional interface 的类：无状态 + 捕获 lambda（阶段2/2b），实例
//     （H_INVOKEVIRTUAL/H_INVOKEINTERFACE/H_INVOKESPECIAL）与构造器
//     （H_NEWINVOKESPECIAL）方法引用（阶段2c），以及擦除泛型的装箱/桥接形态
//     （阶段2d：CHECKCAST 收窄 / 拆装箱 / POP 丢弃返回值）。
// 展开后走 RegC 通用字节码路径转 native，绕开 RegC 缺 owner-class 的障碍。
// 仍 fail-closed 诊断的形态：基本类型互转桥接（i2l 加宽等）、altMetafactory、
// 自定义 bootstrap（交由后续阶段处理）。
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

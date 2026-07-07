// mj2x InvokeDynamicPreprocessor — 展开 invokedynamic 为 bootstrap + MethodHandle.invokeWithArguments。
//
// 详见 doc/PREPROCESSOR.md §4。R1b 只做 STD_JAVA 路径（不做 HOTSPOT linkCallSite）。
// 展开策略（简化版，覆盖常见 lambda / StringConcat）：
//   1. 装箱 dynamic-call 实参为 Object[]
//   2. 加载 BSM 参数（bsmArgs），递归装箱
//   3. INVOKESTATIC bsm(owner, name, type, ...bsmArgs) → CallSite.getTarget() → MethodHandle
//   4. 把 receiver + 已装箱实参作为 Object[] 传给 MethodHandle.invokeWithArguments
//   5. unbox / CHECKCAST 返回类型
//   包 try-catch(Throwable) → 包装成 BootstrapMethodError
//
// 注：完整实现涉及大量装箱字节码生成。R1b 提供骨架 + fail-closed：
//   若遇到不支持的结构（如复杂 bsmArgs），保留原 invokedynamic 并标记方法 fail-closed，
//   L1 翻译期再拒绝（不静默）。骨架覆盖最常见路径（bsmArgs 全是 String/Type/Handle/int）。
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

class InvokeDynamicPreprocessor : public Preprocessor {
public:
    void process(CompilerIndex& index) override;
};

} // namespace mj2x

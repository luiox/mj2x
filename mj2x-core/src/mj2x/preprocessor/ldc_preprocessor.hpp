// mj2x LdcPreprocessor — 展开 LDC Handle / LDC MethodType 为 MethodHandles.Lookup 反射调用链。
//
// 详见 doc/PREPROCESSOR.md §3。Lookup 取 MethodHandles.lookup()（调用者即本类，
// 访问权限与原 ldc 常量一致）；MethodType 构造走 fromMethodDescriptorString(desc,
// caller.getClassLoader())（系统 ClassLoader 解析不了保护态 jar 的应用类引用）。
//
// 处理：
//   ldc Handle  → Lookup.findVirtual/findStatic/findSpecial/findConstructor/
//                 findGetter/findSetter 系（按 tag 分派，压栈顺序对齐 findXxx 描述符，
//                 栈形校验见 unittest ldc_preprocessor_test）
//   ldc MethodType → MethodType.fromMethodDescriptorString(desc, callerClassLoader)
//   其它 ldc（int/float/String/Class）→ 不动（L1 能直译）
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

class LdcPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override;
};

}   // namespace mj2x

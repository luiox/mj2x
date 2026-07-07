// mj2x LdcPreprocessor — 展开 LDC Handle / LDC MethodType 为 MethodHandles.Lookup 反射调用链。
//
// 详见 doc/PREPROCESSOR.md §3。R1b 简化：直接用 MethodHandles.lookup() 和
// getClass().getClassLoader()（不注入 magic 标记，magic 化留 R4）。
//
// 处理：
//   ldc Handle  → Lookup.findVirtual/findStatic/findGetter/...（按 tag 分派）
//   ldc MethodType → MethodType.fromMethodDescriptorString(desc, classloader)
//   其它 ldc（int/float/String/Class）→ 不动（L1 能直译）
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

class LdcPreprocessor : public Preprocessor {
public:
    void process(CompilerIndex& index) override;
};

} // namespace mj2x

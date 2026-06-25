// mj2x MultiANewArrayPreprocessor — 多维数组指令的预处理。
//
// 详见 doc/PREPROCESSOR.md §5。R1b 采用方案 A：不改写字节码，但明确标记 multianewarray
// 为已知指令（消除"静默落 MIRNop"）。真正的字节码层循环展开（方案 B）推到 R2+。
//
// CodeConverter 现在会把 multianewarray 翻成 MIRNewArray（dims>1），builder 翻译 MIRNewArray
// 时若 dims>1 fail-closed（R2 加 MULTIANEWARRAY runtime helper）。
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

#include <jbytecode/jbytecode.hpp>

namespace mj2x {

class MultiANewArrayPreprocessor : public Preprocessor {
public:
    void process(CompilerIndex& index) override;
};

} // namespace mj2x

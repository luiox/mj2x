// mj2x MultiANewArrayPreprocessor — 多维数组指令的预处理。
//
// 详见 mj2x/doc_ai/preprocessor.md §5。当前实现不改写字节码，只统计 multianewarray
// 站点并提示后续由 L1/SSA/RegC helper 路径处理。这样保留原始 bytecode 形状，
// 同时避免早期版本那种静默落 MIRNop 的风险。
#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"


namespace mj2x {

class MultiANewArrayPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override;
};

}   // namespace mj2x

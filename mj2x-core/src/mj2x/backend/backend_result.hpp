#pragma once

#include <string>
#include <vector>

namespace mj2x {

struct BackendResult
{
    enum class Kind
    {
        NONE,
        C_SOURCE,   ///< C 系后端（StackC/RegC/VMC4）的生成源
        LLVM_IR     ///< LLVM 后端的 textual IR（.ll；此前语义错位挂 C_SOURCE，dev_plan G 项收口）
    };
    Kind        kind = Kind::NONE;
    std::string text;
};

}   // namespace mj2x

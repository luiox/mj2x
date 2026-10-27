#pragma once

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

class Preprocessor
{
public:
    virtual ~Preprocessor()                    = default;
    virtual void process(CompilerIndex& index) = 0;

    /// @brief 改写载体声明（批 4）：true = 直接改 DOM（ClassSession 载体）；
    /// false = 仍改树（批 6 前收尾的 pass，如 invokedynamic）。pipeline 按
    /// 此拆段：树载体 pass 先跑并经 syncTreeToDom 落 DOM，DOM 载体 pass
    /// 在 DOM 权威上改写。
    virtual bool editsDom() const noexcept { return true; }
};

}   // namespace mj2x

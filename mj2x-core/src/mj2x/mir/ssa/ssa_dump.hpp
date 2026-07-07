// mj2x SSA dump — 文本 dump，验证用。dump 文本不含 cstack/sp 等 c2j 锚点子串。
#pragma once

#include "mj2x/mir/ssa/ssa_function.hpp"

#include <string>

namespace mj2x {

// dump 整个 SSAFunction。用于人工检查 + R1 验收（grep cstack 应零匹配）。
std::string dumpSSA(const SSAFunction& fn);

} // namespace mj2x

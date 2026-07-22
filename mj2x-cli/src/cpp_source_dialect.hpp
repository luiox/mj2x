/// @file cpp_source_dialect.hpp
/// @brief 将 RegC 的 C99 JNI 调用方言转换为 C++17 JNI 成员调用方言。
#pragma once

#include <string>
#include <string_view>

namespace mj2x::cli {

/// @brief 生成可由 C++17 编译器处理的 JNI source dialect。
/// @details 输入必须使用标准 C JNI 函数表调用形式；不匹配该形式的文本保持原样。
std::string emitCpp17Dialect(std::string_view cSource);

}   // namespace mj2x::cli

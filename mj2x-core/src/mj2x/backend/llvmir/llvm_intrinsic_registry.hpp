/// @file llvm_intrinsic_registry.hpp
/// @brief LLVM IR 后端的 JDK intrinsic 直调注册表（对齐 RegC 纯函数切片）。
#pragma once

#include "libca/str/utf8_string.hpp"

#include <string>

namespace mj2x {

/// @brief 一次 LLVM intrinsic 直调所需信息。
struct LlvmIntrinsicEntry
{
    const char* owner;   // "java/lang/Math"
    const char* name;    // "abs"
    const char* desc;    // "(I)I"
    const char* symbol;  // "mj2x_math_abs_i32"（mj2x-crt mj2x_regc_intrinsic.c 实现）
    const char* returnLlvm;   // "i32"/"i64"/"f32"/"f64"
    const char* argLlvm[2];   // 参数类型（最多 2 个；空串表示无该参数）
    const char* declareText;  // 完整 "declare i32 @sym(i32)" 文本，finalize 直接输出
};

/// @brief 查找 Math 纯函数 intrinsic（与 RegC registry 的 RuntimeFunction +
/// 纯标量条目同源：符号在 mj2x-crt，语义由 JVM 基准测试验证）。
/// @return 命中返回条目，否则 nullptr。
const LlvmIntrinsicEntry* matchLlvmIntrinsic(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc);

}   // namespace mj2x

/// @file regc_runtime_template.hpp
/// @brief RegC 运行时文本注入接口(#894 B 类真下沉后的残量)。
///
/// @details 运行时 helper 主体已物理下沉 mj2x-rt(mj2x_regc_runtime.c/.h),
/// 生成 TU 经链接消费。本文件仅保留逐程序注入文本的渲染入口:
/// - @FORWARD_DECLARATIONS@:fwd 注册表渲染(生成函数前向声明)
/// - @CACHE_DECLARATIONS@:per-callsite 缓存变量定义
/// 角色单例缓存(String/Object.toString/valueOf/parseFloat/异常类等 11 个)
/// 收敛为运行时固定实例,不再逐 TU 注入。
#pragma once

#include "libca/str/utf8_string.hpp"
#include "mj2x/codegen/c_source.hpp"

#include <string>

namespace mj2x {

/// @brief 渲染逐程序注入块(fwd 前向声明 + per-callsite 缓存声明)。
void emitRuntimeHelpers(ca::str::Utf8StringBuilder& out, const std::string& cacheDeclarations,
                        codegen::CForwardDeclarations* fwd = nullptr);

}   // namespace mj2x

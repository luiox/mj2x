/// @file stackc_runtime_prelude.hpp
/// @brief Runtime helper C fragments emitted once per StackCBackend TU.
///
/// @details
/// StackCBackend's generated translation unit needs small static JNI helpers
/// that are reused by every compiled method: an owner-loader-aware class
/// resolver (`_mj2x_find_class`), a class-descriptor resolver
/// (`_mj2x_component_class`), a multi-array allocator
/// (`_mj2x_new_multi_array`), plus cache/分支预测平台宏（无锁 CAS 对齐
/// JNIC 3.7.0）。local-ref 跟踪已移除：对齐 JNIC 裸 JNI 模型，方法返回时
/// JVM 自动释放 local refs。这些 fragment 原先是 `StackCBackend::finalize`
/// 里的字符串字面量，现收口到这里，finalize 只需前置拼装。
#pragma once

#include <string>

namespace mj2x::backend {

/// @brief Render the runtime helper functions as C source text.
/// @details The returned string contains complete `static` function
///          definitions (no trailing separation); callers append it after the
///          `#include` block and before the per-method bodies.
std::string stackCRuntimePrelude();

}   // namespace mj2x::backend

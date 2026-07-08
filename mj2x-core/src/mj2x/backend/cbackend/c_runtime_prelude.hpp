/**
 * @file c_runtime_prelude.hpp
 * @brief Runtime helper C fragments emitted once per CBackend TU.
 *
 * @details
 * CBackend's generated translation unit needs small static JNI helpers
 * that are reused by every compiled method: an owner-loader-aware class
 * resolver (`_mj2x_find_class`), a class-descriptor resolver
 * (`_mj2x_component_class`), and a multi-array allocator
 * (`_mj2x_new_multi_array`).  These used to be inlined as string literals
 * inside `CBackend::finalize`, which made that method long and the helpers
 * impossible to unit-test in isolation.  They now live here as a single
 * fragment the finalize step prepends.
 */
#pragma once

#include <string>

namespace mj2x::backend {

/// @brief Render the runtime helper functions as C source text.
/// @details The returned string contains complete `static` function
///          definitions (no trailing separation); callers append it after the
///          `#include` block and before the per-method bodies.
std::string cBackendRuntimePrelude();

}  // namespace mj2x::backend

/// @file logging.hpp
/// @brief mj2x 全局日志初始化（libca log 门面 + SimpleLogBackend）。
#pragma once

#include "libca/log/level.hpp"

namespace mj2x {

/// @brief 初始化 mj2x 全局日志。
///
/// @details
/// 基于 libca log 门面：按模块 target 注册 Logger（全部共享同一个
/// SimpleLogBackend，同步直写 stderr，彩色 + 时间/[LEVEL]/[target] +
/// file:line）。业务代码用 CA_LOG_* / CA_LOGT_* 宏输出，无需感知后端。
///
/// 注册的 target：default、cli、compile、pipeline、regc、stackc、vmc4、
/// llvm、preprocess、io。未初始化时宏调用被门面静默丢弃（get 返回
/// nullptr），库使用者可自行调用本函数或注册自有 Logger。
///
/// @param level 运行期级别（低于该级别在运行期过滤；编译期裁剪由
///              CA_COMPILE_LOG_LEVEL 控制，见 libca/log/log_macros.hpp）。
/// @param color 是否终端着色（SimpleLogConfig.color）。
void initLogging(ca::log::Level level = ca::log::Level::Info, bool color = true);

/// @brief 统一调整所有已注册 target 的运行期级别（如 CLI --quiet/--verbose）。
void setLogLevel(ca::log::Level level);

}   // namespace mj2x

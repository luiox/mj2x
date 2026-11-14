/// @file mj2x_regc_sys.h
/// @brief System/Objects 类 RegC intrinsic 的 CRT 侧声明。
///
/// @details
/// 实现 mj2x_regc_sys.c（mj2x-rt 编译 + 生成工程整目录分发，新文件经
/// copyRtTree/xmake glob 自动带上）。生成 TU 侧的调用声明不经本头：发射侧
/// RegCIntrinsicRegistry::emitDeclarations 是生成 TU 唯一声明源（与
/// floorDiv/hex-string 同款约定），本头供 rt 侧测试与后续内部消费。
#pragma once

#include "regc/mj2x_regc_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief System.currentTimeMillis()：1970 epoch 起的墙钟毫秒。
///
/// @details
/// Windows 用 GetSystemTimeAsFileTime（1601 epoch 的 100ns 单位，减
/// 116444736000000000 × 100ns 的 1601→1970 偏移后除 10^4）；POSIX 用
/// gettimeofday。纯计算不抛异常（RuntimeFunction 发射面）。
int64_t mj2x_intrinsic_system_current_time_millis(void);

/// @brief System.nanoTime()：单调时钟纳秒（仅用于相对间隔测量）。
///
/// @details
/// Windows 用 QueryPerformanceCounter（频率换算，秒/余数拆分乘法防
/// u64 溢出）；POSIX 用 clock_gettime(CLOCK_MONOTONIC)。纯计算不抛。
int64_t mj2x_intrinsic_system_nano_time(void);

/// @brief Objects.requireNonNull(obj)：null 抛 NullPointerException（无消息）。
///
/// @details
/// JDK 17 Objects.requireNonNull(T) 对 null 抛无消息 NPE（getMessage()
/// 为 null）；非 null 原样返回（引用透传）。异常经 context->throwException
/// 回调路由（与 _mj2x_intrinsic_parse_int 的路由模板一致）。
mj2x_ref _mj2x_intrinsic_require_non_null(mj2x_regc_context* context, mj2x_ref value);

/// @brief Objects.requireNonNull(obj, message)：null 抛 NPE，消息逐字传入。
///
/// @details
/// JDK 17 requireNonNull(T, String) 对 null 抛 new NullPointerException(message)；
/// message 为 null 引用时 NPE 消息亦为 null（ThrowNew 的 msg 参数规范允许
/// NULL）。消息经 GetStringUTFChars 取 modified UTF-8（ThrowNew 的输入口径），
/// 提取失败（OOM）时抛无消息 NPE。
mj2x_ref _mj2x_intrinsic_require_non_null_message(mj2x_regc_context* context, mj2x_ref value,
                                                  mj2x_ref message);

#ifdef __cplusplus
}
#endif

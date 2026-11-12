/// @file mj2x_regc_strictmath.h
/// @brief fdlibm 算法结构的位精确 double 数学 intrinsic(RegC/StrictMath 面)。
///
/// @details
/// java.lang.StrictMath 的契约是与 JDK 实现位精确:HotSpot 对 sin/cos/tan/
/// asin/acos/atan/atan2/exp/log/log10/pow 走 fdlibm 的 C 翻译
/// (sharedRuntimeTrans.cpp 同源),cbrt/hypot 走 StrictMath.java 内
/// FdLibm.* 的 Java 翻译,全部可追溯到 fdlibm 5.3 的同名算法。本文件按
/// fdlibm 算法结构自写 C 实现,常量与判定阈值逐项对齐,经与 JDK 17
/// StrictMath 的对拍 harness(每函数 1e5+ 随机位模式 + 全特殊值)逐位验证。
///
/// Math 面的 rint/cbrt/hypot/atan2 四条与 StrictMath 同算法(OpenJDK 的
/// Math.cbrt/hypot 即 StrictMath 转发),符号在本文件复用,省一份实现。
///
/// 位精确前提:按 IEEE double 逐运算舍入(不收缩 FMA)。x86-64 基线目标
/// (SSE2)与 -std=c99 下 gcc 默认 -ffp-contract=off、MSVC 默认 /fp:precise
/// 且非 AVX2 目标不合成 FMA,均满足;若产物改用启用 FMA 的目标,须对本 TU
/// 关闭收缩(如 -ffp-contract=off),否则对拍结论失效。
///
/// 许可:fdlibm 允许带本声明的再分发(Copyright (C) 1993 by Sun
/// Microsystems, Inc. All rights reserved. / Permission to use, copy,
/// modify, and distribute this software is freely granted, provided that
/// this notice is preserved)。实现以 netlib fdlibm 5.3 公开算法为参考
/// 自写,未从任何第三方翻译仓拷贝代码。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @name java.lang.StrictMath 位精确 12 条(全 RuntimeFunction 形态,纯、不抛)。
/// @{
double mj2x_strictmath_sin(double value);
double mj2x_strictmath_cos(double value);
double mj2x_strictmath_tan(double value);
double mj2x_strictmath_asin(double value);
double mj2x_strictmath_acos(double value);
double mj2x_strictmath_atan(double value);
/// java.lang.StrictMath.atan2(double,double):象限判定 + s_atan 核。
double mj2x_strictmath_atan2(double y, double x);
double mj2x_strictmath_exp(double value);
double mj2x_strictmath_log(double value);
double mj2x_strictmath_log10(double value);
/// java.lang.StrictMath.pow(double,double):2^y*log2(x) 路径 + 全特殊值表。
double mj2x_strictmath_pow(double x, double y);
/// java.lang.StrictMath.hypot(double,double):缩放防溢出 + sqrt 精化。
double mj2x_strictmath_hypot(double x, double y);
/// @}

/// @name java.lang.Math 边缘条目复用的 StrictMath 同源实现。
/// @{
/// java.lang.Math.rint(double):roundTiesToEven(IEEE 唯一定义,2^52 加法技巧)。
double mj2x_strictmath_rint(double value);
/// java.lang.Math.cbrt(double):OpenJDK Math.cbrt 即 StrictMath.cbrt(FdLibm.Cbrt)。
double mj2x_strictmath_cbrt(double value);
/// @}

#ifdef __cplusplus
}
#endif

/// @file mj2x_regc_strictmath.h
/// @brief fdlibm 算法结构的位精确 double 数学 intrinsic(RegC/StrictMath 面)。
///
/// @details
/// java.lang.StrictMath 的契约是与 JDK 实现位精确:HotSpot 对 sin/cos/tan/
/// asin/acos/atan/atan2/exp/log/log10/pow 走 fdlibm 的 C 翻译
/// (sharedRuntimeTrans.cpp 同源),cbrt/hypot 走 StrictMath.java 内
/// FdLibm.* 的 Java 翻译,全部可追溯到 fdlibm 5.3 的同名算法。本文件按
/// fdlibm 算法结构自写 C 实现,常量与判定阈值逐项对齐,经与 JDK 17
/// StrictMath 的比对 harness(每函数 1e5+ 随机位模式 + 全特殊值)逐位验证。
///
/// Math 面的 rint/cbrt/hypot/atan2 四条与 StrictMath 同算法(OpenJDK 的
/// Math.cbrt/hypot 即 StrictMath 转发),符号在本文件复用,省一份实现。
///
/// 位精确前提:按 IEEE double 逐运算舍入(不收缩 FMA)。x86-64 基线目标
/// (SSE2)与 -std=c99 下 gcc 默认 -ffp-contract=off、MSVC 默认 /fp:precise
/// 且非 AVX2 目标不合成 FMA,均满足;若产物改用启用 FMA 的目标,须对本 TU
/// 关闭收缩(如 -ffp-contract=off),否则比对结论失效。
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

/// @name Math/StrictMath 补缺面(perf/regc-jni-hotpath):JDK 17 实源语义对齐。
///
/// @details
/// Math 与 StrictMath 同签名共用同一 symbol(仓内既定口径):JDK 17 中
/// Math.expm1/log1p/sinh/cosh/tanh/IEEEremainder 委派 StrictMath(fdlibm
/// e_sinh/e_cosh/s_tanh/s_expm1/s_log1p/e_remainder 算法),ulp/getExponent/
/// nextAfter/nextUp/nextDown/scalb 在 StrictMath 侧逐条转委派 Math(java.lang
/// Math 位型实现),toDegrees/toRadians/fma/copySign 双方同语义。已核对的
/// 已知残差:JDK 17 StrictMath.copySign 要求 NaN sign 按正数处理,而
/// Math.copySign 不要求;共用实现取 Math 的位型语义(sign 位直取),NaN
/// sign 入参下与 StrictMath 可观测分歧,与文件头"NaN 载荷"残差同类。
/// 位精确前提同文件头注(逐运算 IEEE double 舍入,不收缩 FMA)。
/// @{
/// java.lang.Math/StrictMath.expm1(double):fdlibm s_expm1,exp(x)-1。
double mj2x_strictmath_expm1(double value);
/// java.lang.Math/StrictMath.log1p(double):fdlibm s_log1p,ln(1+x)。
double mj2x_strictmath_log1p(double value);
/// java.lang.Math/StrictMath.sinh(double):fdlibm e_sinh,expm1/exp 分段。
double mj2x_strictmath_sinh(double value);
/// java.lang.Math/StrictMath.cosh(double):fdlibm e_cosh,expm1/exp 分段。
double mj2x_strictmath_cosh(double value);
/// java.lang.Math/StrictMath.tanh(double):fdlibm s_tanh,expm1 分段。
double mj2x_strictmath_tanh(double value);
/// java.lang.Math/StrictMath.IEEEremainder(double,double):fdlibm
/// e_remainder(x REM p,最近偶数商),依赖同文件 fmod 结构。
double mj2x_strictmath_ieee_remainder(double x, double p);
/// java.lang.Math/StrictMath.copySign(double,double):位型直取 sign 位
/// (JDK 17 Math.copySign 位操作式)。
double mj2x_strictmath_copy_sign_f64(double magnitude, double sign);
/// java.lang.Math/StrictMath.copySign(float,float):位型直取 sign 位。
float mj2x_strictmath_copy_sign_f32(float magnitude, float sign);
/// java.lang.Math/StrictMath.toDegrees(double):JDK 17 常量乘式
/// angrad * RADIANS_TO_DEGREES(57.29577951308232,180/pi 双舍入)。
double mj2x_strictmath_to_degrees(double angrad);
/// java.lang.Math/StrictMath.toRadians(double):JDK 17 常量乘式
/// angdeg * DEGREES_TO_RADIANS(0.017453292519943295,pi/180 双舍入)。
double mj2x_strictmath_to_radians(double angdeg);
/// java.lang.Math/StrictMath.fma(double,double,double):C99 fma(),单次舍入。
double mj2x_strictmath_fma_f64(double a, double b, double c);
/// java.lang.Math/StrictMath.fma(float,float,float):C99 fmaf(),单次舍入。
float mj2x_strictmath_fma_f32(float a, float b, float c);
/// java.lang.Math/StrictMath.ulp(double):JDK 17 switch 公式(NaN→NaN、
/// ±Inf→+Inf、0/次正规→MIN_VALUE、其余 2^(exp-52))。
double mj2x_strictmath_ulp_f64(double value);
/// java.lang.Math/StrictMath.ulp(float):JDK 17 switch 公式(float 版)。
float mj2x_strictmath_ulp_f32(float value);
/// java.lang.Math/StrictMath.getExponent(double):阶码位直取-bias
/// (0/次正规→-1023,NaN/±Inf→1024)。
int32_t mj2x_strictmath_get_exponent_f64(double value);
/// java.lang.Math/StrictMath.getExponent(float):阶码位直取-bias(float 版)。
int32_t mj2x_strictmath_get_exponent_f32(float value);
/// java.lang.Math/StrictMath.nextAfter(double,double):JDK 17 位型
/// 邻域实现(下降/上升分支,NaN 侧 start+direction)。
double mj2x_strictmath_next_after_f64(double start, double direction);
/// java.lang.Math/StrictMath.nextUp(double):位型 +1/-1 邻域。
double mj2x_strictmath_next_up_f64(double value);
/// java.lang.Math/StrictMath.nextUp(float):位型 +1/-1 邻域(float 版)。
float mj2x_strictmath_next_up_f32(float value);
/// java.lang.Math/StrictMath.nextDown(double):位型 -1/+1 邻域(0→-MIN_VALUE)。
double mj2x_strictmath_next_down_f64(double value);
/// java.lang.Math/StrictMath.nextDown(float):位型 -1/+1 邻域(float 版)。
float mj2x_strictmath_next_down_f32(float value);
/// java.lang.Math/StrictMath.scalb(double,int):JDK 17 乘法步进式
/// (MAX_SCALE 夹取 + 2^mod512 单乘 + 2^±512 循环,至多一次真实舍入)。
double mj2x_strictmath_scalb_f64(double value, int32_t scale_factor);
/// java.lang.Math/StrictMath.scalb(float,int):JDK 17 double 加宽单乘
/// 后单次舍回 float。
float mj2x_strictmath_scalb_f32(float value, int32_t scale_factor);
/// @}

#ifdef __cplusplus
}
#endif

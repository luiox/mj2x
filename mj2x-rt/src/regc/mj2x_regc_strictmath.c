/// @file mj2x_regc_strictmath.c
/// @brief fdlibm 算法结构的位精确 double 数学实现(StrictMath 12 条 + Math rint/cbrt 复用面)。
///
/// @details
/// 算法与常量逐项对齐 netlib fdlibm 5.3(参照 k_sin/k_cos/k_tan/e_rem_pio2/
/// k_rem_pio2/e_asin/e_acos/s_atan/e_atan2/e_exp/e_log/e_log10/e_pow/
/// e_hypot/s_rint/s_cbrt/s_scalbn)。HotSpot 的 StrictMath native 实现与
/// StrictMath.java 的 FdLibm.* 翻译同源,故位精确比对成立(验证 harness 与
/// 样本量见 PR 说明)。位双关全部经 memcpy(严格别名安全,字节序无关)。
///
/// 许可声明(fdlibm 允许带本声明的再分发,保留原文):
/// ====================================================
/// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
///
/// Developed at SunSoft, a Sun Microsystems, Inc. business.
/// Permission to use, copy, modify, and distribute this
/// software is freely granted, provided that this notice
/// is preserved.
/// ====================================================
///
/// 实现注意:位精确依赖逐运算 IEEE double 舍入,不得收缩 FMA(见头文件注)。
///
/// 位精确口径(以逐位比较为据):实数、0、Inf、次正规、canonical NaN 的结果
/// 逐位一致;NaN 判定本身精确(实现返回 NaN 时 JDK 也返回 NaN,反之亦然)。
/// 已知残差:任一入参是带载荷 NaN(含 sNaN)时,pow/atan2/hypot 结果 NaN 的
/// 载荷位可能与 JDK 不同。fdlibm 用 return x+y 一类加法把 NaN 静默化,幸存
/// 操作数取决于编译器对加法的重排,本实现不强钉操作数顺序;审查加测与新
/// 种子重跑的失配均为个位数且全部落在这类入参上,其余输入逐位一致。
#include "regc/mj2x_regc_strictmath.h"

#include <math.h>
#include <string.h>

// ---- 位型访问(fdlibm __HI/__LO 的 memcpy 形态,字节序无关) ----

static uint32_t mj2x_sm_hi(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return (uint32_t)(bits >> 32);
}

static uint32_t mj2x_sm_lo(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return (uint32_t)bits;
}

static double mj2x_sm_from_words(uint32_t hi, uint32_t lo)
{
    uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
    double   value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void mj2x_sm_set_hi(double* value, uint32_t hi)
{
    uint64_t bits;
    memcpy(&bits, value, sizeof bits);
    bits = (bits & UINT64_C(0x00000000FFFFFFFF)) | ((uint64_t)hi << 32);
    memcpy(value, &bits, sizeof bits);
}

static void mj2x_sm_set_lo(double* value, uint32_t lo)
{
    uint64_t bits;
    memcpy(&bits, value, sizeof bits);
    bits = (bits & UINT64_C(0xFFFFFFFF00000000)) | (uint64_t)lo;
    memcpy(value, &bits, sizeof bits);
}

static double mj2x_sm_copysign(double magnitude, double sign_source)
{
    return mj2x_sm_from_words((mj2x_sm_hi(sign_source) & UINT32_C(0x80000000)) |
                                  (mj2x_sm_hi(magnitude) & UINT32_C(0x7FFFFFFF)),
                              mj2x_sm_lo(magnitude));
}

// ---- scalbn(fdlibm s_scalbn.c):指数加减实现 x*2^n,次正规两步缩放 ----

static double mj2x_sm_scalbn(double x, int n)
{
    static const double two54  = 1.80143985094819840000e+16; /* 0x43500000, 0x00000000 */
    static const double twom54 = 5.55111512312578270212e-17; /* 0x3C900000, 0x00000000 */
    static const double huge   = 1.0e+300;
    static const double tiny   = 1.0e-300;
    int     k;
    uint32_t hx = mj2x_sm_hi(x);
    uint32_t lx = mj2x_sm_lo(x);
    k = (int)((hx & UINT32_C(0x7FF00000)) >> 20); /* 阶码 */
    if (k == 0) {                                 /* 0 或次正规 x */
        if ((lx | (hx & UINT32_C(0x7FFFFFFF))) == 0) return x; /* +-0 */
        x *= two54;
        hx = mj2x_sm_hi(x);
        k  = (int)((hx & UINT32_C(0x7FF00000)) >> 20) - 54;
        if (n < -50000) return tiny * x;          /* 下溢 */
        if (n > 50000) return huge * x;           /* 上溢 */
    }
    if (k == 0x7FF) return x + x;                 /* NaN 或 Inf */
    k += n;
    if (k > 0x7FE) return huge * mj2x_sm_copysign(huge, x); /* 上溢 */
    if (k > 0) {                                  /* 正常结果 */
        mj2x_sm_set_hi(&x, (hx & UINT32_C(0x800FFFFF)) | ((uint32_t)k << 20));
        return x;
    }
    if (k <= -54) {
        if (n > 50000) return huge * mj2x_sm_copysign(huge, x); /* n+k 整型回绕防护 */
        return tiny * mj2x_sm_copysign(tiny, x);                /* 下溢 */
    }
    k += 54;                                      /* 次正规结果 */
    mj2x_sm_set_hi(&x, (hx & UINT32_C(0x800FFFFF)) | ((uint32_t)k << 20));
    return x * twom54;
}

// ---- __kernel_sin(fdlibm k_sin.c):[-pi/4, pi/4] 上的 sin 核 ----

static double mj2x_sm_kernel_sin(double x, double y, int iy)
{
    static const double half = 5.00000000000000000000e-01; /* 0x3FE00000, 0x00000000 */
    static const double S1   = -1.66666666666666324348e-01; /* 0xBFC55555, 0x55555549 */
    static const double S2   = 8.33333333332248946124e-03; /* 0x3F811111, 0x1110F8A6 */
    static const double S3   = -1.98412698298579493134e-04; /* 0xBF2A01A0, 0x19C161D5 */
    static const double S4   = 2.75573137070700676789e-06; /* 0x3EC71DE3, 0x57B1FE7D */
    static const double S5   = -2.50507602534068634195e-08; /* 0xBE5AE5E6, 0x8A2B9CEB */
    static const double S6   = 1.58969099521155010221e-10; /* 0x3DE5D93A, 0x5ACFD57C */
    double z, r, v;
    int    ix = (int)(mj2x_sm_hi(x) & UINT32_C(0x7FFFFFFF));
    if (ix < 0x3E400000) { /* |x| < 2**-27 */
        if ((int)x == 0) return x; /* 保持不精确语义 */
    }
    z = x * x;
    v = z * x;
    r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (half * y - v * r) - y) - v * S1);
}

// ---- __kernel_cos(fdlibm k_cos.c):[-pi/4, pi/4] 上的 cos 核 ----

static double mj2x_sm_kernel_cos(double x, double y)
{
    static const double one = 1.00000000000000000000e+00; /* 0x3FF00000, 0x00000000 */
    static const double C1  = 4.16666666666666019037e-02; /* 0x3FA55555, 0x5555554C */
    static const double C2  = -1.38888888888741095749e-03; /* 0xBF56C16C, 0x16C15177 */
    static const double C3  = 2.48015872894767294178e-05; /* 0x3EFA01A0, 0x19CB1590 */
    static const double C4  = -2.75573143513906633035e-07; /* 0xBE927E4F, 0x809C52AD */
    static const double C5  = 2.08757232129817482790e-09; /* 0x3E21EE9E, 0xBDB4B1C4 */
    static const double C6  = -1.13596475577881948265e-11; /* 0xBDA8FAE9, 0xBE8838D4 */
    double a, hz, z, r, qx;
    int    ix = (int)(mj2x_sm_hi(x) & UINT32_C(0x7FFFFFFF));
    if (ix < 0x3E400000) { /* x < 2**-27 */
        if ((int)x == 0) return one;
    }
    z = x * x;
    r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333) { /* |x| < 0.3 */
        return one - (0.5 * z - (z * r - x * y));
    }
    if (ix > 0x3FE90000) { /* x > 0.78125 */
        qx = 0.28125;
    }
    else {
        qx = mj2x_sm_from_words((uint32_t)ix - UINT32_C(0x00200000), 0); /* x/4 */
    }
    hz = 0.5 * z - qx;
    a  = one - qx;
    return a - (hz - (z * r - x * y));
}

// ---- __kernel_tan(fdlibm k_tan.c):[-pi/4, pi/4] 上的 tan 核 ----

static double mj2x_sm_kernel_tan(double x, double y, int iy)
{
    static const double T[13] = {
        3.33333333333334091986e-01,  /* 0x3FD55555, 0x55555563 */
        1.33333333333201242699e-01,  /* 0x3FC11111, 0x1110FE7A */
        5.39682539762260521377e-02,  /* 0x3FABA1BA, 0x1BB341FE */
        2.18694882948595424599e-02,  /* 0x3F9664F4, 0x8406D637 */
        8.86323982359930005737e-03,  /* 0x3F8226E3, 0xE96E8493 */
        3.59207910759131235356e-03,  /* 0x3F6D6D22, 0xC9560328 */
        1.45620945432529025516e-03,  /* 0x3F57DBC8, 0xFEE08315 */
        5.88041240820264096874e-04,  /* 0x3F4344D8, 0xF2F26501 */
        2.46463134818469906812e-04,  /* 0x3F3026F7, 0x1A8D1068 */
        7.81794442939557092300e-05,  /* 0x3F147E88, 0xA03792A6 */
        7.14072491382608190305e-05,  /* 0x3F12B80F, 0x32F0A7E9 */
        -1.85586374855275456654e-05, /* 0xBEF375CB, 0xDB605373 */
        2.59073051863633712884e-05,  /* 0x3EFB2A70, 0x74BF7AD4 */
    };
    static const double one    = 1.00000000000000000000e+00; /* 0x3FF00000, 0x00000000 */
    static const double pio4   = 7.85398163397448278999e-01; /* 0x3FE921FB, 0x54442D18 */
    static const double pio4lo = 3.06161699786838301793e-17; /* 0x3C81A626, 0x33145C07 */
    double z, r, v, w, s;
    int    hx = (int)mj2x_sm_hi(x);
    int    ix = hx & 0x7FFFFFFF;
    if (ix < 0x3E300000) { /* x < 2**-28 */
        if ((int)x == 0) { /* 保持不精确语义 */
            if (((ix | (int)mj2x_sm_lo(x)) | (iy + 1)) == 0) return one / fabs(x);
            if (iy == 1) return x;
            { /* -1/(x+y) 的精化计算 */
                double a, t;
                z = w = x + y;
                mj2x_sm_set_lo(&z, 0);
                v = y - (z - x);
                t = a = -one / w;
                mj2x_sm_set_lo(&t, 0);
                s = one + t * z;
                return t + a * (s + t * v);
            }
        }
    }
    if (ix >= 0x3FE59428) { /* |x| >= 0.6744 */
        if (hx < 0) {
            x = -x;
            y = -y;
        }
        z = pio4 - x;
        w = pio4lo - y;
        x = z + w;
        y = 0.0;
    }
    z = x * x;
    w = z * z;
    /* x^5*(T[1]+x^2*T[2]+...) 拆为奇偶两路多项式 */
    r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    s = z * x;
    r = y + z * (s * (r + v) + y);
    r += T[0] * s;
    w = x + r;
    if (ix >= 0x3FE59428) {
        v = (double)iy;
        return (double)(1 - ((hx >> 30) & 2)) * (v - 2.0 * (x - (w * w / (w + v) - r)));
    }
    if (iy == 1) return w;
    { /* -1/(x+r) 的精化计算 */
        double a, t;
        z = w;
        mj2x_sm_set_lo(&z, 0);
        v = r - (z - x); /* z+v = r+x */
        t = a = -1.0 / w;
        mj2x_sm_set_lo(&t, 0);
        s = 1.0 + t * z;
        return t + a * (s + t * v);
    }
}

// ---- __kernel_rem_pio2(fdlibm k_rem_pio2.c):大参数的 2/pi 高精度约减 ----

static int mj2x_sm_kernel_rem_pio2(double* x, double* y, int e0, int nx, int prec,
                                   const int* ipio2)
{
    static const int init_jk[] = {2, 3, 4, 6}; /* 按 prec 取 jk */
    static const double PIo2[] = {
        1.57079625129699707031e+00, /* 0x3FF921FB, 0x40000000 */
        7.54978941586159635335e-08, /* 0x3E74442D, 0x00000000 */
        5.39030252995776476554e-15, /* 0x3CF84698, 0x80000000 */
        3.28200341580791294123e-22, /* 0x3B78CC51, 0x60000000 */
        1.27065575308067607349e-29, /* 0x39F01B83, 0x80000000 */
        1.22933308981111328932e-36, /* 0x387A2520, 0x40000000 */
        2.73370053816464559624e-44, /* 0x36E38222, 0x80000000 */
        2.16741683877804819444e-51, /* 0x3569F31D, 0x00000000 */
    };
    static const double two24  = 1.67772160000000000000e+07;  /* 0x41700000, 0x00000000 */
    static const double twon24 = 5.96046447753906250000e-08;  /* 0x3E700000, 0x00000000 */
    int    jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
    double z, fw, f[20], fq[20], q[20];
    /* 本仓调用面恒 prec=2(双精度),jk=init_jk[2]=4,fq 压缩走 prec 1/2 同路 */
    jk = init_jk[prec];
    jp = jk;
    jx = nx - 1;
    jv = (e0 - 3) / 24;
    if (jv < 0) jv = 0;
    q0 = e0 - 24 * (jv + 1);
    j  = jv - jx;
    m  = jx + jk;
    for (i = 0; i <= m; i++, j++) f[i] = (j < 0) ? 0.0 : (double)ipio2[j];
    for (i = 0; i <= jk; i++) {
        for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
        q[i] = fw;
    }
    jz = jk;
recompute:
    for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
        fw    = (double)((int)(twon24 * z));
        iq[i] = (int)(z - two24 * fw);
        z     = q[j - 1] + fw;
    }
    z  = mj2x_sm_scalbn(z, q0);
    z -= 8.0 * floor(z * 0.125);
    n  = (int)z;
    z -= (double)n;
    ih = 0;
    if (q0 > 0) {
        i         = (iq[jz - 1] >> (24 - q0));
        n        += i;
        iq[jz - 1] -= i << (24 - q0);
        ih        = iq[jz - 1] >> (23 - q0);
    }
    else if (q0 == 0) {
        ih = iq[jz - 1] >> 23;
    }
    else if (z >= 0.5) {
        ih = 2;
    }
    if (ih > 0) { /* q > 0.5,取 1-q */
        n += 1;
        carry = 0;
        for (i = 0; i < jz; i++) {
            j = iq[i];
            if (carry == 0) {
                if (j != 0) {
                    carry = 1;
                    iq[i] = 0x1000000 - j;
                }
            }
            else {
                iq[i] = 0xFFFFFF - j;
            }
        }
        if (q0 > 0) { /* 罕见分支 */
            switch (q0) {
            case 1: iq[jz - 1] &= 0x7FFFFF; break;
            case 2: iq[jz - 1] &= 0x3FFFFF; break;
            }
        }
        if (ih == 2) {
            z = 1.0 - z;
            if (carry != 0) z -= mj2x_sm_scalbn(1.0, q0);
        }
    }
    if (z == 0.0) {
        j = 0;
        for (i = jz - 1; i >= jk; i--) j |= iq[i];
        if (j == 0) { /* 需要重算 */
            for (k = 1; iq[jk - k] == 0; k++) {}
            for (i = jz + 1; i <= jz + k; i++) {
                f[jx + i] = (double)ipio2[jv + i];
                for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }
    if (z == 0.0) { /* 截去零项 */
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0) {
            jz--;
            q0 -= 24;
        }
    }
    else { /* z 拆 24 位 */
        z = mj2x_sm_scalbn(z, -q0);
        if (z >= two24) {
            fw     = (double)((int)(twon24 * z));
            iq[jz] = (int)(z - two24 * fw);
            jz += 1;
            q0 += 24;
            iq[jz] = (int)fw;
        }
        else {
            iq[jz] = (int)z;
        }
    }
    fw = mj2x_sm_scalbn(1.0, q0);
    for (i = jz; i >= 0; i--) {
        q[i] = fw * (double)iq[i];
        fw *= twon24;
    }
    for (i = jz; i >= 0; i--) {
        for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++) fw += PIo2[k] * q[i + k];
        fq[jz - i] = fw;
    }
    /* prec=1/2 同路:压缩 fq[] 到 y[0..1] */
    fw = 0.0;
    for (i = jz; i >= 0; i--) fw += fq[i];
    y[0] = (ih == 0) ? fw : -fw;
    fw   = fq[0] - fw;
    for (i = 1; i <= jz; i++) fw += fq[i];
    y[1] = (ih == 0) ? fw : -fw;
    return n & 7;
}

// ---- __ieee754_rem_pio2(fdlibm e_rem_pio2.c):x - n*(pi/2) 的前两位 ----

static int mj2x_sm_rem_pio2(double x, double* y)
{
    static const int two_over_pi[] = {
        0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62,
        0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A,
        0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
        0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
        0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8,
        0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
        0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
        0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
        0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
        0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880,
        0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,
    };
    static const int npio2_hw[] = {
        0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
        0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
        0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
        0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
        0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
        0x404858EB, 0x404921FB,
    };
    static const double two24   = 1.67772160000000000000e+07; /* 0x41700000, 0x00000000 */
    static const double half    = 5.00000000000000000000e-01; /* 0x3FE00000, 0x00000000 */
    static const double invpio2 = 6.36619772367581382433e-01; /* 0x3FE45F30, 0x6DC9C883 */
    static const double pio2_1  = 1.57079632673412561417e+00; /* 0x3FF921FB, 0x54400000 */
    static const double pio2_1t = 6.07710050650619224932e-11; /* 0x3DD0B461, 0x1A626331 */
    static const double pio2_2  = 6.07710050630396597660e-11; /* 0x3DD0B461, 0x1A600000 */
    static const double pio2_2t = 2.02226624879595063154e-21; /* 0x3BA3198A, 0x2E037073 */
    static const double pio2_3  = 2.02226624871116645580e-21; /* 0x3BA3198A, 0x2E000000 */
    static const double pio2_3t = 8.47842766036889956997e-32; /* 0x397B839A, 0x252049C1 */
    double z, w, t, r, fn;
    double tx[3];
    int    e0, i, j, nx, n, ix, hx;
    hx = (int)mj2x_sm_hi(x);
    ix = hx & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) { /* |x| ~<= pi/4,无需约减 */
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002D97C) { /* |x| < 3pi/4,n=+-1 特例 */
        if (hx > 0) {
            z = x - pio2_1;
            if (ix != 0x3FF921FB) { /* 33+53 位 pi 足够 */
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            }
            else { /* 近 pi/2,用 33+33+53 位 pi */
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        z = x + pio2_1;
        if (ix != 0x3FF921FB) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        }
        else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1;
    }
    if (ix <= 0x413921FB) { /* |x| ~<= 2^19*(pi/2),中等大小 */
        t  = fabs(x);
        n  = (int)(t * invpio2 + half);
        fn = (double)n;
        r  = t - fn * pio2_1;
        w  = fn * pio2_1t; /* 首轮好到 85 位 */
        if (n < 32 && ix != npio2_hw[n - 1]) {
            y[0] = r - w; /* 快速检查无相消 */
        }
        else {
            j    = ix >> 20;
            y[0] = r - w;
            i    = j - (((int)mj2x_sm_hi(y[0]) >> 20) & 0x7FF);
            if (i > 16) { /* 第二轮,好到 118 位 */
                t  = r;
                w  = fn * pio2_2;
                r  = t - w;
                w  = fn * pio2_2t - ((t - r) - w);
                y[0] = r - w;
                i    = j - (((int)mj2x_sm_hi(y[0]) >> 20) & 0x7FF);
                if (i > 49) { /* 第三轮,好到 151 位 */
                    t  = r;
                    w  = fn * pio2_3;
                    r  = t - w;
                    w  = fn * pio2_3t - ((t - r) - w);
                    y[0] = r - w;
                }
            }
        }
        y[1] = (r - y[0]) - w;
        if (hx < 0) {
            y[0] = -y[0];
            y[1] = -y[1];
            return -n;
        }
        return n;
    }
    if (ix >= 0x7FF00000) { /* x 为 inf 或 NaN */
        y[0] = y[1] = x - x;
        return 0;
    }
    /* 其余大参数:z = scalbn(|x|, ilogb(x)-23) */
    mj2x_sm_set_lo(&z, mj2x_sm_lo(x));
    e0        = (ix >> 20) - 1046; /* e0 = ilogb(z)-23 */
    mj2x_sm_set_hi(&z, (uint32_t)ix - ((uint32_t)e0 << 20));
    for (i = 0; i < 2; i++) {
        tx[i] = (double)((int)(z));
        z     = (z - tx[i]) * two24;
    }
    tx[2] = z;
    nx    = 3;
    while (tx[nx - 1] == 0.0) nx--; /* 跳过零项 */
    n = mj2x_sm_kernel_rem_pio2(tx, y, e0, nx, 2, two_over_pi);
    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

double mj2x_strictmath_sin(double value)
{
    double y[2], z = 0.0;
    int    n, ix;
    ix = (int)mj2x_sm_hi(value);
    ix &= 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return mj2x_sm_kernel_sin(value, z, 0);
    if (ix >= 0x7FF00000) return value - value; /* NaN/Inf */
    n = mj2x_sm_rem_pio2(value, y);
    switch (n & 3) {
    case 0: return mj2x_sm_kernel_sin(y[0], y[1], 1);
    case 1: return mj2x_sm_kernel_cos(y[0], y[1]);
    case 2: return -mj2x_sm_kernel_sin(y[0], y[1], 1);
    default: return -mj2x_sm_kernel_cos(y[0], y[1]);
    }
}

double mj2x_strictmath_cos(double value)
{
    double y[2], z = 0.0;
    int    n, ix;
    ix = (int)mj2x_sm_hi(value);
    ix &= 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return mj2x_sm_kernel_cos(value, z);
    if (ix >= 0x7FF00000) return value - value; /* NaN/Inf */
    n = mj2x_sm_rem_pio2(value, y);
    switch (n & 3) {
    case 0: return mj2x_sm_kernel_cos(y[0], y[1]);
    case 1: return -mj2x_sm_kernel_sin(y[0], y[1], 1);
    case 2: return -mj2x_sm_kernel_cos(y[0], y[1]);
    default: return mj2x_sm_kernel_sin(y[0], y[1], 1);
    }
}

double mj2x_strictmath_tan(double value)
{
    double y[2], z = 0.0;
    int    n, ix;
    ix = (int)mj2x_sm_hi(value);
    ix &= 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return mj2x_sm_kernel_tan(value, z, 1);
    if (ix >= 0x7FF00000) return value - value; /* NaN/Inf */
    n = mj2x_sm_rem_pio2(value, y);
    return mj2x_sm_kernel_tan(y[0], y[1], 1 - ((n & 1) << 1));
}

double mj2x_strictmath_asin(double value)
{
    static const double one     = 1.00000000000000000000e+00; /* 0x3FF00000, 0x00000000 */
    static const double huge    = 1.000e+300;
    static const double pio2_hi = 1.57079632679489655800e+00; /* 0x3FF921FB, 0x54442D18 */
    static const double pio2_lo = 6.12323399573676603587e-17; /* 0x3C91A626, 0x33145C07 */
    static const double pio4_hi = 7.85398163397448278999e-01; /* 0x3FE921FB, 0x54442D18 */
    static const double pS0 = 1.66666666666666657415e-01;  /* 0x3FC55555, 0x55555555 */
    static const double pS1 = -3.25565818622400915405e-01; /* 0xBFD4D612, 0x03EB6F7D */
    static const double pS2 = 2.01212532134862925881e-01;  /* 0x3FC9C155, 0x0E884455 */
    static const double pS3 = -4.00555345006794114027e-02; /* 0xBFA48228, 0xB5688F3B */
    static const double pS4 = 7.91534994289814532176e-04;  /* 0x3F49EFE0, 0x7501B288 */
    static const double pS5 = 3.47933107596021167570e-05;  /* 0x3F023DE1, 0x0DFDF709 */
    static const double qS1 = -2.40339491173441421878e+00; /* 0xC0033A27, 0x1C8A2D4B */
    static const double qS2 = 2.02094576023350569471e+00;  /* 0x40002AE5, 0x9C598AC8 */
    static const double qS3 = -6.88283971605453293030e-01; /* 0xBFE6066C, 0x1B8D0159 */
    static const double qS4 = 7.70381505559019352791e-02;  /* 0x3FB3B8C5, 0xB12E9282 */
    double t, w, p, q, c, r, s;
    int    hx = (int)mj2x_sm_hi(value);
    int    ix = hx & 0x7FFFFFFF;
    if (ix >= 0x3FF00000) { /* |x| >= 1 */
        uint32_t lx = mj2x_sm_lo(value);
        if (((uint32_t)(ix - 0x3FF00000) | lx) == 0) return value * pio2_hi + value * pio2_lo;
        return (value - value) / (value - value); /* asin(|x|>1) 是 NaN */
    }
    if (ix < 0x3FE00000) { /* |x| < 0.5 */
        if (ix < 0x3E400000) { /* |x| < 2**-27 */
            if (huge + value > one) return value;
        }
        t = value * value;
        p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
        q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
        w = p / q;
        return value + value * w;
    }
    /* 1 > |x| >= 0.5 */
    w = one - fabs(value);
    t = w * 0.5;
    p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    s = sqrt(t);
    if (ix >= 0x3FEF3333) { /* |x| > 0.975 */
        w = p / q;
        t = pio2_hi - (2.0 * (s + s * w) - pio2_lo);
    }
    else {
        w = s;
        mj2x_sm_set_lo(&w, 0);
        c = (t - w * w) / (s + w);
        r = p / q;
        p = 2.0 * s * r - (pio2_lo - 2.0 * c);
        q = pio4_hi - 2.0 * w;
        t = pio4_hi - (p - q);
    }
    if (hx > 0) return t;
    return -t;
}

double mj2x_strictmath_acos(double value)
{
    static const double one     = 1.00000000000000000000e+00; /* 0x3FF00000, 0x00000000 */
    static const double pi      = 3.14159265358979311600e+00; /* 0x400921FB, 0x54442D18 */
    static const double pio2_hi = 1.57079632679489655800e+00; /* 0x3FF921FB, 0x54442D18 */
    static const double pio2_lo = 6.12323399573676603587e-17; /* 0x3C91A626, 0x33145C07 */
    static const double pS0 = 1.66666666666666657415e-01;  /* 0x3FC55555, 0x55555555 */
    static const double pS1 = -3.25565818622400915405e-01; /* 0xBFD4D612, 0x03EB6F7D */
    static const double pS2 = 2.01212532134862925881e-01;  /* 0x3FC9C155, 0x0E884455 */
    static const double pS3 = -4.00555345006794114027e-02; /* 0xBFA48228, 0xB5688F3B */
    static const double pS4 = 7.91534994289814532176e-04;  /* 0x3F49EFE0, 0x7501B288 */
    static const double pS5 = 3.47933107596021167570e-05;  /* 0x3F023DE1, 0x0DFDF709 */
    static const double qS1 = -2.40339491173441421878e+00; /* 0xC0033A27, 0x1C8A2D4B */
    static const double qS2 = 2.02094576023350569471e+00;  /* 0x40002AE5, 0x9C598AC8 */
    static const double qS3 = -6.88283971605453293030e-01; /* 0xBFE6066C, 0x1B8D0159 */
    static const double qS4 = 7.70381505559019352791e-02;  /* 0x3FB3B8C5, 0xB12E9282 */
    double z, p, q, r, w, s, c, df;
    int    hx = (int)mj2x_sm_hi(value);
    int    ix = hx & 0x7FFFFFFF;
    if (ix >= 0x3FF00000) { /* |x| >= 1 */
        uint32_t lx = mj2x_sm_lo(value);
        if (((uint32_t)(ix - 0x3FF00000) | lx) == 0) { /* |x| == 1 */
            if (hx > 0) return 0.0;                    /* acos(1) = 0 */
            return pi + 2.0 * pio2_lo;                 /* acos(-1) = pi */
        }
        return (value - value) / (value - value); /* acos(|x|>1) 是 NaN */
    }
    if (ix < 0x3FE00000) { /* |x| < 0.5 */
        if (ix <= 0x3C600000) return pio2_hi + pio2_lo; /* |x| < 2**-57 */
        z = value * value;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        r = p / q;
        return pio2_hi - (value - (pio2_lo - value * r));
    }
    if (hx < 0) { /* x < -0.5 */
        z = (one + value) * 0.5;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        s = sqrt(z);
        r = p / q;
        w = r * s - pio2_lo;
        return pi - 2.0 * (s + w);
    }
    /* x > 0.5 */
    z  = (one - value) * 0.5;
    s  = sqrt(z);
    df = s;
    mj2x_sm_set_lo(&df, 0);
    c = (z - df * df) / (s + df);
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    w = r * s + c;
    return 2.0 * (df + w);
}

double mj2x_strictmath_atan(double value)
{
    static const double atanhi[] = {
        4.63647609000806093515e-01, /* atan(0.5)hi 0x3FDDAC67, 0x0561BB4F */
        7.85398163397448278999e-01, /* atan(1.0)hi 0x3FE921FB, 0x54442D18 */
        9.82793723247329054082e-01, /* atan(1.5)hi 0x3FEF730B, 0xD281F69B */
        1.57079632679489655800e+00, /* atan(inf)hi 0x3FF921FB, 0x54442D18 */
    };
    static const double atanlo[] = {
        2.26987774529616870924e-17, /* atan(0.5)lo 0x3C7A2B7F, 0x222F65E2 */
        3.06161699786838301793e-17, /* atan(1.0)lo 0x3C81A626, 0x33145C07 */
        1.39033110312309984516e-17, /* atan(1.5)lo 0x3C700788, 0x7AF0CBBD */
        6.12323399573676603587e-17, /* atan(inf)lo 0x3C91A626, 0x33145C07 */
    };
    static const double aT[] = {
        3.33333333333329318027e-01,  /* 0x3FD55555, 0x5555550D */
        -1.99999999998764832476e-01, /* 0xBFC99999, 0x9998EBC4 */
        1.42857142725034663711e-01,  /* 0x3FC24924, 0x920083FF */
        -1.11111104054623557880e-01, /* 0xBFBC71C6, 0xFE231671 */
        9.09088713343650656196e-02,  /* 0x3FB745CD, 0xC54C206E */
        -7.69187620504482999495e-02, /* 0xBFB3B0F2, 0xAF749A6D */
        6.66107313738753120669e-02,  /* 0x3FB10D66, 0xA0D03D51 */
        -5.83357013379057348645e-02, /* 0xBFADDE2D, 0x52DEFD9A */
        4.97687799461593236017e-02,  /* 0x3FA97B4B, 0x24760DEB */
        -3.65315727442169155270e-02, /* 0xBFA2B444, 0x2C6A6C2F */
        1.62858201153657823623e-02,  /* 0x3F90AD3A, 0xE322DA11 */
    };
    static const double one  = 1.0;
    static const double huge = 1.0e300;
    double w, s1, s2, z;
    int    hx = (int)mj2x_sm_hi(value);
    int    ix = hx & 0x7FFFFFFF;
    int    id;
    if (ix >= 0x44100000) { /* |x| >= 2^66 */
        uint32_t lx = mj2x_sm_lo(value);
        if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0)) return value + value; /* NaN */
        if (hx > 0) return atanhi[3] + atanlo[3];
        return -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3FDC0000) { /* |x| < 0.4375 */
        if (ix < 0x3E200000) { /* |x| < 2^-29 */
            if (huge + value > one) return value; /* 保持不精确语义 */
        }
        id = -1;
    }
    else {
        value = fabs(value);
        if (ix < 0x3FF30000) { /* |x| < 1.1875 */
            if (ix < 0x3FE60000) { /* 7/16 <= |x| < 11/16 */
                id    = 0;
                value = (2.0 * value - one) / (2.0 + value);
            }
            else { /* 11/16 <= |x| < 19/16 */
                id    = 1;
                value = (value - one) / (value + one);
            }
        }
        else {
            if (ix < 0x40038000) { /* |x| < 2.4375 */
                id    = 2;
                value = (value - 1.5) / (one + 1.5 * value);
            }
            else { /* 2.4375 <= |x| < 2^66 */
                id    = 3;
                value = -1.0 / value;
            }
        }
    }
    z = value * value;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return value - value * (s1 + s2);
    z = atanhi[id] - ((value * (s1 + s2) - atanlo[id]) - value);
    return (hx < 0) ? -z : z;
}

double mj2x_strictmath_atan2(double y, double x)
{
    static const double tiny    = 1.0e-300;
    static const double zero    = 0.0;
    static const double pi_o_4  = 7.8539816339744827900E-01;  /* 0x3FE921FB, 0x54442D18 */
    static const double pi_o_2  = 1.5707963267948965580E+00;  /* 0x3FF921FB, 0x54442D18 */
    static const double pi      = 3.1415926535897931160E+00;  /* 0x400921FB, 0x54442D18 */
    static const double pi_lo   = 1.2246467991473531772E-16;  /* 0x3CA1A626, 0x33145C07 */
    double z;
    int    k, m, hx, hy, ix, iy;
    uint32_t lx, ly;
    hx = (int)mj2x_sm_hi(x);
    ix = hx & 0x7FFFFFFF;
    lx = mj2x_sm_lo(x);
    hy = (int)mj2x_sm_hi(y);
    iy = hy & 0x7FFFFFFF;
    ly = mj2x_sm_lo(y);
    if (((ix | (uint32_t)((lx | -lx) >> 31)) > (uint32_t)0x7FF00000) ||
        ((iy | (uint32_t)((ly | -ly) >> 31)) > (uint32_t)0x7FF00000)) { /* x 或 y 是 NaN */
        return x + y;
    }
    if (((uint32_t)(hx - 0x3FF00000) | lx) == 0) return mj2x_strictmath_atan(y); /* x = 1.0 */
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2); /* 2*sign(x)+sign(y) */
    if ((iy | (int)ly) == 0) { /* y = 0 */
        switch (m) {
        case 0:
        case 1: return y; /* atan(+-0,+anything) = +-0 */
        case 2: return pi + tiny; /* atan(+0,-anything) = pi */
        default: return -pi - tiny; /* atan(-0,-anything) = -pi */
        }
    }
    if ((ix | (int)lx) == 0) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny; /* x = 0 */
    if (ix == 0x7FF00000) { /* x 是 inf */
        if (iy == 0x7FF00000) {
            switch (m) {
            case 0: return pi_o_4 + tiny;          /* atan(+INF,+INF) */
            case 1: return -pi_o_4 - tiny;         /* atan(-INF,+INF) */
            case 2: return 3.0 * pi_o_4 + tiny;    /* atan(+INF,-INF) */
            default: return -3.0 * pi_o_4 - tiny;  /* atan(-INF,-INF) */
            }
        }
        switch (m) {
        case 0: return zero;         /* atan(+...,+INF) */
        case 1: return -zero;        /* atan(-...,+INF) */
        case 2: return pi + tiny;    /* atan(+...,-INF) */
        default: return -pi - tiny;  /* atan(-...,-INF) */
        }
    }
    if (iy == 0x7FF00000) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny; /* y 是 inf */
    k = (iy - ix) >> 20;
    if (k > 60) {
        z = pi_o_2 + 0.5 * pi_lo; /* |y/x| > 2**60 */
    }
    else if (hx < 0 && k < -60) {
        z = 0.0; /* |y|/x < -2**60 */
    }
    else {
        z = mj2x_strictmath_atan(fabs(y / x)); /* y/x 安全 */
    }
    switch (m) {
    case 0: return z;                    /* atan(+,+) */
    case 1:
        mj2x_sm_set_hi(&z, mj2x_sm_hi(z) ^ UINT32_C(0x80000000));
        return z;                        /* atan(-,+) */
    case 2: return pi - (z - pi_lo);     /* atan(+,-) */
    default: return (z - pi_lo) - pi;    /* atan(-,-) */
    }
}

double mj2x_strictmath_exp(double value)
{
    static const double one        = 1.0;
    static const double halF[2]    = {0.5, -0.5};
    static const double huge       = 1.0e+300;
    static const double twom1000   = 9.33263618503218878990e-302; /* 2**-1000 = 0x01700000,0 */
    static const double o_threshold = 7.09782712893383973096e+02; /* 0x40862E42, 0xFEFA39EF */
    static const double u_threshold = -7.45133219101941108420e+02; /* 0xC0874910, 0xD52D3051 */
    static const double ln2HI[2]   = {6.93147180369123816490e-01,   /* 0x3FE62E42, 0xFEE00000 */
                                      -6.93147180369123816490e-01}; /* 0xBFE62E42, 0xFEE00000 */
    static const double ln2LO[2]   = {1.90821492927058770002e-10,   /* 0x3DEA39EF, 0x35793C76 */
                                      -1.90821492927058770002e-10}; /* 0xBDEA39EF, 0x35793C76 */
    static const double invln2 = 1.44269504088896338700e+00; /* 0x3FF71547, 0x652B82FE */
    static const double P1 = 1.66666666666666019037e-01;     /* 0x3FC55555, 0x5555553E */
    static const double P2 = -2.77777777770155933842e-03;    /* 0xBF66C16C, 0x16BEBD93 */
    static const double P3 = 6.61375632143793436117e-05;     /* 0x3F11566A, 0xAF25DE2C */
    static const double P4 = -1.65339022054652515390e-06;    /* 0xBEBBBD41, 0xC5D26BF1 */
    static const double P5 = 4.13813679705723846039e-08;     /* 0x3E663769, 0x72BEA4D0 */
    double y, hi, lo = 0.0, c, t;
    int    k, xsb;
    uint32_t hx;
    hx  = mj2x_sm_hi(value);
    xsb = (int)(hx >> 31);
    hx &= UINT32_C(0x7FFFFFFF);
    if (hx >= UINT32_C(0x40862E42)) { /* |x| >= 709.78... */
        if (hx >= UINT32_C(0x7FF00000)) {
            if (((hx & UINT32_C(0xFFFFF)) | mj2x_sm_lo(value)) != 0) {
                return value + value; /* NaN */
            }
            return (xsb == 0) ? value : 0.0; /* exp(+-inf) = {inf,0} */
        }
        if (value > o_threshold) return huge * huge; /* 上溢 */
        if (value < u_threshold) return twom1000 * twom1000; /* 下溢 */
    }
    if (hx > UINT32_C(0x3FD62E42)) { /* |x| > 0.5*ln2 */
        if (hx < UINT32_C(0x3FF0A2B2)) { /* |x| < 1.5*ln2 */
            hi = value - ln2HI[xsb];
            lo = ln2LO[xsb];
            k  = 1 - xsb - xsb;
        }
        else {
            k  = (int)(invln2 * value + halF[xsb]);
            t  = (double)k;
            hi = value - t * ln2HI[0]; /* t*ln2HI 精确 */
            lo = t * ln2LO[0];
        }
        value = hi - lo;
    }
    else if (hx < UINT32_C(0x3E300000)) { /* |x| < 2**-28 */
        if (huge + value > one) return one + value; /* 触发不精确 */
    }
    else {
        k = 0;
    }
    t = value * value;
    c = value - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return one - ((value * c) / (c - 2.0) - value);
    y = one - ((lo - (value * c) / (2.0 - c)) - hi);
    if (k >= -1021) {
        mj2x_sm_set_hi(&y, mj2x_sm_hi(y) + (uint32_t)(k << 20));
        return y;
    }
    mj2x_sm_set_hi(&y, mj2x_sm_hi(y) + (uint32_t)((k + 1000) << 20));
    return y * twom1000;
}

double mj2x_strictmath_log(double value)
{
    static const double ln2_hi = 6.93147180369123816490e-01; /* 0x3FE62E42, 0xFEE00000 */
    static const double ln2_lo = 1.90821492927058770002e-10; /* 0x3DEA39EF, 0x35793C76 */
    static const double two54  = 1.80143985094819840000e+16; /* 0x43500000, 0x00000000 */
    static const double Lg1 = 6.666666666666735130e-01; /* 0x3FE55555, 0x55555593 */
    static const double Lg2 = 3.999999999940941908e-01; /* 0x3FD99999, 0x9997FA04 */
    static const double Lg3 = 2.857142874366239149e-01; /* 0x3FD24924, 0x94229359 */
    static const double Lg4 = 2.222219843214978396e-01; /* 0x3FCC71C5, 0x1D8E78AF */
    static const double Lg5 = 1.818357216161805012e-01; /* 0x3FC74664, 0x96CB03DE */
    static const double Lg6 = 1.531383769920937332e-01; /* 0x3FC39A09, 0xD078C69F */
    static const double Lg7 = 1.479819860511658591e-01; /* 0x3FC2F112, 0xDF3E5244 */
    double hfsq, f, s, z, R, w, t1, t2, dk;
    int    k, hx, i, j;
    uint32_t lx;
    hx = (int)mj2x_sm_hi(value);
    lx = mj2x_sm_lo(value);
    k  = 0;
    if (hx < 0x00100000) { /* x < 2**-1022 */
        if (((hx & 0x7FFFFFFF) | (int)lx) == 0) return -two54 / 0.0; /* log(+-0) = -inf */
        if (hx < 0) return (value - value) / 0.0; /* log(-#) = NaN */
        k -= 54;
        value *= two54; /* 次正规,放大 */
        hx = (int)mj2x_sm_hi(value);
    }
    if (hx >= 0x7FF00000) return value + value;
    k += (hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    i = (hx + 0x95F64) & 0x100000;
    mj2x_sm_set_hi(&value, (uint32_t)hx | ((uint32_t)i ^ UINT32_C(0x3FF00000)));
    k += i >> 20;
    f = value - 1.0;
    if ((0x000FFFFF & (2 + hx)) < 0x3) { /* |f| < 2**-20 */
        if (f == 0.0) {
            if (k == 0) return 0.0;
            dk = (double)k;
            return dk * ln2_hi + dk * ln2_lo;
        }
        R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        dk = (double)k;
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s  = f / (2.0 + f);
    dk = (double)k;
    z  = s * s;
    i  = hx - 0x6147A;
    w  = z * z;
    j  = 0x6B851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    R = t2 + t1;
    if (i > 0) {
        hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

double mj2x_strictmath_log10(double value)
{
    static const double two54     = 1.80143985094819840000e+16; /* 0x43500000, 0x00000000 */
    static const double ivln10    = 4.34294481903251816668e-01; /* 0x3FDBCB7B, 0x1526E50E */
    static const double log10_2hi = 3.01029995663611771306e-01; /* 0x3FD34413, 0x509F6000 */
    static const double log10_2lo = 3.69423907715893078616e-13; /* 0x3D59FEF3, 0x11F12B36 */
    double y, z;
    int    i, k, hx;
    uint32_t lx;
    hx = (int)mj2x_sm_hi(value);
    lx = mj2x_sm_lo(value);
    k  = 0;
    if (hx < 0x00100000) { /* x < 2**-1022 */
        if (((hx & 0x7FFFFFFF) | (int)lx) == 0) return -two54 / 0.0; /* log10(+-0) = -inf */
        if (hx < 0) return (value - value) / 0.0; /* log10(-#) = NaN */
        k -= 54;
        value *= two54; /* 次正规,放大 */
        hx = (int)mj2x_sm_hi(value);
    }
    if (hx >= 0x7FF00000) return value + value;
    k += (hx >> 20) - 1023;
    i  = (int)(((uint32_t)k & UINT32_C(0x80000000)) >> 31);
    hx = (int)((hx & 0x000FFFFF) | ((0x3FF - i) << 20));
    y  = (double)(k + i);
    mj2x_sm_set_hi(&value, (uint32_t)hx);
    z = y * log10_2lo + ivln10 * mj2x_strictmath_log(value);
    return z + y * log10_2hi;
}

double mj2x_strictmath_pow(double x, double y)
{
    static const double bp[]   = {1.0, 1.5};
    static const double dp_h[] = {0.0, 5.84962487220764160156e-01}; /* 0x3FE2B803, 0x40000000 */
    static const double dp_l[] = {0.0, 1.35003920212974897128e-08}; /* 0x3E4CFDEB, 0x43CFD006 */
    static const double zero   = 0.0;
    static const double one    = 1.0;
    static const double two    = 2.0;
    static const double two53  = 9007199254740992.0; /* 0x43400000, 0x00000000 */
    static const double huge   = 1.0e300;
    static const double tiny   = 1.0e-300;
    /* (3/2)*(log(x)-2s-2/3*s^3) 的多项式系数 */
    static const double L1 = 5.99999999999994648725e-01;   /* 0x3FE33333, 0x33333303 */
    static const double L2 = 4.28571428578550184252e-01;   /* 0x3FDB6DB6, 0xDB6FABFF */
    static const double L3 = 3.33333329818377432918e-01;   /* 0x3FD55555, 0x518F264D */
    static const double L4 = 2.72728123808534006489e-01;   /* 0x3FD17460, 0xA91D4101 */
    static const double L5 = 2.30660745775561754067e-01;   /* 0x3FCD864A, 0x93C9DB65 */
    static const double L6 = 2.06975017800338417784e-01;   /* 0x3FCA7E28, 0x4A454EEF */
    static const double P1 = 1.66666666666666019037e-01;   /* 0x3FC55555, 0x5555553E */
    static const double P2 = -2.77777777770155933842e-03;  /* 0xBF66C16C, 0x16BEBD93 */
    static const double P3 = 6.61375632143793436117e-05;   /* 0x3F11566A, 0xAF25DE2C */
    static const double P4 = -1.65339022054652515390e-06;  /* 0xBEBBBD41, 0xC5D26BF1 */
    static const double P5 = 4.13813679705723846039e-08;   /* 0x3E663769, 0x72BEA4D0 */
    static const double lg2   = 6.93147180559945286227e-01;  /* 0x3FE62E42, 0xFEFA39EF */
    static const double lg2_h = 6.93147182464599609375e-01;  /* 0x3FE62E43, 0x00000000 */
    static const double lg2_l = -1.90465429995776804525e-09; /* 0xBE205C61, 0x0CA86C39 */
    static const double ovt   = 8.0085662595372944372e-017;  /* -(1024-log2(ovfl+.5ulp)) */
    static const double cp    = 9.61796693925975554329e-01;  /* 0x3FEEC709, 0xDC3A03FD =2/(3ln2) */
    static const double cp_h  = 9.61796700954437255859e-01;  /* 0x3FEEC709, 0xE0000000 =(float)cp */
    static const double cp_l  = -7.02846165095275826516e-09; /* 0xBE3E2FE0, 0x145B01F5 =cp 尾数 */
    static const double ivln2    = 1.44269504088896338700e+00;    /* 0x3FF71547, 0x652B82FE */
    static const double ivln2_h  = 1.44269502162933349609e+00;    /* 0x3FF71547, 0x60000000 =24b 1/ln2 */
    static const double ivln2_l  = 1.92596299112661746887e-08;    /* 0x3E54AE0B, 0xF85DDF44 =1/ln2 尾数 */
    double z, ax, z_h, z_l, p_h, p_l;
    double y1, t1, t2, r, s, t, u, v, w;
    int    i, j, k, yisint, n;
    int    hx, hy, ix, iy;
    uint32_t lx, ly;
    hx = (int)mj2x_sm_hi(x);
    lx = mj2x_sm_lo(x);
    hy = (int)mj2x_sm_hi(y);
    ly = mj2x_sm_lo(y);
    ix = hx & 0x7FFFFFFF;
    iy = hy & 0x7FFFFFFF;
    if ((iy | (int)ly) == 0) return one; /* y == 0: x**0 = 1 */
    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0) ||
        iy > 0x7FF00000 || (iy == 0x7FF00000 && ly != 0)) {
        return x + y; /* +-NaN 返回 x+y */
    }
    /* x<0 时 y 是否奇整数:yisint = 0 非 / 1 奇 / 2 偶 */
    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000) {
            yisint = 2; /* 偶整数 y */
        }
        else if (iy >= 0x3FF00000) {
            k = (iy >> 20) - 0x3FF;
            if (k > 20) {
                /* k 可到 52,有符号左移会溢出;用无符号回绕语义(与 fdlibm
                 * 实际编译行为一致),判 ly 低 (52-k) 位是否全零 */
                uint32_t ju = ly >> (52 - k);
                if ((ju << (52 - k)) == ly) yisint = 2 - (int)(ju & 1u);
            }
            else if (ly == 0) {
                j = iy >> (20 - k);
                if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
            }
        }
    }
    if (ly == 0) { /* y 的特殊值 */
        if (iy == 0x7FF00000) { /* y 是 +-inf */
            if (((uint32_t)(ix - 0x3FF00000) | lx) == 0) return y - y; /* inf**+-1 是 NaN */
            if (ix >= 0x3FF00000) return (hy >= 0) ? y : zero;
            return (hy < 0) ? -y : zero;
        }
        if (iy == 0x3FF00000) { /* y 是 +-1 */
            if (hy < 0) return one / x;
            return x;
        }
        if (hy == 0x40000000) return x * x; /* y 是 2 */
        if (hy == 0x3FE00000) { /* y 是 0.5 */
            if (hx >= 0) return sqrt(x);
        }
    }
    ax = fabs(x);
    if (lx == 0) { /* x 的特殊值 */
        if (ix == 0x7FF00000 || ix == 0 || ix == 0x3FF00000) {
            z = ax; /* x 是 +-0/+-inf/+-1 */
            if (hy < 0) z = one / z;
            if (hx < 0) {
                if (((uint32_t)(ix - 0x3FF00000) | (uint32_t)yisint) == 0) {
                    z = (z - z) / (z - z); /* (-1)**非整数 是 NaN */
                }
                else if (yisint == 1) {
                    z = -z; /* (x<0)**奇 = -(|x|**奇) */
                }
            }
            return z;
        }
    }
    n = (hx >> 31) + 1;
    if ((n | yisint) == 0) return (x - x) / (x - x); /* (x<0)**(非整数) 是 NaN */
    s = one;
    if ((n | (yisint - 1)) == 0) s = -one; /* (-ve)**(奇整数) */
    if (iy > 0x41E00000) { /* |y| 巨大 */
        if (iy > 0x43F00000) { /* |y| > 2**64,必然上/下溢 */
            if (ix <= 0x3FEFFFFF) return (hy < 0) ? huge * huge : tiny * tiny;
            if (ix >= 0x3FF00000) return (hy > 0) ? huge * huge : tiny * tiny;
        }
        /* x 不接近 1 时上/下溢 */
        if (ix < 0x3FEFFFFF) return (hy < 0) ? s * huge * huge : s * tiny * tiny;
        if (ix > 0x3FF00000) return (hy > 0) ? s * huge * huge : s * tiny * tiny;
        /* |1-x| <= 2**-20,用 x-x^2/2+x^3/3-x^4/4 计算 log(x) */
        t = ax - one; /* t 有 20 个尾零 */
        w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        u = ivln2_h * t;
        v = t * ivln2_l - w * ivln2;
        t1 = u + v;
        mj2x_sm_set_lo(&t1, 0);
        t2 = v - (t1 - u);
    }
    else {
        double ss, s2, s_h, s_l, t_h, t_l;
        n = 0;
        if (ix < 0x00100000) { /* 次正规 */
            ax *= two53;
            n -= 53;
            ix = (int)(mj2x_sm_hi(ax) & UINT32_C(0x7FFFFFFF));
        }
        n += (ix >> 20) - 0x3FF;
        j  = ix & 0x000FFFFF;
        ix = j | 0x3FF00000; /* 归一化 ix */
        if (j <= 0x3988E) k = 0;         /* |x| < sqrt(3/2) */
        else if (j < 0xBB67A) k = 1;     /* |x| < sqrt(3) */
        else {
            k = 0;
            n += 1;
            ix -= 0x00100000;
        }
        mj2x_sm_set_hi(&ax, (uint32_t)ix);
        /* ss = s_h+s_l = (x-1)/(x+1) 或 (x-1.5)/(x+1.5) */
        u = ax - bp[k];
        v = one / (ax + bp[k]);
        ss = u * v;
        s_h = ss;
        mj2x_sm_set_lo(&s_h, 0);
        t_h = zero;
        mj2x_sm_set_hi(&t_h, (uint32_t)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)));
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l);
        /* 计算 log(ax) */
        s2 = ss * ss;
        r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = 3.0 + s2 + r;
        mj2x_sm_set_lo(&t_h, 0);
        t_l = r - ((t_h - 3.0) - s2);
        /* u+v = ss*(1+...) */
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        /* 2/(3*log2)*(ss+...) */
        p_h = u + v;
        mj2x_sm_set_lo(&p_h, 0);
        p_l = v - (p_h - u);
        z_h = cp_h * p_h;
        z_l = cp_l * p_h + p_l * cp + dp_l[k];
        /* log2(ax) = (ss+..)*2/(3*log2) = n + dp_h + z_h + z_l */
        t = (double)n;
        t1 = (((z_h + z_l) + dp_h[k]) + t);
        mj2x_sm_set_lo(&t1, 0);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }
    /* y 拆 y1+y2,计算 (y1+y2)*(t1+t2) */
    y1 = y;
    mj2x_sm_set_lo(&y1, 0);
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    j = (int)mj2x_sm_hi(z);
    i = (int)mj2x_sm_lo(z);
    if (j >= 0x40900000) { /* z >= 1024 */
        if (((j - 0x40900000) | i) != 0) return s * huge * huge; /* 上溢 */
        if (p_l + ovt > z - p_h) return s * huge * huge;         /* 上溢 */
    }
    else if ((j & 0x7FFFFFFF) >= 0x4090CC00) { /* z <= -1075 */
        if (((j - 0xC090CC00) | i) != 0) return s * tiny * tiny; /* 下溢 */
        if (p_l <= z - p_h) return s * tiny * tiny;              /* 下溢 */
    }
    /* 计算 2**(p_h+p_l) */
    i = j & 0x7FFFFFFF;
    k = (i >> 20) - 0x3FF;
    n = 0;
    if (i > 0x3FE00000) { /* |z| > 0.5,n = [z+0.5] */
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7FFFFFFF) >> 20) - 0x3FF;
        t = zero;
        mj2x_sm_set_hi(&t, (uint32_t)n & (uint32_t)(~(0x000FFFFF >> k)));
        n = ((n & 0x000FFFFF) | 0x00100000) >> (20 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    t = p_l + p_h;
    mj2x_sm_set_lo(&t, 0);
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    r = (z * t1) / (t1 - two) - (w + z * w);
    z = one - (r - z);
    j = (int)mj2x_sm_hi(z);
    j += (n << 20);
    if ((j >> 20) <= 0) {
        z = mj2x_sm_scalbn(z, n); /* 次正规输出 */
    }
    else {
        mj2x_sm_set_hi(&z, (uint32_t)j);
    }
    return s * z;
}

double mj2x_strictmath_hypot(double x, double y)
{
    double a, b, t1, t2, y1, y2, w;
    int    j, k, ha, hb;
    ha = (int)(mj2x_sm_hi(x) & UINT32_C(0x7FFFFFFF));
    hb = (int)(mj2x_sm_hi(y) & UINT32_C(0x7FFFFFFF));
    if (hb > ha) {
        a  = y;
        b  = x;
        j  = ha;
        ha = hb;
        hb = j;
    }
    else {
        a = x;
        b = y;
    }
    mj2x_sm_set_hi(&a, (uint32_t)ha); /* a <- |a| */
    mj2x_sm_set_hi(&b, (uint32_t)hb); /* b <- |b| */
    if ((ha - hb) > 0x3C00000) return a + b; /* x/y > 2**60 */
    k = 0;
    if (ha > 0x5F300000) { /* a > 2**500 */
        if (ha >= 0x7FF00000) { /* Inf 或 NaN */
            w = a + b; /* 对 sNaN */
            if (((ha & 0xFFFFF) | (int)mj2x_sm_lo(a)) == 0) w = a;
            if (((hb ^ 0x7FF00000) | (int)mj2x_sm_lo(b)) == 0) w = b;
            return w;
        }
        ha -= 0x25800000; /* 缩放 a 和 b 至 2**-600 */
        hb -= 0x25800000;
        k += 600;
        mj2x_sm_set_hi(&a, (uint32_t)ha);
        mj2x_sm_set_hi(&b, (uint32_t)hb);
    }
    if (hb < 0x20B00000) { /* b < 2**-500 */
        if (hb <= 0x000FFFFF) { /* b 次正规或 0 */
            if ((hb | (int)mj2x_sm_lo(b)) == 0) return a;
            t1 = 0.0;
            mj2x_sm_set_hi(&t1, UINT32_C(0x7FD00000)); /* t1 = 2^1022 */
            b *= t1;
            a *= t1;
            k -= 1022;
        }
        else { /* 缩放 a 和 b 至 2^600 */
            ha += 0x25800000;
            hb += 0x25800000;
            k -= 600;
            mj2x_sm_set_hi(&a, (uint32_t)ha);
            mj2x_sm_set_hi(&b, (uint32_t)hb);
        }
    }
    w = a - b;
    if (w > b) {
        t1 = 0.0;
        mj2x_sm_set_hi(&t1, (uint32_t)ha);
        t2 = a - t1;
        w  = sqrt(t1 * t1 - (b * (-b) - t2 * (a + t1)));
    }
    else {
        a  = a + a;
        y1 = 0.0;
        mj2x_sm_set_hi(&y1, (uint32_t)hb);
        y2 = b - y1;
        t1 = 0.0;
        mj2x_sm_set_hi(&t1, (uint32_t)(ha + 0x00100000));
        t2 = a - t1;
        w  = sqrt(t1 * y1 - (w * (-w) - (t1 * y2 + t2 * b)));
    }
    if (k != 0) {
        t1 = 1.0;
        mj2x_sm_set_hi(&t1, mj2x_sm_hi(t1) + (uint32_t)(k << 20));
        return t1 * w;
    }
    return w;
}

double mj2x_strictmath_rint(double value)
{
    static const double TWO52 = 4.50359962737049600000e+15; /* 2^52 = 0x43300000, 0x00000000 */
    /* 结构对齐 JDK 17 StrictMath.rint(与 fdlibm s_rint 同值域):sign 携带
     * 符号,|a| < 2^52 时以 2^52 加减舍入到整值,其余(整值/inf/NaN)经
     * sign*a 原样(或静默化 NaN)返回。NaN 载荷经乘法静默化,与 JDK 一致。 */
    double sign = mj2x_sm_copysign(1.0, value);
    double a    = fabs(value);
    if (a < TWO52) {
        a = (TWO52 + a) - TWO52;
    }
    return sign * a;
}

double mj2x_strictmath_cbrt(double value)
{
    static const unsigned B1 = 715094163u; /* B1 = (682-0.03306235651)*2**20 */
    static const unsigned B2 = 696219795u; /* B2 = (664-0.03306235651)*2**20 */
    static const double C = 5.42857142857142815906e-01; /* 19/35     = 0x3FE15F15, 0xF15F15F1 */
    static const double D = -7.05306122448979611050e-01; /* -864/1225 = 0xBFE691DE, 0x2532C834 */
    static const double E = 1.41428571428571436819e+00; /* 99/70     = 0x3FF6A0EA, 0x0EA0EA0F */
    static const double F = 1.60714285714285720630e+00; /* 45/28     = 0x3FF9B6DB, 0x6DB6DB6E */
    static const double G = 3.57142857142857150787e-01; /* 5/14      = 0x3FD6DB6D, 0xB6DB6DB7 */
    int      hx;
    double   r, s, t = 0.0, w;
    unsigned sign;
    /* JDK 17 FdLibm.Cbrt:0 与 NaN/Inf 一律原样返回(不同于 fdlibm 的 x+x,
     * 后者会把 signaling NaN 静默化,与 JDK 观测行为不符)。 */
    if (value == 0.0 || !isfinite(value)) return value;
    hx = (int)mj2x_sm_hi(value);
    sign = (unsigned)hx & UINT32_C(0x80000000);
    hx   = (int)((unsigned)hx ^ sign);
    mj2x_sm_set_hi(&value, (uint32_t)hx); /* value <- |value| */
    if (hx < 0x00100000) { /* 次正规:粗 cbrt 到 5 位 */
        mj2x_sm_set_hi(&t, UINT32_C(0x43500000)); /* t = 2**54 */
        t *= value;
        mj2x_sm_set_hi(&t, mj2x_sm_hi(t) / 3u + B2);
    }
    else {
        mj2x_sm_set_hi(&t, (uint32_t)hx / 3u + B1);
    }
    /* 新 cbrt 到 23 位 */
    r = t * t / value;
    s = C + r * t;
    t *= G + F / (s + E + D / s);
    /* 截到 20 位并略大于 cbrt(x) */
    mj2x_sm_set_lo(&t, 0);
    mj2x_sm_set_hi(&t, mj2x_sm_hi(t) + 1u);
    /* 一步牛顿迭代到 53 位,误差 < 0.667 ulp */
    s = t * t; /* t*t 精确 */
    r = value / s;
    w = t + t;
    r = (r - t) / (w + r); /* r-t 精确 */
    t = t + t * r;
    mj2x_sm_set_hi(&t, mj2x_sm_hi(t) | sign); /* 恢复符号位 */
    return t;
}

// ---- Math/StrictMath 补缺面(perf/regc-jni-hotpath) ----
//
// JDK 17 实源对齐(Math 与 StrictMath 同签名共用 symbol,见头文件注):
//   - expm1/log1p/sinh/cosh/tanh/IEEEremainder:fdlibm 5.3 算法结构移植
//     (s_expm1/s_log1p/e_sinh/e_cosh/s_tanh/e_remainder + e_fmod 依赖),
//     位型操作全部经上方 memcpy 助手(免别名 UB,字节序无关)。
//   - copySign/toDegrees/toRadians/fma/ulp/getExponent/nextAfter/nextUp/
//     nextDown/scalb:java.lang.Math JDK 17 实源逐式移植(StrictMath 侧
//     JDK 17 即转委派 Math,同 symbol 位精确)。
//   - 32 位整型上的移位/回绕(如 k<<20 指数加减)先转 uint32 再运算,
//     规避 C 负数左移 UB;uint32 加法回绕语义与 fdlibm __HI(x) += 一致。

static uint32_t mj2x_sm_f32_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float mj2x_sm_f32_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint64_t mj2x_sm_f64_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double mj2x_sm_f64_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* JDK 17 powerOfTwoD(n):正常区 2^n(阶码位构造,n 在 [-1022, 1023])。 */
static double mj2x_sm_pow2_f64(int32_t n)
{
    return mj2x_sm_f64_from_bits((uint64_t)(uint32_t)(n + 1023) << 52);
}

/* ---- expm1(fdlibm s_expm1.c):k*ln2+r 归约 + 5 阶有理近似 ---- */

double mj2x_strictmath_expm1(double x)
{
    static const double one         = 1.0;
    static const double huge        = 1.0e+300;
    static const double tiny        = 1.0e-300;
    static const double o_threshold = 7.09782712893383973096e+02; /* 0x40862E42, 0xFEFA39EF */
    static const double ln2_hi      = 6.93147180369123816490e-01; /* 0x3fe62e42, 0xfee00000 */
    static const double ln2_lo      = 1.90821492927058770002e-10; /* 0x3dea39ef, 0x35793c76 */
    static const double invln2      = 1.44269504088896338700e+00; /* 0x3ff71547, 0x652b82fe */
    /* 缩放系数(Qi*2^i,z 取 x^2/2) */
    static const double Q1 = -3.33333333333331316428e-02; /* BFA11111 111110F4 */
    static const double Q2 =  1.58730158725481460165e-03; /* 3F5A01A0 19FE5585 */
    static const double Q3 = -7.93650757867487942473e-05; /* BF14CE19 9EAADBB7 */
    static const double Q4 =  4.00821782732936239552e-06; /* 3ED0CFCA 86E65239 */
    static const double Q5 = -2.01099218183624371326e-07; /* BE8AFDB7 6E09C32D */
    double   y, hi = 0.0, lo = 0.0, c = 0.0, t, e, hxs, hfx, r1;
    int32_t  k, xsb;
    uint32_t hx;

    hx  = mj2x_sm_hi(x);
    xsb = (int32_t)(hx & UINT32_C(0x80000000)); /* x 的符号位 */
    y   = (xsb == 0) ? x : -x;                  /* y = |x| */
    hx &= UINT32_C(0x7fffffff);                 /* |x| 高字 */
    /* 大参数与非有限值分流 */
    if (hx >= UINT32_C(0x4043687A)) {           /* |x| >= 56*ln2 */
        if (hx >= UINT32_C(0x40862E42)) {       /* |x| >= 709.78... */
            if (hx >= UINT32_C(0x7ff00000)) {
                if (((hx & UINT32_C(0xfffff)) | mj2x_sm_lo(x)) != 0) {
                    return x + x;               /* NaN */
                }
                return (xsb == 0) ? x : -1.0;   /* expm1(+-inf) = {+inf, -1} */
            }
            if (x > o_threshold) return huge * huge; /* 溢出 → +Inf */
        }
        if (xsb != 0) {                         /* x < -56*ln2 → -1.0(带 inexact) */
            if (x + tiny < 0.0) return tiny - one;
        }
    }
    /* 参数归约 */
    if (hx > UINT32_C(0x3fd62e42)) {            /* |x| > 0.5 ln2 */
        if (hx < UINT32_C(0x3FF0A2B2)) {        /* |x| < 1.5 ln2 */
            if (xsb == 0) {
                hi = x - ln2_hi;
                lo = ln2_lo;
                k  = 1;
            }
            else {
                hi = x + ln2_hi;
                lo = -ln2_lo;
                k  = -1;
            }
        }
        else {
            k  = (int32_t)(invln2 * x + ((xsb == 0) ? 0.5 : -0.5));
            t  = (double)k;
            hi = x - t * ln2_hi;                /* t*ln2_hi 精确 */
            lo = t * ln2_lo;
        }
        x  = hi - lo;
        c  = (hi - x) - lo;
    }
    else if (hx < UINT32_C(0x3c900000)) {       /* |x| < 2^-54 → 原样(带 inexact) */
        t = huge + x;
        return x - (t - (huge + x));
    }
    else {
        k = 0;
    }
    /* 主区间有理近似 */
    hfx = 0.5 * x;
    hxs = x * hfx;
    r1  = one + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    t   = 3.0 - r1 * hfx;
    e   = hxs * ((r1 - t) / (6.0 - x * t));
    if (k == 0) return x - (x * e - hxs);       /* c 为 0 */
    e  = (x * (e - c) - c);
    e -= hxs;
    if (k == -1) return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return one + 2.0 * (x - e);
    }
    if (k <= -2 || k > 56) {                    /* 直接 2^k*(exp(r))-1 */
        y = one - (e - x);
        mj2x_sm_set_hi(&y, mj2x_sm_hi(y) + ((uint32_t)(int32_t)k << 20));
        return y - one;
    }
    t = one;
    if (k < 20) {
        mj2x_sm_set_hi(&t, UINT32_C(0x3ff00000) - (UINT32_C(0x200000) >> k)); /* t = 1-2^-k */
        y = t - (e - x);
        mj2x_sm_set_hi(&y, mj2x_sm_hi(y) + ((uint32_t)(int32_t)k << 20));
    }
    else {
        mj2x_sm_set_hi(&t, (uint32_t)(0x3ff - k) << 20); /* 2^-k */
        y = x - (e + t);
        y += one;
        mj2x_sm_set_hi(&y, mj2x_sm_hi(y) + ((uint32_t)(int32_t)k << 20));
    }
    return y;
}

/* ---- log1p(fdlibm s_log1p.c):1+x=2^k*(1+f) 归约 + s=f/(2+f) 级数 ---- */

double mj2x_strictmath_log1p(double x)
{
    static const double ln2_hi = 6.93147180369123816490e-01; /* 3fe62e42 fee00000 */
    static const double ln2_lo = 1.90821492927058770002e-10; /* 3dea39ef 35793c76 */
    static const double two54  = 1.80143985094819840000e+16; /* 43500000 00000000 */
    static const double Lp1    = 6.666666666666735130e-01;   /* 3FE55555 55555593 */
    static const double Lp2    = 3.999999999940941908e-01;   /* 3FD99999 9997FA04 */
    static const double Lp3    = 2.857142874366239149e-01;   /* 3FD24924 94229359 */
    static const double Lp4    = 2.222219843214978396e-01;   /* 3FCC71C5 1D8E78AF */
    static const double Lp5    = 1.818357216161805012e-01;   /* 3FC74664 96CB03DE */
    static const double Lp6    = 1.531383769920937332e-01;   /* 3FC39A09 D078C69F */
    static const double Lp7    = 1.479819860511658591e-01;   /* 3FC2F112 DF3E5244 */
    static double       zero   = 0.0;                        /* 防-1/0 常量折叠 */
    double   hfsq, f, c = 0.0, s, z, R, u;
    int32_t  k, hx, hu = 0, ax;

    hx = (int32_t)mj2x_sm_hi(x); /* 高字(带符号,负数走 int 比较分支) */
    ax = hx & 0x7fffffff;
    k  = 1;
    if (hx < (int32_t)UINT32_C(0x3FDA827A)) {   /* x < 0.41422 */
        if (ax >= (int32_t)UINT32_C(0x3ff00000)) { /* x <= -1.0 */
            if (x == -1.0) return -two54 / zero;   /* log1p(-1) = -Inf */
            return (x - x) / (x - x);              /* log1p(x<-1) = NaN */
        }
        if (ax < (int32_t)UINT32_C(0x3e200000)) {  /* |x| < 2^-29 */
            if (two54 + x > zero && ax < (int32_t)UINT32_C(0x3c900000)) {
                return x;                          /* |x| < 2^-54 原样 */
            }
            return x - x * x * 0.5;
        }
        if (hx > 0 || hx <= (int32_t)UINT32_C(0xbfd2bec3)) { /* -0.2929<x 或 x<=-0.2929 */
            k  = 0;
            f  = x;
            hu = 1;
        }
    }
    else {
        if (hx >= (int32_t)UINT32_C(0x7ff00000)) return x + x; /* NaN/+Inf */
    }
    if (k != 0) {
        if (hx < (int32_t)UINT32_C(0x43400000)) {
            u  = 1.0 + x;
            hu = (int32_t)mj2x_sm_hi(u);
            k  = (hu >> 20) - 1023;
            c  = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0); /* 修正项 */
            c /= u;
        }
        else {
            u  = x;
            hu = (int32_t)mj2x_sm_hi(u);
            k  = (hu >> 20) - 1023;
            c  = 0;
        }
        hu &= 0x000fffff;
        if (hu < 0x6a09e) {
            mj2x_sm_set_hi(&u, (uint32_t)hu | UINT32_C(0x3ff00000)); /* 归一化 u */
        }
        else {
            k += 1;
            mj2x_sm_set_hi(&u, (uint32_t)hu | UINT32_C(0x3fe00000)); /* 归一化 u/2 */
            hu = (int32_t)((UINT32_C(0x00100000) - (uint32_t)hu) >> 2);
        }
        f = u - 1.0;
    }
    hfsq = 0.5 * f * f;
    if (hu == 0) {                              /* |f| < 2^-20 快路径 */
        if (f == zero) {
            if (k == 0) return zero;
            c += (double)k * ln2_lo;
            return (double)k * ln2_hi + c;
        }
        R = hfsq * (1.0 - 0.66666666666666666 * f);
        if (k == 0) return f - R;
        return (double)k * ln2_hi - ((R - ((double)k * ln2_lo + c)) - f);
    }
    s = f / (2.0 + f);
    z = s * s;
    R = z * (Lp1 + z * (Lp2 + z * (Lp3 + z * (Lp4 + z * (Lp5 + z * (Lp6 + z * Lp7))))));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return (double)k * ln2_hi - ((hfsq - (s * (hfsq + R) + ((double)k * ln2_lo + c))) - f);
}

/* ---- sinh(fdlibm e_sinh.c):expm1/exp/溢出阈值分段 ---- */

double mj2x_strictmath_sinh(double x)
{
    static const double one   = 1.0;
    static const double shuge = 1.0e307;
    double   t, w, h;
    int32_t  ix, jx;
    uint32_t lx;

    jx = (int32_t)mj2x_sm_hi(x);
    ix = jx & 0x7fffffff;
    if (ix >= (int32_t)UINT32_C(0x7ff00000)) return x + x; /* NaN → NaN,±Inf → ±Inf */
    h = 0.5;
    if (jx < 0) h = -h;
    if (ix < (int32_t)UINT32_C(0x40360000)) {   /* |x| < 22 */
        if (ix < (int32_t)UINT32_C(0x3e300000)) { /* |x| < 2^-28 */
            if (shuge + x > one) return x;        /* sinh(tiny) = tiny(带 inexact) */
        }
        t = mj2x_strictmath_expm1(fabs(x));
        if (ix < (int32_t)UINT32_C(0x3ff00000)) return h * (2.0 * t - t * t / (t + one));
        return h * (t + t / (t + one));
    }
    if (ix < (int32_t)UINT32_C(0x40862E42)) {   /* |x| < 709.78 */
        return h * mj2x_strictmath_exp(fabs(x));
    }
    /* [709.78, 710.43..):exp(|x|) 溢出但 sinh 半积仍有限 */
    lx = mj2x_sm_lo(x);
    if (ix < (int32_t)UINT32_C(0x408633CE) ||
        (ix == (int32_t)UINT32_C(0x408633ce) && lx <= UINT32_C(0x8fb9f87d))) {
        w = mj2x_strictmath_exp(0.5 * fabs(x));
        t = h * w;
        return t * w;
    }
    return x * shuge;                           /* 溢出 → ±Inf */
}

/* ---- cosh(fdlibm e_cosh.c):expm1/exp/溢出阈值分段 ---- */

double mj2x_strictmath_cosh(double x)
{
    static const double one  = 1.0;
    static const double half = 0.5;
    static const double huge = 1.0e300;
    double   t, w;
    int32_t  ix;
    uint32_t lx;

    ix = (int32_t)mj2x_sm_hi(x) & 0x7fffffff;
    if (ix >= (int32_t)UINT32_C(0x7ff00000)) return x * x; /* NaN → NaN,±Inf → +Inf */
    if (ix < (int32_t)UINT32_C(0x3fd62e43)) {   /* |x| < 0.5*ln2 */
        t = mj2x_strictmath_expm1(fabs(x));
        w = one + t;
        if (ix < (int32_t)UINT32_C(0x3c800000)) return w; /* cosh(tiny) = 1 */
        return one + (t * t) / (w + w);
    }
    if (ix < (int32_t)UINT32_C(0x40360000)) {   /* |x| < 22 */
        t = mj2x_strictmath_exp(fabs(x));
        return half * t + half / t;
    }
    if (ix < (int32_t)UINT32_C(0x40862E42)) {   /* |x| < 709.78 */
        return half * mj2x_strictmath_exp(fabs(x));
    }
    lx = mj2x_sm_lo(x);
    if (ix < (int32_t)UINT32_C(0x408633CE) ||
        (ix == (int32_t)UINT32_C(0x408633ce) && lx <= UINT32_C(0x8fb9f87d))) {
        w = mj2x_strictmath_exp(half * fabs(x));
        t = half * w;
        return t * w;
    }
    return huge * huge;                         /* 溢出 → +Inf */
}

/* ---- tanh(fdlibm s_tanh.c):expm1(-2x)/(expm1(2x)) 分段 ---- */

double mj2x_strictmath_tanh(double x)
{
    static const double one  = 1.0;
    static const double two  = 2.0;
    static const double tiny = 1.0e-300;
    double  t, z;
    int32_t jx, ix;

    jx = (int32_t)mj2x_sm_hi(x);
    ix = jx & 0x7fffffff;
    if (ix >= (int32_t)UINT32_C(0x7ff00000)) {
        if (jx >= 0) return one / x + one;      /* tanh(+inf) = +1 */
        return one / x - one;                   /* tanh(NaN) = NaN */
    }
    if (ix < (int32_t)UINT32_C(0x40360000)) {   /* |x| < 22 */
        if (ix < (int32_t)UINT32_C(0x3c800000)) { /* |x| < 2^-55 */
            return x * (one + x);               /* tanh(small) = small */
        }
        if (ix >= (int32_t)UINT32_C(0x3ff00000)) { /* |x| >= 1 */
            t = mj2x_strictmath_expm1(two * fabs(x));
            z = one - two / (t + two);
        }
        else {
            t = mj2x_strictmath_expm1(-two * fabs(x));
            z = -t / (t + two);
        }
    }
    else {
        z = one - tiny;                         /* |x| > 22 → ±1(带 inexact) */
    }
    return (jx >= 0) ? z : -z;
}

/* ---- fmod(fdlibm e_fmod.c):移位相减精确余,IEEEremainder 依赖 ---- */

static double mj2x_sm_fmod(double x, double y)
{
    static const double kZero[2] = {0.0, -0.0};
    int32_t  n, hx, hy, hz, ix, iy;
    uint32_t lx, ly, lz, sx, i;

    hx = (int32_t)mj2x_sm_hi(x);
    lx = mj2x_sm_lo(x);
    hy = (int32_t)mj2x_sm_hi(y);
    ly = mj2x_sm_lo(y);
    sx = (uint32_t)hx & UINT32_C(0x80000000);   /* x 的符号位 */
    hx ^= (int32_t)sx;                          /* |x| 高字 */
    hy &= UINT32_C(0x7fffffff);                 /* |y| 高字 */
    /* 异常值:y=0、x 非有限、y 为 NaN → 0/0 型 NaN */
    if (((uint32_t)hy | ly) == 0 || hx >= (int32_t)UINT32_C(0x7ff00000) ||
        ((uint32_t)hy | ((ly | (0u - ly)) >> 31)) > UINT32_C(0x7ff00000)) {
        return (x * y) / (x * y);
    }
    if (hx <= hy) {
        if (hx < hy || lx < ly) return x;                 /* |x| < |y| 原样 */
        if (lx == ly) return kZero[sx >> 31];             /* |x| == |y| → 符号零 */
    }
    /* ilogb(x)：fdlibm 的 i 为带符号 int，靠符号位截断停止（迭代数 =
     * clz32(lx)）；C 有符号左移溢出 UB，改 uint32 计数 + 显式符号位判定，
     * 迭代数逐位一致。 */
    if (hx < (int32_t)UINT32_C(0x00100000)) {
        if (hx == 0) {
            for (ix = -1043, i = lx; i != 0 && (i & UINT32_C(0x80000000)) == 0; i <<= 1) ix -= 1;
        }
        else {
            for (ix = -1022, i = (uint32_t)hx << 11; i != 0 && (i & UINT32_C(0x80000000)) == 0; i <<= 1) ix -= 1;
        }
    }
    else {
        ix = (hx >> 20) - 1023;
    }
    /* ilogb(y) */
    if (hy < (int32_t)UINT32_C(0x00100000)) {
        if (hy == 0) {
            for (iy = -1043, i = ly; i != 0 && (i & UINT32_C(0x80000000)) == 0; i <<= 1) iy -= 1;
        }
        else {
            for (iy = -1022, i = (uint32_t)hy << 11; i != 0 && (i & UINT32_C(0x80000000)) == 0; i <<= 1) iy -= 1;
        }
    }
    else {
        iy = (hy >> 20) - 1023;
    }
    /* 对齐 {hx,lx} 与 {hy,ly} */
    if (ix >= -1022) {
        hx = (int32_t)(UINT32_C(0x00100000) | (UINT32_C(0x000fffff) & (uint32_t)hx));
    }
    else {                                      /* 次正规 x:移位到正规 */
        n = -1022 - ix;
        if (n <= 31) {
            hx = (int32_t)(((uint32_t)hx << n) | (lx >> (32 - n)));
            lx <<= n;
        }
        else {
            hx = (int32_t)(lx << (n - 32));
            lx = 0;
        }
    }
    if (iy >= -1022) {
        hy = (int32_t)(UINT32_C(0x00100000) | (UINT32_C(0x000fffff) & (uint32_t)hy));
    }
    else {
        n = -1022 - iy;
        if (n <= 31) {
            hy = (int32_t)(((uint32_t)hy << n) | (ly >> (32 - n)));
            ly <<= n;
        }
        else {
            hy = (int32_t)(ly << (n - 32));
            ly = 0;
        }
    }
    /* 定点移位相减 */
    n = ix - iy;
    while (n-- > 0) {
        hz = hx - hy;
        lz = lx - ly;
        if (lx < ly) hz -= 1;
        if (hz < 0) {
            hx = hx + hx + (int32_t)(lx >> 31);
            lx = lx + lx;
        }
        else {
            if ((hz | (int32_t)lz) == 0) return kZero[sx >> 31];
            hx = hz + hz + (int32_t)(lz >> 31);
            lx = lz + lz;
        }
    }
    hz = hx - hy;
    lz = lx - ly;
    if (lx < ly) hz -= 1;
    if (hz >= 0) {
        hx = hz;
        lx = lz;
    }
    if (((uint32_t)hx | lx) == 0) return kZero[sx >> 31];
    /* 还原浮点值并恢复符号 */
    while (hx < (int32_t)UINT32_C(0x00100000)) { /* 归一化 */
        hx = hx + hx + (int32_t)(lx >> 31);
        lx = lx + lx;
        iy -= 1;
    }
    if (iy >= -1022) {                          /* 正常输出 */
        hx = (int32_t)(((uint32_t)(hx - 0x00100000)) | ((uint32_t)(iy + 1023) << 20));
        mj2x_sm_set_hi(&x, (uint32_t)hx | sx);
        mj2x_sm_set_lo(&x, lx);
    }
    else {                                      /* 次正规输出 */
        n = -1022 - iy;
        if (n <= 20) {
            lx = (lx >> n) | ((uint32_t)hx << (32 - n));
            hx >>= n;
        }
        else if (n <= 31) {
            lx = ((uint32_t)hx << (32 - n)) | (lx >> n);
            hx = (int32_t)sx;
        }
        else {
            lx = (uint32_t)hx >> (n - 32);
            hx = (int32_t)sx;
        }
        mj2x_sm_set_hi(&x, (uint32_t)hx | sx);
        mj2x_sm_set_lo(&x, lx);
        x *= 1.0;                               /* 产生必要 inexact 信号 */
    }
    return x;
}

/* ---- IEEEremainder(fdlibm e_remainder.c):x REM p(最近偶数商) ---- */

double mj2x_strictmath_ieee_remainder(double x, double p)
{
    int32_t  hx, hp;
    uint32_t sx, lx, lp;
    double   p_half;

    hx = (int32_t)mj2x_sm_hi(x);
    lx = mj2x_sm_lo(x);
    hp = (int32_t)mj2x_sm_hi(p);
    lp = mj2x_sm_lo(p);
    sx = (uint32_t)hx & UINT32_C(0x80000000);
    hp &= (int32_t)UINT32_C(0x7fffffff);
    hx &= (int32_t)UINT32_C(0x7fffffff);
    /* 异常值:p=0、x 非有限、p 为 NaN → 0/0 型 NaN */
    if ((hp == 0 && lp == 0) || hx >= (int32_t)UINT32_C(0x7ff00000) ||
        (hp >= (int32_t)UINT32_C(0x7ff00000) &&
         (((uint32_t)hp - UINT32_C(0x7ff00000)) | lp) != 0)) {
        return (x * p) / (x * p);
    }
    if (hp <= (int32_t)UINT32_C(0x7fdfffff)) x = mj2x_sm_fmod(x, p + p); /* x < 2p */
    if ((((uint32_t)(hx - hp)) | (lx - lp)) == 0) return 0.0 * x; /* x==±p → 符号零 */
    x = fabs(x);
    p = fabs(p);
    if (hp < (int32_t)UINT32_C(0x00200000)) {   /* 次正规 p:倍加比较 */
        if (x + x > p) {
            x -= p;
            if (x + x >= p) x -= p;
        }
    }
    else {
        p_half = 0.5 * p;
        if (x > p_half) {
            x -= p;
            if (x >= p_half) x -= p;
        }
    }
    mj2x_sm_set_hi(&x, mj2x_sm_hi(x) ^ sx);     /* 恢复被除数符号 */
    return x;
}

/* ---- copySign(JDK 17 Math 位操作式:sign 位直取) ---- */

double mj2x_strictmath_copy_sign_f64(double magnitude, double sign)
{
    /* 与静态助手 mj2x_sm_copysign 同式(SIGN_BIT_MASK 取 sign、其余取
     * magnitude);NaN sign 的位型残留口径见头文件注。 */
    return mj2x_sm_copysign(magnitude, sign);
}

float mj2x_strictmath_copy_sign_f32(float magnitude, float sign)
{
    const uint32_t mb = mj2x_sm_f32_bits(magnitude);
    const uint32_t sb = mj2x_sm_f32_bits(sign);
    return mj2x_sm_f32_from_bits((sb & UINT32_C(0x80000000)) | (mb & UINT32_C(0x7FFFFFFF)));
}

/* ---- toDegrees/toRadians(JDK 17 常量乘式,StrictMath 委派 Math) ---- */

double mj2x_strictmath_to_degrees(double angrad)
{
    /* JDK 17 Math.RADIANS_TO_DEGREES = 57.29577951308232(180/pi 双舍入)。 */
    return angrad * 57.29577951308232;
}

double mj2x_strictmath_to_radians(double angdeg)
{
    /* JDK 17 Math.DEGREES_TO_RADIANS = 0.017453292519943295(pi/180 双舍入)。 */
    return angdeg * 0.017453292519943295;
}

/* ---- fma(C99 fma/fmaf:单次舍入,HotSpot vfmadd 同语义) ---- */

double mj2x_strictmath_fma_f64(double a, double b, double c)
{
    return fma(a, b, c);
}

float mj2x_strictmath_fma_f32(float a, float b, float c)
{
    return fmaf(a, b, c);
}

/* ---- ulp(JDK 17 switch 公式) ---- */

double mj2x_strictmath_ulp_f64(double d)
{
    /* JDK 17 Math.ulp(double):NaN/±Inf→|d|;0/次正规→MIN_VALUE;
     * 其余 2^(exp-52)(次正规结果按位型 1<<(exp-MIN_SUB_EXPONENT))。 */
    int32_t exp = (int32_t)((mj2x_sm_hi(d) & UINT32_C(0x7FF00000)) >> 20) - 1023;
    if (exp == 1024) return fabs(d);            /* NaN 或 ±Inf */
    if (exp == -1023) return mj2x_sm_f64_from_bits(UINT64_C(1)); /* 0/次正规 → MIN_VALUE */
    exp -= 52;                                  /* SIGNIFICAND_WIDTH-1 */
    if (exp >= -1022) return mj2x_sm_pow2_f64(exp);
    return mj2x_sm_f64_from_bits((uint64_t)1u << (uint32_t)(exp + 1074)); /* 次正规 */
}

float mj2x_strictmath_ulp_f32(float f)
{
    int32_t exp = (int32_t)((mj2x_sm_f32_bits(f) & UINT32_C(0x7F800000)) >> 23) - 127;
    if (exp == 128) return fabsf(f);            /* NaN 或 ±Inf */
    if (exp == -127) return mj2x_sm_f32_from_bits(1u); /* 0/次正规 → Float.MIN_VALUE */
    exp -= 23;
    if (exp >= -125) return mj2x_sm_f32_from_bits((uint32_t)(exp + 127) << 23);
    return mj2x_sm_f32_from_bits(1u << (uint32_t)(exp + 149));
}

/* ---- getExponent(阶码位直取 - bias) ---- */

int32_t mj2x_strictmath_get_exponent_f64(double d)
{
    return (int32_t)((mj2x_sm_hi(d) & UINT32_C(0x7FF00000)) >> 20) - 1023;
}

int32_t mj2x_strictmath_get_exponent_f32(float f)
{
    return (int32_t)((mj2x_sm_f32_bits(f) & UINT32_C(0x7F800000)) >> 23) - 127;
}

/* ---- nextAfter/nextUp/nextDown(JDK 17 位型邻域) ---- */

double mj2x_strictmath_next_after_f64(double start, double direction)
{
    /* IEEE 位型按有符号幅值整数字典序:负侧整数表示递减即向零方向,
     * 下降分支 transducer>0?-1:+1、上升分支 transducer>=0?+1:-1。 */
    if (start > direction) {                    /* 下降 */
        if (start != 0.0) {
            const int64_t transducer = (int64_t)mj2x_sm_f64_bits(start);
            return mj2x_sm_f64_from_bits(
                (uint64_t)(transducer + ((transducer > 0) ? INT64_C(-1) : INT64_C(1))));
        }
        /* start == 0.0 且 direction < 0.0 */
        return mj2x_sm_f64_from_bits(UINT64_C(0x8000000000000001)); /* -MIN_VALUE */
    }
    if (start < direction) {                    /* 上升(+0.0 归一 -0.0) */
        const int64_t transducer = (int64_t)mj2x_sm_f64_bits(start + 0.0);
        return mj2x_sm_f64_from_bits(
            (uint64_t)(transducer + ((transducer >= 0) ? INT64_C(1) : INT64_C(-1))));
    }
    if (start == direction) return direction;
    return start + direction;                   /* 任一入参 NaN:JDK 即 start+direction */
}

double mj2x_strictmath_next_up_f64(double d)
{
    if (d < HUGE_VAL) {                         /* NaN/+Inf 落入 else 原样返回 */
        const int64_t transducer = (int64_t)mj2x_sm_f64_bits(d + 0.0); /* 归一 -0.0 */
        return mj2x_sm_f64_from_bits(
            (uint64_t)(transducer + ((transducer >= 0) ? INT64_C(1) : INT64_C(-1))));
    }
    return d;
}

float mj2x_strictmath_next_up_f32(float f)
{
    if (f < HUGE_VALF) {
        const int32_t transducer = (int32_t)mj2x_sm_f32_bits(f + 0.0f);
        return mj2x_sm_f32_from_bits((uint32_t)(transducer + ((transducer >= 0) ? 1 : -1)));
    }
    return f;
}

double mj2x_strictmath_next_down_f64(double d)
{
    if (isnan(d) || d == -HUGE_VAL) return d;
    if (d == 0.0) return mj2x_sm_f64_from_bits(UINT64_C(0x8000000000000001)); /* -MIN_VALUE */
    {
        const int64_t transducer = (int64_t)mj2x_sm_f64_bits(d);
        return mj2x_sm_f64_from_bits(
            (uint64_t)(transducer + ((d > 0.0) ? INT64_C(-1) : INT64_C(1))));
    }
}

float mj2x_strictmath_next_down_f32(float f)
{
    if (isnan(f) || f == -HUGE_VALF) return f;
    if (f == 0.0f) return mj2x_sm_f32_from_bits(UINT32_C(0x80000001)); /* -MIN_VALUE */
    {
        const int32_t transducer = (int32_t)mj2x_sm_f32_bits(f);
        return mj2x_sm_f32_from_bits((uint32_t)(transducer + ((f > 0.0f) ? -1 : 1)));
    }
}

/* ---- scalb(JDK 17 乘法步进式) ---- */

double mj2x_strictmath_scalb_f64(double d, int32_t scale_factor)
{
    /* JDK 17 Math.scalb(double,int):MAX_SCALE = MAX_EXPONENT + -MIN_EXPONENT
     * + SIGNIFICAND_WIDTH + 1 = 2099;先 2^(scale mod 512) 单乘再 2^±512
     * 步进,乘法次序保证缩下方向至多一次真实舍入(次正规只在单乘出现)。 */
    static const int32_t kMaxScale = 1023 + 1022 + 53 + 1;
    int32_t exp_adjust;
    int32_t scale_increment;
    double  exp_delta;
    if (scale_factor < 0) {
        if (scale_factor < -kMaxScale) scale_factor = -kMaxScale;
        scale_increment = -512;
        exp_delta = mj2x_sm_f64_from_bits(UINT64_C(0x1FF0000000000000)); /* 2^-512 */
    }
    else {
        if (scale_factor > kMaxScale) scale_factor = kMaxScale;
        scale_increment = 512;
        exp_delta = mj2x_sm_f64_from_bits(UINT64_C(0x5FF0000000000000)); /* 2^512 */
    }
    /* scale mod 512(带符号余数,JDK 17 用 (scale>>8)>>>23 移位技巧求同值,
     * 本夹取区间 [-2099, 2099] 内 C99 % 与之逐位等价)。 */
    exp_adjust = scale_factor % 512;
    d *= mj2x_sm_f64_from_bits((uint64_t)(uint32_t)(exp_adjust + 1023) << 52);
    scale_factor -= exp_adjust;
    while (scale_factor != 0) {
        d *= exp_delta;
        scale_factor -= scale_increment;
    }
    return d;
}

float mj2x_strictmath_scalb_f32(float f, int32_t scale_factor)
{
    /* JDK 17 Math.scalb(float,int):MAX_SCALE = 127 + 125 + 24 + 1 = 277;
     * float→double 精确加宽 + 2^scale 精确乘 + 单次 double→float 舍入。 */
    static const int32_t kMaxScale = 127 + 125 + 24 + 1;
    if (scale_factor > kMaxScale) scale_factor = kMaxScale;
    if (scale_factor < -kMaxScale) scale_factor = -kMaxScale;
    return (float)((double)f * mj2x_sm_f64_from_bits(
        (uint64_t)(uint32_t)(scale_factor + 1023) << 52));
}

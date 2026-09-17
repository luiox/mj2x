#pragma once

// VMC4 运行时 ABI(#894 运行时/模板分离下沉)。
//
// 解释器生成代码只含方法级结构(常量池/字节码数组、dispatcher、handler);
// 本头提供跨 TU 固定的类型与 JVM 语义 helper 声明。实现位于
// mj2x_vmc4_runtime.c(mj2x-rt 编译),生成工程经编译链接消费,
// 不再由发射侧逐 TU 内联。

#include <jni.h>

#include <stddef.h>
#include <stdint.h>

#ifndef MJ2X_VMC4_API
#define MJ2X_VMC4_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 寄存器槽联合体 ----
// i 用于 I32/I64(I32 写入时由 handler 截断到 32 位),f 用于 F32/F64(统一用
// double)。ref 用 void* 而非 jobject 保持头自包含语义(存取时双向 cast)。
typedef union mj2x_vm_slot {
    int64_t i;
    double  f;
    void*   ref;
} mj2x_vm_slot;

// Call helper 函数指针类型:解释器经函数指针表分派到 per-callsite 的 JNI
// helper。参数:(env, regs, recv, argIdxArr, argc, dest)。
typedef void (*mj2x_vm_call_fn)(JNIEnv*, mj2x_vm_slot*, unsigned int, unsigned int*, unsigned char,
                                unsigned int);

// ---- JVM 整数除法语义(INT_MIN / -1 不抛,回绕) ----
MJ2X_VMC4_API int32_t mj2x_vm_div_i32(int32_t a, int32_t b);
MJ2X_VMC4_API int64_t mj2x_vm_div_i64(int64_t a, int64_t b);
MJ2X_VMC4_API int32_t mj2x_vm_rem_i32(int32_t a, int32_t b);
MJ2X_VMC4_API int64_t mj2x_vm_rem_i64(int64_t a, int64_t b);

// ---- JVM f2i/f2l 饱和转换(NaN->0,向零截断,超界饱和) ----
MJ2X_VMC4_API int32_t mj2x_vm_f2i32(double v);
MJ2X_VMC4_API int64_t mj2x_vm_f2i64(double v);

// ---- 异常抛出与匹配 ----
MJ2X_VMC4_API void mj2x_vm_throw_npe(JNIEnv* env, const char* msg);
MJ2X_VMC4_API void mj2x_vm_throw(JNIEnv* env, void* value);
MJ2X_VMC4_API void mj2x_vm_throw_arith(JNIEnv* env);
MJ2X_VMC4_API int32_t mj2x_vm_exception_matches(JNIEnv* env, jobject ex, const char* caught_type);

// ---- 程序 blob at-rest 解密（issue #760 首切片） ----
// 发射期（mj2x 侧）以 xorshift32 密钥流逐字节 XOR 加密 opcode 字节流数据段；
// 本函数在生成代码的 JNI_OnLoad（库加载期，先于任何 native 调用）被调用，
// 对 blob 原地一次性解密。密钥流与打包侧 mj2x-core native_lib_crypto.hpp 的
// xorStreamBytes 逐字节一致（每字节先推进 state 再取 state & 0xFF 异或）；
// XOR 为对合运算，同一函数兼作加密/解密。key == 0 回退固定种子（与打包侧
// 约定一致；xorshift32 的 0 态是死锁态，密钥流会退化为全零）。
// 密钥材料用后擦除（#760 切片 2，对齐 jvmtp 1.6.4 密钥栈擦除）：返回前把
// keystream 状态与密钥参数 volatile 清零，不在栈上留密钥副本。
MJ2X_VMC4_API void mj2x_vm_blob_decrypt(unsigned char* data, unsigned int size, uint32_t key);

// ---- 分块懒解密（issue #760 切片 2，--vmc4-lazy-decrypt） ----
// 程序 blob 按指令对齐的 64 字节目标块独立加密（块边界只落在指令起点，
// 单条超长指令——如大 SWITCH——自成一块）；解释器执行到某块首次取码时才
// 原地解密该块，未走到的分支保持密文。块下标 c 的字节区间为
// [c == 0 ? 0 : ends[c-1], ends[c])，ends 单调不减。

// 分块密钥派生：blob_key ^ golden-ratio 扰动后经 4 轮 xorshift32 扩散，
// 结果恒非 0（0 是 xorshift32 死锁态）。与发射侧 vmc4DeriveChunkKey
// （mj2x-core vmc4_instance.cpp）同一公式双镜像，纯 u32 算术逐位一致。
MJ2X_VMC4_API uint32_t mj2x_vm_chunk_key(uint32_t blob_key, uint32_t chunk_index);

// pc → 块下标：返回满足 pc < ends[c] 的最小 c（ends 单调）；pc 越界时
// 归末块（防御）。慢路径专用（块首次进入），线性扫描即可。
MJ2X_VMC4_API uint32_t mj2x_vm_chunk_locate(const uint32_t* ends, uint32_t n_chunks,
                                            uint32_t pc);

// 懒解密一块：state[c] == 1 视为已解密（快路径无锁）；否则进程级互斥锁内
// 解密 [begin, end) 并置 state[c] = 1——多 Java 线程首执行同一方法时只解
// 一次（XOR 对合，重复解密会还原成密文，必须防重入）。派生密钥解密完
// 立即 volatile 清零（密钥材料用后擦除）。state 由生成代码以 volatile
// unsigned char 数组持有（0 = 密文，1 = 已解密），跨线程的可见性由
// volatile 读写 + 锁内发布共同保证。
MJ2X_VMC4_API void mj2x_vm_lazy_decrypt(unsigned char* code, const uint32_t* ends,
                                        volatile unsigned char* state, uint32_t n_chunks,
                                        uint32_t blob_key, uint32_t chunk_index);

#ifdef __cplusplus
}
#endif

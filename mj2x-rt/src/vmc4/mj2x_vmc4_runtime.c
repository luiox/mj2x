/// @file mj2x_vmc4_runtime.c
/// @brief VMC4 运行时实现(#894 运行时/模板分离下沉)。
///
/// 原为 vm_interpreter_emit.cpp::emitRuntimePrelude 的逐 TU 内联字符串,
/// 现物理下沉 mj2x-rt,生成工程经编译链接消费。语义逐位不变。

#include "vmc4/mj2x_vmc4_runtime.h"

#include <limits.h>
#include <math.h>

// 懒解密互斥锁（#760 切片 2）：多个 Java 线程并发首执行同一方法时，同一块
// 只允许解密一次（XOR 对合，并发双重解密会把块还原成密文）。锁为进程级
// 单例、静态初始化（无生命周期管理）；慢路径（块首次解密）才加锁，稳态
// 零锁开销。平台面与 regc CRT（mj2x_regc_string_builder.c）同一套宏约定。
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
static SRWLOCK mj2x_vm_lazy_gate = SRWLOCK_INIT;
static void mj2x_vm_lazy_gate_lock(void)
{
    AcquireSRWLockExclusive(&mj2x_vm_lazy_gate);
}
static void mj2x_vm_lazy_gate_unlock(void)
{
    ReleaseSRWLockExclusive(&mj2x_vm_lazy_gate);
}
#else
#    ifndef _POSIX_C_SOURCE
#        define _POSIX_C_SOURCE 200809L
#    endif
#    include <pthread.h>
static pthread_mutex_t mj2x_vm_lazy_gate = PTHREAD_MUTEX_INITIALIZER;
static void mj2x_vm_lazy_gate_lock(void)
{
    (void)pthread_mutex_lock(&mj2x_vm_lazy_gate);
}
static void mj2x_vm_lazy_gate_unlock(void)
{
    (void)pthread_mutex_unlock(&mj2x_vm_lazy_gate);
}
#endif

// 密钥材料用后擦除（#760 切片 2，对齐 jvmtp 1.6.4 密钥栈擦除）：volatile
// 写阻止编译器把清零优化掉（死存储消除），密钥/keystream 不在栈上留副本。
static void mj2x_vm_scrub_u32(uint32_t* v)
{
    volatile uint32_t* p = v;
    *p = 0u;
}

// JVM 整数除法语义:INT_MIN / -1 不抛(回绕到 INT_MIN),其余交给硬件。
int32_t mj2x_vm_div_i32(int32_t a, int32_t b)
{
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}

int64_t mj2x_vm_div_i64(int64_t a, int64_t b)
{
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}

int32_t mj2x_vm_rem_i32(int32_t a, int32_t b)
{
    if (a == INT32_MIN && b == -1) return 0;
    return a % b;
}

int64_t mj2x_vm_rem_i64(int64_t a, int64_t b)
{
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

// JVM f2i/f2l 语义:NaN -> 0,向零截断;截断后超界才饱和(C cast 对超界/NaN
// 是 UB)。饱和点取 (double)INTxx_MAX + 1.0 = 2^31/2^63((double)INT64_MAX 因
// 53 位尾数本就舍入为 2^63),不用硬编码魔数。
int32_t mj2x_vm_f2i32(double v)
{
    if (v != v) return 0;
    if (v >= (double)INT32_MAX + 1.0) return INT32_MAX;
    if (v < (double)INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

int64_t mj2x_vm_f2i64(double v)
{
    if (v != v) return 0;
    if (v >= (double)INT64_MAX) return INT64_MAX;
    if (v < (double)INT64_MIN) return INT64_MIN;
    return (int64_t)v;
}

// ---- 单步二元运算（FusedBinary super-op 的两段共用，issue #9） ----
// 与解释器 BINARY handler 的分支逐条对应（同一份语义两个镜像，改其一必须同步
// 另一个）。整数路径的 I32 逐步截断是关键：未折叠形态下中间值先存 regs[t].i
// 再读，截断发生在存储点；折叠后中间值只活在调用方局部变量里，必须在 helper
// 内收窄，否则两步之间的位宽语义丢失。
int mj2x_vm_bin_step_i(JNIEnv* env, int64_t x, int64_t y, unsigned char op, unsigned char type,
                       int64_t* out)
{
    int64_t r = 0;
    switch (op) {
    case 0: r = (int64_t)((uint64_t)x + (uint64_t)y); break;
    case 1: r = (int64_t)((uint64_t)x - (uint64_t)y); break;
    case 2: r = (int64_t)((uint64_t)x * (uint64_t)y); break;
    case 3:
        if (y == 0) { mj2x_vm_throw_arith(env); return 1; }
        r = (type == 0) ? mj2x_vm_div_i32((int32_t)x, (int32_t)y) : mj2x_vm_div_i64(x, y);
        break;
    case 4:
        if (y == 0) { mj2x_vm_throw_arith(env); return 1; }
        r = (type == 0) ? mj2x_vm_rem_i32((int32_t)x, (int32_t)y) : mj2x_vm_rem_i64(x, y);
        break;
    case 5: r = x & y; break;
    case 6: r = x | y; break;
    case 7: r = x ^ y; break;
    case 8: r = (type == 0) ? (int32_t)((uint32_t)x << (y & 31))
                            : (int64_t)((uint64_t)x << (y & 63)); break;
    case 9: r = (type == 0) ? ((int32_t)x >> (y & 31)) : (x >> (y & 63)); break;
    case 10: r = (type == 0) ? (int32_t)((uint32_t)x >> (y & 31))
                             : (int64_t)((uint64_t)x >> (y & 63)); break;
    default: break;
    }
    *out = (type == 0) ? (int32_t)r : r;
    return 0;
}

// 浮点单步：F32（type == 2）走 float 精度运算并舍回 float（与 BINARY handler
// 一致——slot 是 double，不做 float 舍入会引入 double 中间精度，比较/再次运算
// 时差 1 ULP）；F64 直接 double。浮点运算不抛异常，恒返回 0。
int mj2x_vm_bin_step_f(JNIEnv* env, double x, double y, unsigned char op, unsigned char type,
                       double* out)
{
    (void)env;
    double r = 0;
    if (type == 2) {
        float fa = (float)x, fb = (float)y, fr = 0;
        switch (op) {
        case 0: fr = fa + fb; break;
        case 1: fr = fa - fb; break;
        case 2: fr = fa * fb; break;
        case 3: fr = fa / fb; break;
        case 4: fr = fmodf(fa, fb); break;
        default: break;
        }
        r = fr;
    } else {
        switch (op) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        case 4: r = fmod(x, y); break;
        default: break;
        }
    }
    *out = r;
    return 0;
}

// 抛 NullPointerException(数组/对象 null 访问)。pending exception 由调用方
// 检测并 return,让异常传播回 Java。
void mj2x_vm_throw_npe(JNIEnv* env, const char* msg)
{
    jclass _c = (*env)->FindClass(env, "java/lang/NullPointerException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, msg);
        (*env)->DeleteLocalRef(env, _c);
    }
}

// 抛已有异常对象(athrow):null -> NPE(JVM 语义)。
void mj2x_vm_throw(JNIEnv* env, void* value)
{
    if (value == NULL) {
        mj2x_vm_throw_npe(env, "athrow null");
        return;
    }
    (*env)->Throw(env, (jthrowable)value);
}

// 除零抛 ArithmeticException(JVM 语义)。
void mj2x_vm_throw_arith(JNIEnv* env)
{
    jclass _c = (*env)->FindClass(env, "java/lang/ArithmeticException");
    if (_c != NULL) {
        (*env)->ThrowNew(env, _c, "divide by zero");
        (*env)->DeleteLocalRef(env, _c);
    }
}

// 异常类型匹配:catch-all(caught_type 空/NULL)恒匹配;FindClass 失败(类型
// 不可解析)清 pending 并按不匹配处理(对齐 RegC 语义)。
int32_t mj2x_vm_exception_matches(JNIEnv* env, jobject ex, const char* caught_type)
{
    if (caught_type == NULL || caught_type[0] == '\0') return 1;
    jclass _c = (*env)->FindClass(env, caught_type);
    if (_c == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    int32_t _r = (*env)->IsInstanceOf(env, ex, _c) ? 1 : 0;
    (*env)->DeleteLocalRef(env, _c);
    return _r;
}

// ---------------------------------------------------------------------------
// 程序 blob at-rest 解密(issue #760 首切片)。
//
// 密钥流形态与 native lib 加密(mj2x-core native_lib_crypto.hpp / NativeLoader
// 字节码内实现)保持一致:xorshift32 逐步推进,每字节取 state & 0xFF 异或。
// 产物 C 源/二进制中只有密文 blob + 现场派生的 32 位 seed 常量,不出现
// 明文程序流,也不出现整块 key/keystream 数组(密钥流由循环现场重算)。
// ---------------------------------------------------------------------------

// xorshift32 一步推进(x ^= x<<13; x ^= x>>17; x ^= x<<5)。
static uint32_t mj2x_vm_blob_xorshift32(uint32_t state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void mj2x_vm_blob_decrypt(unsigned char* data, unsigned int size, uint32_t key)
{
    // key == 0 回退固定种子:0 是 xorshift32 的死锁态(密钥流恒 0,等价未加密)。
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    unsigned int i;
    for (i = 0; i < size; ++i) {
        state = mj2x_vm_blob_xorshift32(state);
        data[i] = (unsigned char)(data[i] ^ (unsigned char)(state & 0xFFu));
    }
    // 密钥材料用后擦除(#760 切片 2):keystream 状态与密钥参数清零后再返回,
    // 栈上不留可被内存取证捞到的密钥副本。
    mj2x_vm_scrub_u32(&state);
    mj2x_vm_scrub_u32(&key);
}

// ---------------------------------------------------------------------------
// 分块懒解密(issue #760 切片 2,--vmc4-lazy-decrypt)。
//
// 发射侧把 blob 按指令对齐的 64 字节目标块独立加密(块 key =
// mj2x_vm_chunk_key(blob_key, c),密钥流各自从块 key 起);解释器取码前经
// 生成代码的 mj2x_vm_lazy_touch_<tag> 调用本组函数,块首次进入才解密,
// 未走到的分支保持密文(内存 dump 只能拿到实际执行过的块,对齐 jvmtp
// 1.6.4 vm.lazy-decrypt 形态)。解密后的块保持明文不回擦:本 rt 的指令流
// 读取方式是 handler 内联直读 code[pc](无中央取译码步骤),回擦需要在
// 5 种 dispatcher 的每条边上插入重加密点并跟踪"块离开"事件(循环回边
// 使该事件不可判定),收益/风险不成比例——权衡记录见切片 2 设计说明。
// ---------------------------------------------------------------------------

uint32_t mj2x_vm_chunk_key(uint32_t blob_key, uint32_t chunk_index)
{
    // golden-ratio 扰动把块下标散进密钥空间,再 4 轮 xorshift32 扩散;
    // 纯 u32 算术(C 标准定义的回绕),与发射侧 vmc4DeriveChunkKey 逐位一致。
    uint32_t s = blob_key ^ (0x9E3779B9u + 0x9E3779B9u * (chunk_index + 1u));
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    // 0 是 xorshift32 死锁态(密钥流恒 0 = 未加密),回退固定种子。
    return s != 0u ? s : 0x9E3779B9u;
}

uint32_t mj2x_vm_chunk_locate(const uint32_t* ends, uint32_t n_chunks, uint32_t pc)
{
    uint32_t c;
    for (c = 0; c < n_chunks; ++c) {
        if (pc < ends[c]) return c;
    }
    // 越界 pc(仅解释器不变量被破坏时可达)归末块,不越界读。
    return n_chunks != 0u ? n_chunks - 1u : 0u;
}

void mj2x_vm_lazy_decrypt(unsigned char* code, const uint32_t* ends,
                          volatile unsigned char* state, uint32_t n_chunks,
                          uint32_t blob_key, uint32_t chunk_index)
{
    if (code == NULL || ends == NULL || state == NULL || chunk_index >= n_chunks) return;
    // 快路径:已解密,无锁返回。state 为 volatile 数组,每次派发生效。
    if (state[chunk_index] == 1u) return;
    mj2x_vm_lazy_gate_lock();
    // 双重检查:等锁期间其他线程可能已完成本块解密。
    if (state[chunk_index] != 1u) {
        const uint32_t begin = (chunk_index != 0u) ? ends[chunk_index - 1u] : 0u;
        const uint32_t end   = ends[chunk_index];
        if (begin < end) {
            uint32_t k = mj2x_vm_chunk_key(blob_key, chunk_index);
            mj2x_vm_blob_decrypt(code + begin, end - begin, k);
            // 派生密钥用后擦除:密钥只在本地变量里活到解密调用返回。
            mj2x_vm_scrub_u32(&k);
        }
        state[chunk_index] = 1u;   /* 锁内发布:解密完成后才对其他线程可见 */
    }
    mj2x_vm_lazy_gate_unlock();
}

// ---------------------------------------------------------------------------
// 块粒度链式懒解密(issue #1116 切片 B,--vmc4-block-lazy-decrypt)。
//
// 发射侧把 blob 按 CFG 块粒度独立加密(块区间由 blockTargets 划界,块 key =
// mj2x_vm_block_chain_key 链式派生:首块 k_0 = F(seed,0,seed),其后
// k_c = F(seed,c,k_{c-1}));解释器取码前经生成代码的 mj2x_vm_block_touch_<tag>
// 调用本组函数,块首次进入才解密,未走到的块保持密文(内存 dump 只能拿到
// 实际执行过的块,对齐 jvmtp 1.6.8 vm.control-flow-flatten 的 first-arrival
// decrypt 口径)。与 --vmc4-lazy-decrypt(64 字节网格档)互斥,两档不共存。
// 解密后的块保持明文不回擦:权衡同上节(循环回边使"块离开"事件不可判定)。
// 链上密钥材料不持久驻留:块 c 的密钥从 seed 逐块重导出(O(c) 次 4 轮
// xorshift32,u16 块数上界亦微秒级),链状态只在栈上瞬时存在,用后擦除。
// ---------------------------------------------------------------------------

uint32_t mj2x_vm_block_chain_key(uint32_t seed, uint32_t block_index, uint32_t prev_key)
{
    // golden-ratio 扰动项与前块密钥 XOR 后 4 轮 xorshift32 扩散;纯 u32 算术
    // (C 标准定义的回绕),与发射侧 vmc4DeriveBlockChainKey 逐位一致。
    uint32_t s = seed ^ prev_key ^ (0x9E3779B9u + 0x9E3779B9u * (block_index + 1u));
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    s = mj2x_vm_blob_xorshift32(s);
    // 0 是 xorshift32 死锁态(密钥流恒 0 = 未加密),回退固定种子。
    return s != 0u ? s : 0x9E3779B9u;
}

uint32_t mj2x_vm_block_locate(const uint32_t* targets, uint32_t n_blocks,
                              uint32_t total_bytes, uint32_t pc)
{
    for (uint32_t c = 0; c < n_blocks; ++c) {
        const uint32_t end = (c + 1u < n_blocks) ? targets[c + 1u] : total_bytes;
        if (pc < end) return c;
    }
    // 越界 pc(仅解释器不变量被破坏时可达)归末块,不越界读。
    return n_blocks != 0u ? n_blocks - 1u : 0u;
}

void mj2x_vm_block_lazy_decrypt(unsigned char* code, const uint32_t* targets,
                                volatile unsigned char* state, uint32_t n_blocks,
                                uint32_t total_bytes, uint32_t seed, uint32_t block_index)
{
    if (code == NULL || targets == NULL || state == NULL || block_index >= n_blocks) return;
    // 快路径:已解密,无锁返回。state 为 volatile 数组,每次派发生效。
    if (state[block_index] == 1u) return;
    mj2x_vm_lazy_gate_lock();
    // 双重检查:等锁期间其他线程可能已完成本块解密。
    if (state[block_index] != 1u) {
        const uint32_t begin = targets[block_index];
        const uint32_t end = (block_index + 1u < n_blocks) ? targets[block_index + 1u]
                                                           : total_bytes;
        if (begin < end) {
            // 从 seed 走链重导出 k_block_index:首块 prevKey = seed,链状态只在
            // 本地变量里瞬时存在,用后擦除(密钥材料不持久驻留)。
            uint32_t k = seed;
            for (uint32_t i = 0; i <= block_index; ++i) {
                k = mj2x_vm_block_chain_key(seed, i, k);
            }
            mj2x_vm_blob_decrypt(code + begin, end - begin, k);
            mj2x_vm_scrub_u32(&k);
        }
        state[block_index] = 1u;   /* 锁内发布:解密完成后才对其他线程可见 */
    }
    mj2x_vm_lazy_gate_unlock();
}

// VMC4 程序 blob at-rest 解密原语单测（issue #760 切片 1/2）。
//
// 由 mj2x_regc_crt_test.c 的 main 统一调用（单 runner 惯例）。
// 覆盖：roundtrip（独立密钥流加密 -> mj2x_vm_blob_decrypt 还原）、
// key==0 固定种子回退、错误密钥产出不同流、size=0 无操作；
// 切片 2 追加：分块密钥派生镜像对拍（mj2x_vm_chunk_key）、块定位
// （mj2x_vm_chunk_locate）、懒解密 round-trip（乱序 + 重复调用幂等，
// mj2x_vm_lazy_decrypt）。

#include "vmc4/mj2x_vmc4_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

// 独立实现的 xorshift32 密钥流（与打包侧 mj2x-core native_lib_crypto.hpp /
// 生成侧 mj2x_vm_blob_decrypt 同算法、独立成文交叉验证）。
static void test_side_xor_stream(unsigned char* data, unsigned int size, uint32_t key)
{
    uint32_t state = (key != 0u) ? key : 0x9E3779B9u;
    for (unsigned int i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = (unsigned char)(data[i] ^ (unsigned char)(state & 0xFFu));
    }
}

// 独立实现的分块密钥派生（与发射侧 vmc4DeriveChunkKey / CRT mj2x_vm_chunk_key
// 同公式、独立成文）：golden-ratio 扰动 + 4 轮 xorshift32，恒非 0。
static uint32_t test_side_chunk_key(uint32_t blob_key, uint32_t chunk_index)
{
    uint32_t s = blob_key ^ (0x9E3779B9u + 0x9E3779B9u * (chunk_index + 1u));
    for (int i = 0; i < 4; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
    }
    return s != 0u ? s : 0x9E3779B9u;
}

void mj2x_vmc4_blob_crypto_test_run(void)
{
    unsigned char blob[64];
    unsigned char backup[64];
    for (unsigned int i = 0; i < (unsigned int)sizeof(blob); ++i) {
        blob[i] = (unsigned char)(i * 7u + 3u);
    }
    memcpy(backup, blob, sizeof(blob));

    // roundtrip：独立密钥流加密 -> mj2x_vm_blob_decrypt 还原。
    test_side_xor_stream(blob, (unsigned int)sizeof(blob), 0x12345678u);
    assert(memcmp(blob, backup, sizeof(blob)) != 0);   /* 密文 != 明文 */
    mj2x_vm_blob_decrypt(blob, (unsigned int)sizeof(blob), 0x12345678u);
    assert(memcmp(blob, backup, sizeof(blob)) == 0);   /* 解密逐字节还原 */

    // XOR 对合：同一函数再跑一遍 = 再异或一次（加密方向等价）。
    mj2x_vm_blob_decrypt(blob, (unsigned int)sizeof(blob), 0x12345678u);
    test_side_xor_stream(blob, (unsigned int)sizeof(blob), 0x12345678u);
    assert(memcmp(blob, backup, sizeof(blob)) == 0);

    // key == 0 回退固定种子（与打包侧 xorStreamBytes 约定一致）。
    test_side_xor_stream(blob, (unsigned int)sizeof(blob), 0u);
    mj2x_vm_blob_decrypt(blob, (unsigned int)sizeof(blob), 0u);
    assert(memcmp(blob, backup, sizeof(blob)) == 0);

    // 错误密钥：解出的是不同字节流（首切片无校验和，不做显式 fail-closed
    // 检测；运行期由解释器 InvalidBytecode 防御停机隐式兜底）。
    test_side_xor_stream(blob, (unsigned int)sizeof(blob), 0x12345678u);
    mj2x_vm_blob_decrypt(blob, (unsigned int)sizeof(blob), 0x87654321u);
    assert(memcmp(blob, backup, sizeof(blob)) != 0);

    // size = 0 无操作（不越界、不崩溃）。
    mj2x_vm_blob_decrypt(blob, 0u, 0x12345678u);
    mj2x_vm_blob_decrypt(NULL, 0u, 0x12345678u);

    // ------------------------------------------------------------------
    // 切片 2：分块懒解密原语。
    // ------------------------------------------------------------------

    // 分块密钥派生：与独立镜像逐位一致；确定性；恒非 0；下标/主密钥敏感。
    for (uint32_t k = 0u; k < 8u; ++k) {
        for (uint32_t c = 0u; c < 8u; ++c) {
            const uint32_t got = mj2x_vm_chunk_key(k, c);
            assert(got == test_side_chunk_key(k, c));
            assert(got == mj2x_vm_chunk_key(k, c));   /* 确定性 */
            assert(got != 0u);
        }
        assert(mj2x_vm_chunk_key(k, 0u) != mj2x_vm_chunk_key(k, 1u));
        assert(mj2x_vm_chunk_key(k, 3u) != mj2x_vm_chunk_key(k ^ 0xA5A5A5A5u, 3u));
    }

    // 块定位：单调 ends 上的 lower-bound（pc < ends[c] 的最小 c）+ 越界归末块。
    {
        const uint32_t ends[4] = {64u, 128u, 192u, 200u};
        assert(mj2x_vm_chunk_locate(ends, 4u, 0u) == 0u);
        assert(mj2x_vm_chunk_locate(ends, 4u, 63u) == 0u);
        assert(mj2x_vm_chunk_locate(ends, 4u, 64u) == 1u);
        assert(mj2x_vm_chunk_locate(ends, 4u, 199u) == 3u);
        assert(mj2x_vm_chunk_locate(ends, 4u, 500u) == 3u);   /* 防御：归末块 */
        assert(mj2x_vm_chunk_locate(ends, 0u, 0u) == 0u);     /* 防御：空表 */
    }

    // 懒解密 round-trip：发射侧等价（独立镜像逐块加密）-> 乱序逐块懒解密 ->
    // 整流还原；同一块第二次调用必须幂等（state=1 短路，XOR 不重复施加）。
    {
        unsigned char stream[200];
        unsigned char plain[200];
        const uint32_t ends[3] = {64u, 128u, 200u};   /* 末块 72 字节（越目标块） */
        const uint32_t n = 3u;
        const uint32_t blob_key = 0x0BADC0DEu;
        for (unsigned int i = 0; i < (unsigned int)sizeof(stream); ++i) {
            plain[i] = (unsigned char)(i * 13u + 5u);
        }
        memcpy(stream, plain, sizeof(stream));
        /* 发射侧等价：逐块独立加密（块 c 用 chunk_key(blob_key, c) 起步）。 */
        uint32_t begin = 0u;
        for (uint32_t c = 0u; c < n; ++c) {
            test_side_xor_stream(stream + begin, ends[c] - begin, test_side_chunk_key(blob_key, c));
            begin = ends[c];
        }
        assert(memcmp(stream, plain, sizeof(stream)) != 0);

        /* 懒解密：乱序 2,0,1；末块块长超 64（指令对齐块允许超目标）。 */
        volatile unsigned char state[3] = {0, 0, 0};
        mj2x_vm_lazy_decrypt(stream, ends, state, n, blob_key, 2u);
        assert(memcmp(stream + ends[1], plain + ends[1], ends[2] - ends[1]) == 0);
        assert(memcmp(stream, plain, ends[1]) != 0);   /* 其余块仍密文 */
        mj2x_vm_lazy_decrypt(stream, ends, state, n, blob_key, 0u);
        mj2x_vm_lazy_decrypt(stream, ends, state, n, blob_key, 0u);   /* 幂等 */
        mj2x_vm_lazy_decrypt(stream, ends, state, n, blob_key, 1u);
        assert(memcmp(stream, plain, sizeof(stream)) == 0);

        /* 防御：越界块下标与 NULL state 不崩溃、不解密。 */
        mj2x_vm_lazy_decrypt(stream, ends, state, n, blob_key, 3u);
        mj2x_vm_lazy_decrypt(stream, ends, NULL, n, blob_key, 0u);
        assert(memcmp(stream, plain, sizeof(stream)) == 0);
    }
}

// VMC4 程序 blob at-rest 解密原语单测（issue #760 首切片）。
//
// 由 mj2x_regc_crt_test.c 的 main 统一调用（单 runner 惯例）。
// 覆盖：roundtrip（独立密钥流加密 -> mj2x_vm_blob_decrypt 还原）、
// key==0 固定种子回退、错误密钥产出不同流、size=0 无操作。

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
}
